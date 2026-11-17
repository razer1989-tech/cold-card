#!/usr/bin/env python3
"""atomrun.py -- userspace AtomBIOS command-table executor.

Faithful port of the Linux kernel Atom parser
(drivers/gpu/drm/amd/amdgpu/atom.c, opcode_table + operand semantics).

DRY mode (default): MM registers live in a shadow dict (reads return the last
written value, else 0). LIVE mode (--live): MM registers go through the card's
BAR5 (resource5 under /sys/bus/pci/devices/<bdf>/), exactly as the driver's
rmmio accesses do.

Usage: atomrun.py --rom egpu-rom.bin [--live] [--bdf 0000:44:00.0]
                  [--table 0] [--trace trace.txt] [--budget N]
"""
import sys, os, mmap, struct, time, argparse

M32 = 0xFFFFFFFF
ARG_MASK  = [0xFFFFFFFF,0xFFFF,0xFFFF00,0xFFFF0000,0xFF,0xFF00,0xFF0000,0xFF000000]
ARG_SHIFT = [0,0,8,16,0,8,16,24]
DEF_DST   = [0,0,1,2,0,1,2,3]
DST2SRC   = [[0,0,0,0],[1,2,3,0],[1,2,3,0],[1,2,3,0],[4,5,6,7],[4,5,6,7],[4,5,6,7],[4,5,6,7]]
REG,PS,WS,FB,ID,IMM,PLL,MC = 0,1,2,3,4,5,6,7
WS_QUOT,WS_REM,WS_DPTR,WS_SHIFT,WS_ORM,WS_ANDM,WS_FBWIN,WS_ATTR,WS_RPTR = \
    0x40,0x41,0x42,0x43,0x44,0x45,0x46,0x47,0x48
IIO_LEN = [1,2,3,3,3,3,4,4,4,3]
CASE_MAGIC, CASE_END = 0x63, 0x5A5A

_CLASSES = [REG,PS,WS,FB,PLL,MC]
def _fam(name, base):
    return {base+i: (name, c) for i, c in enumerate(_CLASSES)}
DISP = {}
DISP.update(_fam('move', 1));  DISP.update(_fam('and', 7));   DISP.update(_fam('or', 13))
DISP.update(_fam('shift_left', 19)); DISP.update(_fam('shift_right', 25))
DISP.update(_fam('mul', 31));  DISP.update(_fam('div', 37))
DISP.update(_fam('add', 43));  DISP.update(_fam('sub', 49))
DISP[55] = ('setport', 'ATI'); DISP[56] = ('setport', 'PCI'); DISP[57] = ('setport', 'SYSIO')
DISP[58] = ('setregblock', 0); DISP[59] = ('setfbbase', 0)
DISP.update(_fam('compare', 60)); DISP[66] = ('switch', 0)
for _i, _c in enumerate(['ALWAYS','EQUAL','BELOW','ABOVE','BELOWOREQUAL','ABOVEOREQUAL','NOTEQUAL']):
    DISP[67+_i] = ('jump', _c)
DISP.update(_fam('test', 74))
DISP[80] = ('delay', 'MS'); DISP[81] = ('delay', 'US')
DISP[82] = ('calltable', 0); DISP[83] = ('repeat', 0)
DISP.update(_fam('clear', 84)); DISP[90] = ('nop', 0); DISP[91] = ('eot', 0)
DISP.update(_fam('mask', 92)); DISP[98] = ('postcard', 0); DISP[99] = ('beep', 0)
DISP[100] = ('savereg', 0); DISP[101] = ('restorereg', 0); DISP[102] = ('setdatablock', 0)
DISP.update(_fam('xor', 103)); DISP.update(_fam('shl', 109)); DISP.update(_fam('shr', 115))
DISP[121] = ('debug', 0); DISP[122] = ('processds', 0)
DISP[123] = ('mul32', PS); DISP[124] = ('mul32', WS); DISP[125] = ('div32', PS); DISP[126] = ('div32', WS)

class Abort(Exception):
    pass

class ECtx:
    __slots__ = ('ps','ps_base','ps_size','ps_shift','ws','ws_size','start')

class Atom:
    def __init__(self, bios, live=False, bar5_path=None, trace=None, budget=5000000, max_writes=0):
        self.b = bios
        self.live = live
        self.tracef = trace
        self.budget = budget
        self.max_writes = max_writes
        self.shadow = {}
        self.n_writes = 0
        self.delay_ms = 0.0
        self.instr = 0
        self.notes = {}
        self.depth = 0
        h = self.u16(0x48)
        self.cmd_table = self.u16(h + 0x1E)
        self.data_table = self.u16(h + 0x20)
        self.reg_block = 0; self.data_block = 0; self.io_mode = 0
        self.fb_base = 0; self.divmul = [0, 0]; self.shift = 0; self.io_attr = 0
        self.cs_equal = False; self.cs_above = False
        self.scratch = [0] * 8192
        self.iio = [0] * 256
        p = self.u16(self.data_table + 0x32) + 4
        while p < len(self.b) and self.b[p] == 1:
            self.iio[self.b[p+1]] = p + 2
            p += 2
            while self.b[p] != 9:
                p += IIO_LEN[self.b[p]]
            p += 3
        self.mm = None
        if live:
            fd = os.open(bar5_path, os.O_RDWR)
            self.mm = mmap.mmap(fd, 0x40000, mmap.MAP_SHARED,
                                mmap.PROT_READ | mmap.PROT_WRITE)

    # ---- primitives -----------------------------------------------------
    def u16(self, o): return struct.unpack_from('<H', self.b, o)[0]
    def u32(self, o): return struct.unpack_from('<I', self.b, o)[0]
    def note(self, what):
        self.notes[what] = self.notes.get(what, 0) + 1

    # PCIE indirect window (dword idx of byte 0x38 / 0x3C): how the driver
    # reaches registers beyond BAR5's 256 KiB (amdgpu_mm_rreg/wreg ->
    # pcie_rreg/wreg with the BYTE address written to PCIE_INDEX).
    PCIE_INDEX = 0x0E
    PCIE_DATA = 0x0F

    def _mm_read(self, idx):
        return struct.unpack_from('<I', self.mm, idx*4)[0]

    def _mm_write(self, idx, val):
        struct.pack_into('<I', self.mm, idx*4, val & M32)

    def reg_read(self, idx):
        if self.live:
            if idx < 0x10000:
                return self._mm_read(idx)
            self._mm_write(self.PCIE_INDEX, idx*4)
            self._mm_read(self.PCIE_INDEX)  # flush, as the driver does
            return self._mm_read(self.PCIE_DATA)
        return self.shadow.get(idx, 0)

    def reg_write(self, idx, val):
        val &= M32
        if self.max_writes and self.n_writes >= self.max_writes:
            raise Abort('write cap %d reached' % self.max_writes)
        self.n_writes += 1
        if self.tracef:
            self.tracef.write("W 0x%05x = 0x%08x\n" % (idx*4, val))
        if self.live:
            if idx < 0x10000:
                self._mm_write(idx, val)
            else:
                self._mm_write(self.PCIE_INDEX, idx*4)
                self._mm_read(self.PCIE_INDEX)
                self._mm_write(self.PCIE_DATA, val)
        else:
            self.shadow[idx] = val

    def iio_execute(self, base, index, data):
        temp = 0xCDCDCDCD
        p = base
        while True:
            op = self.b[p]
            if op == 0:
                p += 1
            elif op == 2:
                temp = self.reg_read(self.u16(p+1)); p += 3
            elif op == 3:
                self.reg_write(self.u16(p+1), temp); p += 3
            elif op == 4:
                n, s = self.b[p+1], self.b[p+2]
                temp = (temp & (~(((M32 >> (32-n)) << s) & M32))) & M32; p += 3
            elif op == 5:
                n, s = self.b[p+1], self.b[p+2]
                temp = (temp | (((M32 >> (32-n)) << s) & M32)) & M32; p += 3
            elif op == 6:
                n, ss, sd = self.b[p+1], self.b[p+2], self.b[p+3]
                m = M32 >> (32-n)
                temp = ((temp & (~((m << sd) & M32))) | (((index >> ss) & m) << sd)) & M32; p += 4
            elif op == 8:
                n, ss, sd = self.b[p+1], self.b[p+2], self.b[p+3]
                m = M32 >> (32-n)
                temp = ((temp & (~((m << sd) & M32))) | (((data >> ss) & m) << sd)) & M32; p += 4
            elif op == 7:
                n, ss, sd = self.b[p+1], self.b[p+2], self.b[p+3]
                m = M32 >> (32-n)
                temp = ((temp & (~((m << sd) & M32))) | (((self.io_attr >> ss) & m) << sd)) & M32; p += 4
            elif op == 9:
                return temp
            else:
                return 0

    def pll_read(self, idx):
        if self.iio[2]: return self.iio_execute(self.iio[2], idx, 0)
        self.note('PLL read (no IIO prog)'); return 0
    def pll_write(self, idx, v):
        if self.iio[0x82]: self.iio_execute(self.iio[0x82], idx, v)
        else: self.note('PLL write (no IIO prog)')
    def mc_read(self, idx):
        if self.iio[3]: return self.iio_execute(self.iio[3], idx, 0)
        self.note('MC read (no IIO prog)'); return 0
    def mc_write(self, idx, v):
        if self.iio[0x83]: self.iio_execute(self.iio[0x83], idx, v)
        else: self.note('MC write (no IIO prog)')

    # ---- operand machinery (port of atom_get_src_int/get_src/get_dst/put_dst)
    def _operand(self, ec, a, attr, ptr):
        """Read one operand of class a; return its RAW (unshifted) value."""
        b = self.b
        if a == REG:
            idx = self.u16(ptr[0]); ptr[0] += 2
            idx += self.reg_block
            if self.io_mode == 0:
                return self.reg_read(idx)
            if self.io_mode in (1, 2):
                self.note('PCI/SYSIO read'); return 0
            prog = self.iio[self.io_mode & 0x7F]
            if not prog: self.note('IIO read: no prog'); return 0
            return self.iio_execute(prog, idx, 0)
        if a == PS:
            idx = b[ptr[0]]; ptr[0] += 1
            if idx >= ec.ps_size: self.note('PS read OOB'); return 0
            return ec.ps[ec.ps_base + idx]
        if a == WS:
            idx = b[ptr[0]]; ptr[0] += 1
            if idx == WS_QUOT: return self.divmul[0]
            if idx == WS_REM: return self.divmul[1]
            if idx == WS_DPTR: return self.data_block
            if idx == WS_SHIFT: return self.shift
            if idx == WS_ORM: return (1 << self.shift) & M32
            if idx == WS_ANDM: return (~(1 << self.shift)) & M32
            if idx == WS_FBWIN: return self.fb_base
            if idx == WS_ATTR: return self.io_attr
            if idx == WS_RPTR: return self.reg_block
            if idx < ec.ws_size: return ec.ws[idx]
            self.note('WS read OOB'); return 0
        if a == FB:
            idx = b[ptr[0]]; ptr[0] += 1
            pos = self.fb_base//4 + idx
            if 0 <= pos < len(self.scratch): return self.scratch[pos]
            self.note('FB read OOB'); return 0
        if a == ID:
            idx = self.u16(ptr[0]); ptr[0] += 2
            return self.u32(self.data_block + idx)
        if a == PLL:
            idx = b[ptr[0]]; ptr[0] += 1
            return self.pll_read(idx)
        if a == MC:
            idx = b[ptr[0]]; ptr[0] += 1
            return self.mc_read(idx)
        if a == IMM:
            align = (attr >> 3) & 7
            if align == 0:
                v = self.u32(ptr[0]); ptr[0] += 4; return v
            if align in (1, 2, 3):
                v = self.u16(ptr[0]); ptr[0] += 2; return v
            v = b[ptr[0]]; ptr[0] += 1; return v
        return 0

    def get_src(self, ec, attr, ptr):
        a = attr & 7
        if a == IMM:
            return self._operand(ec, IMM, attr, ptr)
        val = self._operand(ec, a, attr, ptr)
        align = (attr >> 3) & 7
        return (val & ARG_MASK[align]) >> ARG_SHIFT[align]

    def get_src_direct(self, align, ptr):
        if align == 0:
            v = self.u32(ptr[0]); ptr[0] += 4; return v
        if align in (1, 2, 3):
            v = self.u16(ptr[0]); ptr[0] += 2; return v
        v = self.b[ptr[0]]; ptr[0] += 1; return v

    def get_dst(self, ec, arg, attr, ptr):
        align = DST2SRC[(attr >> 3) & 7][(attr >> 6) & 3]
        saved = self._operand(ec, arg, arg | (align << 3), ptr)
        return (saved & ARG_MASK[align]) >> ARG_SHIFT[align], saved

    def put_dst(self, ec, arg, attr, dptr, val, saved):
        align = DST2SRC[(attr >> 3) & 7][(attr >> 6) & 3]
        v = val & (ARG_MASK[align] >> ARG_SHIFT[align])
        v = ((v << ARG_SHIFT[align]) & ARG_MASK[align]) | (saved & (~ARG_MASK[align] & M32))
        b = self.b
        if arg == REG:
            idx = self.u16(dptr[0]); dptr[0] += 2
            idx += self.reg_block
            if self.io_mode == 0:
                self.reg_write(idx, (v << 2) & M32 if idx == 0 else v)
            elif self.io_mode in (1, 2):
                self.note('PCI/SYSIO write')
            else:
                prog = self.iio[self.io_mode & 0xFF]
                if prog: self.iio_execute(prog, idx, v)
                else: self.note('IIO write: no prog')
        elif arg == PS:
            idx = b[dptr[0]]; dptr[0] += 1
            if idx >= ec.ps_size: self.note('PS write OOB'); return
            ec.ps[ec.ps_base + idx] = v
        elif arg == WS:
            idx = b[dptr[0]]; dptr[0] += 1
            if idx == WS_QUOT: self.divmul[0] = v
            elif idx == WS_REM: self.divmul[1] = v
            elif idx == WS_DPTR: self.data_block = v
            elif idx == WS_SHIFT: self.shift = v
            elif idx in (WS_ORM, WS_ANDM): self.note('WS mask write (ignored)')
            elif idx == WS_FBWIN: self.fb_base = v
            elif idx == WS_ATTR: self.io_attr = v
            elif idx == WS_RPTR: self.reg_block = v
            elif idx < ec.ws_size: ec.ws[idx] = v
            else: self.note('WS write OOB')
        elif arg == FB:
            idx = b[dptr[0]]; dptr[0] += 1
            pos = self.fb_base//4 + idx
            if 0 <= pos < len(self.scratch): self.scratch[pos] = v
            else: self.note('FB write OOB')
        elif arg == PLL:
            idx = b[dptr[0]]; dptr[0] += 1
            self.pll_write(idx, v)
        elif arg == MC:
            idx = b[dptr[0]]; dptr[0] += 1
            self.mc_write(idx, v)

    # ---- table execution -------------------------------------------------
    def execute_table(self, idx, ps, ps_base, ps_size):
        base = self.u16(self.cmd_table + 4 + 2*idx)
        if not base:
            return
        if self.depth > 24:
            raise Abort('call depth')
        length = self.u16(base)
        ec = ECtx()
        ec.ps = ps; ec.ps_base = ps_base; ec.ps_size = ps_size
        ec.ps_shift = (self.b[base+5] & 0x7F) // 4
        ws_n = self.b[base+4]
        ec.ws = [0]*ws_n; ec.ws_size = ws_n
        ec.start = base
        ptr = [base + 6]
        end = base + length
        self.depth += 1
        if self.tracef:
            self.tracef.write("== table %d @0x%04x (len %d, WS %d, PSshift %d) depth %d\n"
                              % (idx, base, length, ws_n, ec.ps_shift, self.depth))
        try:
            while True:
                if ptr[0] >= end:
                    if self.tracef: self.tracef.write("== table %d ran off end\n" % idx)
                    return
                op = self.b[ptr[0]]; ptr[0] += 1
                self.instr += 1
                if self.instr > self.budget:
                    raise Abort('instruction budget exceeded at table %d +0x%x' % (idx, ptr[0]-base))
                if op == 0 or op >= 127:
                    return  # kernel: op 0 / out-of-range breaks the loop
                ent = DISP.get(op)
                if ent is None:
                    raise Abort('unknown opcode 0x%02x at table %d +0x%x' % (op, idx, ptr[0]-1-base))
                name, darg = ent
                if name == 'eot':
                    return
                self.exec_op(name, darg, ec, ptr)
        finally:
            self.depth -= 1

    def exec_op(self, name, darg, ec, ptr):
        b = self.b
        if name in ('nop', 'beep', 'savereg', 'restorereg', 'repeat'):
            return
        if name == 'debug':
            ptr[0] += 1; return
        if name == 'postcard':
            ptr[0] += 1; return
        if name == 'processds':
            ln = self.u16(ptr[0]); ptr[0] += ln + 2; return
        if name == 'setdatablock':
            idx = b[ptr[0]]; ptr[0] += 1
            self.data_block = self.u16(self.data_table + 4 + 2*idx); return
        if name == 'setregblock':
            self.reg_block = self.u16(ptr[0]); ptr[0] += 2; return
        if name == 'setfbbase':
            attr = b[ptr[0]]; ptr[0] += 1
            self.fb_base = self.get_src(ec, attr, ptr); return
        if name == 'setport':
            if darg == 'ATI':
                port = self.u16(ptr[0]); ptr[0] += 2
                self.io_mode = 0 if port == 0 else (0x80 | port)
            elif darg == 'PCI':
                self.io_mode = 1; ptr[0] += 1
            else:
                self.io_mode = 2; ptr[0] += 1
            return
        if name == 'calltable':
            idx = b[ptr[0]]; ptr[0] += 1
            self.execute_table(idx, ec.ps, ec.ps_base + ec.ps_shift,
                               ec.ps_size - ec.ps_shift)
            return
        if name == 'delay':
            n = b[ptr[0]]; ptr[0] += 1
            if darg == 'MS':
                self.delay_ms += n
                if self.live: time.sleep(n/1000.0)
            else:
                self.delay_ms += n/1000.0
                if self.live: time.sleep(n/1e6)
            return
        if name == 'jump':
            target = self.u16(ptr[0]); ptr[0] += 2
            take = (darg == 'ALWAYS' or
                    (darg == 'EQUAL' and self.cs_equal) or
                    (darg == 'NOTEQUAL' and not self.cs_equal) or
                    (darg == 'ABOVE' and self.cs_above) or
                    (darg == 'BELOW' and not self.cs_above and not self.cs_equal) or
                    (darg == 'ABOVEOREQUAL' and (self.cs_above or self.cs_equal)) or
                    (darg == 'BELOWOREQUAL' and not self.cs_above))
            if take:
                key = (ec.start, target)
                now = time.time()
                if getattr(self, '_jkey', None) != key:
                    self._jkey = key; self._jsince = now
                elif now - self._jsince > 5.0:
                    raise Abort('stuck poll: jump to table +0x%x looping >5s '
                                '(waiting on a hardware bit)' % target)
                ptr[0] = ec.start + target
            return
        if name == 'switch':
            attr = b[ptr[0]]; ptr[0] += 1
            src = self.get_src(ec, attr, ptr)
            matched = False
            while self.u16(ptr[0]) != CASE_END:
                if b[ptr[0]] == CASE_MAGIC:
                    ptr[0] += 1
                    val = self.get_src(ec, (attr & 0x38) | IMM, ptr)
                    target = self.u16(ptr[0])
                    if val == src:
                        ptr[0] = ec.start + target; matched = True; break
                    ptr[0] += 2
                else:
                    self.note('switch: bad case'); break
            if not matched:
                ptr[0] += 2
            return
        # two-operand ops: read attr, dst (via dptr double-read), src
        attr = b[ptr[0]]; ptr[0] += 1
        dptr = [ptr[0]]
        if name in ('shift_left', 'shift_right'):
            dst, saved = self.get_dst(ec, darg, attr, ptr)
            sh = b[ptr[0]]; ptr[0] += 1
            dst = ((dst << sh) & M32) if name == 'shift_left' else (dst >> sh)
            self.put_dst(ec, darg, attr, dptr, dst, saved); return
        if name == 'clear':
            _v, saved = self.get_dst(ec, darg, attr, ptr)
            self.put_dst(ec, darg, attr, dptr, 0, saved); return
        dst, saved = self.get_dst(ec, darg, attr, ptr)
        if name == 'compare':
            src = self.get_src(ec, attr, ptr)
            self.cs_equal = (dst == src); self.cs_above = (dst > src); return
        if name == 'test':
            src = self.get_src(ec, attr, ptr)
            self.cs_equal = ((dst & src) == 0); return
        if name in ('mul', 'div', 'mul32', 'div32'):
            src = self.get_src(ec, attr, ptr)
            if name == 'mul':
                self.divmul[0] = (dst * src) & M32
            elif name == 'div':
                self.divmul[0], self.divmul[1] = (dst // src, dst % src) if src else (0, 0)
            elif name == 'mul32':
                p = dst * src; self.divmul[0] = p & M32; self.divmul[1] = (p >> 32) & M32
            else:
                if src:
                    q = ((self.divmul[1] << 32) | dst) // src
                    self.divmul[0] = q & M32; self.divmul[1] = (q >> 32) & M32
                else:
                    self.divmul[0] = self.divmul[1] = 0
            return
        if name in ('shl', 'shr'):
            dst_align = DST2SRC[(attr >> 3) & 7][(attr >> 6) & 3]
            src = self.get_src(ec, attr, ptr)
            full = (saved << src) & M32 if name == 'shl' else (saved >> src)
            out = (full & ARG_MASK[dst_align]) >> ARG_SHIFT[dst_align]
            self.put_dst(ec, darg, attr, dptr, out, saved); return
        if name == 'mask':
            m = self.get_src_direct((attr >> 3) & 7, ptr)
            src = self.get_src(ec, attr, ptr)
            self.put_dst(ec, darg, attr, dptr, (dst & m) | src, saved); return
        # plain ALU ops
        src = self.get_src(ec, attr, ptr)
        if name == 'move': out = src
        elif name == 'and': out = dst & src
        elif name == 'or': out = dst | src
        elif name == 'add': out = (dst + src) & M32
        elif name == 'sub': out = (dst - src) & M32
        elif name == 'xor': out = dst ^ src
        else: raise Abort('unhandled op ' + name)
        self.put_dst(ec, darg, attr, dptr, out, saved)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rom', required=True)
    ap.add_argument('--live', action='store_true')
    ap.add_argument('--bdf', default='0000:44:00.0')
    ap.add_argument('--table', type=int, default=0)
    ap.add_argument('--trace')
    ap.add_argument('--budget', type=int, default=5000000)
    ap.add_argument('--max-writes', type=int, default=0)
    a = ap.parse_args()
    bios = open(a.rom, 'rb').read()
    tracef = open(a.trace, 'w') if a.trace else None
    bar5 = '/sys/bus/pci/devices/%s/resource5' % a.bdf if a.live else None
    at = Atom(bios, live=a.live, bar5_path=bar5, trace=tracef, budget=a.budget, max_writes=a.max_writes)
    # root PS exactly as amdgpu_atom_asic_init builds it:
    # ps[0] = FirmwareInfo default SCLK, ps[1] = default MCLK (10 kHz units)
    fwi = at.u16(at.data_table + 0x0C)
    ps = [0]*16
    ps[0] = at.u32(fwi + 8)
    ps[1] = at.u32(fwi + 0x0C)
    print('root PS: defSCLK=%d defMCLK=%d (10 kHz units)' % (ps[0], ps[1]))
    try:
        at.execute_table(a.table, ps, 0, 16)
        status = 'COMPLETED (EOT or end of table)'
    except Abort as e:
        status = 'ABORTED: %s' % e
    print('status        :', status)
    print('instructions  :', at.instr)
    print('MM writes     :', at.n_writes)
    print('total delays  : %.1f ms' % at.delay_ms)
    if at.notes:
        print('notes         :', at.notes)
    if not a.live:
        for label, idx in (('FB_LOCATION 0x2024', 0x809), ('CHMAP 0x2004', 0x801),
                           ('CONFIG_MEMSIZE 0x5428', 0x150A), ('MC_SEQ_MISC0 0x2a00', 0xA80)):
            if idx in at.shadow:
                print('shadow %-22s = 0x%08x' % (label, at.shadow[idx]))
    if tracef: tracef.close()

if __name__ == '__main__':
    main()
