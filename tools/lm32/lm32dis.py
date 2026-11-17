#!/usr/bin/env python3
"""Minimal LM32 disassembler for AMD SMU7 firmware (polaris10_smc.bin).

Opcode map parsed from jevinskie/ghidra-lm32's lm32.sinc (same dir).
Image: file offset 256 (array_offset), executes at SRAM 0x20000.
Usage: lm32dis.py START_PC END_PC [--file PATH]
"""
import re
import struct
import sys
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OPS = {}
for line in open(os.path.join(HERE, "lm32.sinc")):
    m = re.match(r":(\S+).*?is op=(0x[0-9a-fA-F]+|\d+)", line)
    if m:
        OPS.setdefault(int(m.group(2), 0), m.group(1))

BASE = 0x20000
BRANCHES = (0x11, 0x12, 0x13, 0x14, 0x15, 0x17)
STORES = (0x03, 0x0C, 0x16)


def dis(img, pc):
    w = struct.unpack_from("<I", img, pc - BASE)[0]
    op = (w >> 26) & 0x3F
    reg0 = (w >> 21) & 0x1F
    reg1 = (w >> 16) & 0x1F
    reg2 = (w >> 11) & 0x1F
    imm = w & 0xFFFF
    simm = imm - 0x10000 if imm & 0x8000 else imm
    name = OPS.get(op, "op%02x" % op)
    if op in (0x38, 0x3E):
        s26 = w & 0x3FFFFFF
        s26 -= 0x4000000 if s26 & 0x2000000 else 0
        return "%s 0x%x" % (name, pc + 4 * s26)
    if op in BRANCHES:
        return "%s r%d, r%d, 0x%x" % (name, reg0, reg1, pc + 4 * simm)
    if op in STORES:
        return "%s (r%d%+d), r%d" % (name, reg0, simm, reg1)
    if op == 0x30:
        return "ret" if reg0 == 29 else "b r%d" % reg0
    if op == 0x36:
        return "call r%d" % reg0
    if 0x20 <= op <= 0x37:
        return "%s r%d, r%d, r%d" % (name, reg2, reg0, reg1)
    if op == 0x1E and reg0 == 0:
        return "mvhi r%d, 0x%x" % (reg1, imm)
    return "%s r%d, r%d, %d/0x%x" % (name, reg1, reg0, simm, imm)


def main():
    path = "/home/hatch/workspace/user/files/polaris10_smc.bin"
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if "--file" in sys.argv:
        path = sys.argv[sys.argv.index("--file") + 1]
    img = open(path, "rb").read()[256:]
    start, end = int(args[0], 0), int(args[1], 0)
    for pc in range(start, end, 4):
        print("0x%x: %s" % (pc, dis(img, pc)))


if __name__ == "__main__":
    main()
