#!/usr/bin/env python3
# asicdiff.py v3 - wide ASIC register diff: cold Blackmagic eGPU vs warm
# internal twin, on ONE stock boot, after the stock driver has treated
# both cards the same way.
#
# v3 changes (2026-11-17, the author's pick after the designed diff cleared
# the BAR5 register space): adds a sweep of the SMU's own hardware
# register block behind the SMC indirect window, 0xE0003000-0xE0003800 -
# the last register territory never compared (SMU_STATUS at 0xE0003088
# was previously the only address ever sampled there).
#
# v2 changes (from the 2026-11-17 first run, which read a sleeping eGPU -
# all 0xFFFFFFFF, the documented "device asleep, decode off" void - and a
# twin whose driver pass had not happened yet):
#   * the script performs the frontier wake block itself before reading:
#     power/control=on + COMMAND memory/bus-master bits (read-modify-write
#     on the config file), then settles 1 s;
#   * it prints each card's conditions (bound driver, power state, COMMAND)
#     so the output file states the conditions it was taken under;
#   * sanity guards: a card still reading all-FF after the wake is reported
#     as NOT ANSWERING and no garbage diff is produced.
# Device writes are exactly: the wake block above + the SMC address-select
# writes needed to READ SMC space (same as smustat.py). No dance, no load,
# no mailbox messages.
#
# Run order: boot normally, gfx.sh to the desktop, wait a full minute at
# the desktop (the driver needs its pass at both cards and the eGPU needs
# time to arrive), then:
#   sudo python3 ~/egpu/asicdiff.py
import mmap, os, struct, sys, time

NAMES = {0: "SMC", 1: "SDMA0", 2: "SDMA1", 3: "CP_CE", 4: "CP_PFP", 5: "CP_ME",
         6: "CP_MEC", 7: "CP_MEC_JT1", 8: "CP_MEC_JT2", 9: "SMC_EX", 10: "RLC_G",
         11: "SDMA2", 12: "SDMA3", 13: "UVD", 14: "VCE", 15: "ACP", 16: "SAMU"}

R_IND_INDEX, R_IND_DATA = 0x6B0, 0x6B4
A_PC, A_SMU_STATUS = 0x80000370, 0xE0003088
HDR_SOFTREGS_PTR, STATUS_OFFSET = 0x20030, 108

ANCHORS = [
    (0x250, "SMC_MESSAGE"), (0x254, "SMC_RESP"), (0x290, "SMC_ARG"),
    (0x8008, "GRBM_STATUS2"), (0x8010, "GRBM_STATUS"),
    (0x8020, "GRBM_SOFT_RESET"), (0x8090, "MC_VM_FB_LOCATION"),
    (0x86D8, "CP_ME_CNTL"), (0x8708, "CP_RB_WPTR_POLL_CNTL"),
    (0x3B120, "RLC_CGTT_MGCG_OVERRIDE"), (0x3B124, "RLC_CGCG_CGLS_CTRL"),
    (0x3C2C0, "CGTT_CP_CLK_CTRL"), (0x3C2C4, "CGTT_CPF_CLK_CTRL"),
    (0x3C2C8, "CGTT_CPC_CLK_CTRL"), (0x3C2E0, "CGTT_RLC_CLK_CTRL"),
]
SWEEPS = [(0x8000, 0x8800), (0x3B100, 0x3B180), (0x3C200, 0x3C300)]
SMC_SWEEP = (0x80000300, 0x800003C0)
# SMU hardware register block, reached via the SMC indirect window
SMU_SWEEP = (0xE0003000, 0xE0003800)
SMU_NAMES = {0xE0003088: "SMU_STATUS"}


def find_cards():
    cards = []
    base = "/sys/bus/pci/devices"
    for bdf in sorted(os.listdir(base)):
        dev = os.path.join(base, bdf)
        try:
            with open(dev + "/vendor") as f: vendor = f.read().strip()
            with open(dev + "/device") as f: device = f.read().strip()
            if vendor != "0x1002" or device != "0x67df":
                continue
            with open(dev + "/subsystem_vendor") as f: sv = f.read().strip()
            with open(dev + "/subsystem_device") as f: sd = f.read().strip()
            with open(dev + "/resource") as f: res = f.read().splitlines()
            bar5 = int(res[5].split()[0], 16)
        except Exception:
            continue
        label = "EGPU" if sd == "0xa14c" else "TWIN"
        cards.append({"bdf": bdf, "label": label, "subsys": sv + ":" + sd,
                      "bar5": bar5, "dev": dev})
    return cards


def read_text(path):
    try:
        with open(path) as f:
            return f.read().strip()
    except Exception:
        return "?"


def wake(dev):
    # frontier wake block: power/control on, COMMAND |= IO? no - MEM + BME
    try:
        with open(dev + "/power/control", "w") as f:
            f.write("on")
    except Exception:
        pass
    cfg = os.open(dev + "/config", os.O_RDWR)
    cur = struct.unpack("<H", os.pread(cfg, 2, 0x04))[0]
    os.pwrite(cfg, struct.pack("<H", cur | 0x0006), 0x04)
    os.close(cfg)
    time.sleep(1.0)


class Card:
    def __init__(self, info):
        self.info = info
        self.mm = None
        if info["bar5"]:
            self.mm = mmap.mmap(os.open(info["dev"] + "/resource5", os.O_RDWR),
                                0x40000, mmap.MAP_SHARED,
                                mmap.PROT_READ | mmap.PROT_WRITE)

    def r32(self, off):
        return struct.unpack_from("<I", self.mm, off)[0]

    def w32(self, off, val):
        struct.pack_into("<I", self.mm, off, val & 0xFFFFFFFF)

    def sram(self, addr):
        self.w32(R_IND_INDEX, addr)
        return self.r32(R_IND_DATA)

    def answering(self):
        return self.mm and self.r32(0x250) != 0xFFFFFFFF and self.r32(0x8010) != 0xFFFFFFFF

    def snapshot(self):
        snap = {"bar5regs": {}, "smc": {}, "sram_soft": {}, "smu": {}}
        if not self.mm:
            return snap
        offs = set(o for o, _ in ANCHORS)
        for lo, hi in SWEEPS:
            offs.update(range(lo, hi, 4))
        for off in sorted(offs):
            snap["bar5regs"][off] = self.r32(off)
        for addr in range(SMC_SWEEP[0], SMC_SWEEP[1], 4):
            snap["smc"][addr] = self.sram(addr)
        for addr in range(SMU_SWEEP[0], SMU_SWEEP[1], 4):
            snap["smu"][addr] = self.sram(addr)
        soft = self.sram(HDR_SOFTREGS_PTR)
        snap["soft_regs"] = soft
        snap["smu_status"] = self.sram(A_SMU_STATUS)
        snap["load_status"] = self.sram(soft + STATUS_OFFSET) if 0 < soft < 0x40000 else None
        if 0 < soft < 0x40000:
            for i in range(0, 0x200, 4):
                snap["sram_soft"][soft + i] = self.sram(soft + i)
        snap["pc"] = [self.sram(A_PC) for _ in range(4)]
        return snap


def fmt(v):
    return "0x%08x" % v if v is not None else "    None"


def main():
    cards = find_cards()
    if len(cards) < 2:
        sys.exit("found %d Ellesmere card(s), need 2 - is the eGPU plugged in "
                 "and given a minute to arrive?" % len(cards))
    lines = []
    snaps = {}
    for info in cards:
        dev = info["dev"]
        drv = "none"
        link = os.path.join(dev, "driver")
        if os.path.islink(link):
            drv = os.path.basename(os.readlink(link))
        head = "%s %s subsys %s BAR5 %s driver %s power/control %s runtime %s" % (
            info["label"], info["bdf"], info["subsys"],
            hex(info["bar5"]) if info["bar5"] else "UNASSIGNED",
            drv, read_text(dev + "/power/control"),
            read_text(dev + "/power/runtime_status"))
        lines.append(head)
        print(head)
        if not info["bar5"]:
            lines.append("  !! BAR5 unassigned - run barfix2.sh first, then re-run this")
            snaps[info["label"]] = None
            continue
        card = Card(info)
        wake(dev)
        if not card.answering():
            lines.append("  !! NOT ANSWERING even after the wake block (all-FF reads) - state unknown, no diff for this card")
            snaps[info["label"]] = None
            continue
        s1 = card.snapshot()
        time.sleep(0.5)
        s2 = card.snapshot()
        snaps[info["label"]] = (s1, s2)
        st = s1["load_status"]
        loaded = [NAMES.get(b, "bit%d" % b) for b in range(17)
                  if st is not None and st & (1 << b)]
        lines.append("  soft_regs 0x%x  SMU_STATUS 0x%x" % (s1["soft_regs"], s1["smu_status"]))
        lines.append("  UcodeLoadStatus %s -> %s" % (fmt(st), ", ".join(loaded) if loaded else "(none)"))
        lines.append("  PC samples: %s" % " ".join("0x%x" % p for p in s1["pc"]))
    twin = snaps.get("TWIN")
    egpu = snaps.get("EGPU")
    if twin and egpu:
        lines.append("")
        lines.append("=== STABLE DIFFERENCES (same value twice per card, twin != eGPU) ===")
        ndiff = 0
        for region in ("bar5regs", "smc", "sram_soft", "smu"):
            keys = sorted(set(twin[0][region]) & set(egpu[0][region]))
            for k in keys:
                t1, t2 = twin[0][region][k], twin[1][region][k]
                e1, e2 = egpu[0][region][k], egpu[1][region][k]
                if t1 == t2 and e1 == e2 and t1 != e1:
                    name = ""
                    if region == "bar5regs":
                        name = dict(ANCHORS).get(k, "")
                    elif region == "smu":
                        name = SMU_NAMES.get(k, "")
                    lines.append("  %-9s %s %-24s twin %s  eGPU %s" %
                                 (region, ("0x%x" % k), name, fmt(t1), fmt(e1)))
                    ndiff += 1
        lines.append("stable differences: %d" % ndiff)
        lines.append("")
        lines.append("=== VOLATILE (changed between the two reads on a card - live counters, ignore) ===")
        for region in ("bar5regs", "smc", "sram_soft", "smu"):
            keys = sorted(set(twin[0][region]) & set(egpu[0][region]))
            vol = [k for k in keys
                   if twin[0][region][k] != twin[1][region][k]
                   or egpu[0][region][k] != egpu[1][region][k]]
            lines.append("  %s: %d volatile registers" % (region, len(vol)))
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "asicdiff.txt")
    with open(out, "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))
    print("\nwrote", out)


if __name__ == "__main__":
    main()
