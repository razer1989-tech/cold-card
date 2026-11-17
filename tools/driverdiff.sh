#!/usr/bin/env bash
# driverdiff.sh v2 - the post-driver register diff as ONE command, typed
# while the screen is still visible. NO blind typing: after launch this
# needs zero input. The screen will grey during modprobe (twin modeset) -
# expected; the script logs everything to driverdiff.log and brings the
# GUI back via gfx.sh at the end on success AND on abort.
#
# v2 changes (from run 3, 2026-11-17): run 3's twin half was perfect
# (UcodeLoadStatus 0x5FE) but the eGPU's SMC was still VIRGIN - that
# boot's BAR map was a dud (BAR0 unassigned), so the stock probe died at
# gmc sw_init -22 BEFORE ever reaching the SMC load stage. The Nov 16
# parked-0x6 state only happens when BAR0 is assigned before the probe.
# So v2 runs barfix2 itself (BARs assigned first), then wakes the card,
# then modprobes. Also: explicit root check (run 1's EPERM lesson).
#
# BEFORE running: boot the STOCK kernel (the normal GRUB default,
# 7.0.0-43 - NOT the -42 frontier kernel), log in at the TTY, then:
#     sudo bash /home/wednesday/egpu/driverdiff.sh
# Full run takes ~4-5 minutes. Do not touch the keyboard meanwhile.
# Afterwards, upload ~/egpu/asicdiff.txt.
set -u
EGPU=/home/wednesday/egpu
LOG=$EGPU/driverdiff.log
exec > >(tee -a "$LOG") 2>&1
echo
echo "######## driverdiff.sh v2 - $(date) ########"
echo "kernel: $(uname -r)"
echo "amdgpu module: $(modinfo -F filename amdgpu 2>/dev/null || echo '?')"
[ "$(id -u)" = "0" ] || { echo "*** run with sudo: sudo bash $EGPU/driverdiff.sh"; exit 1; }

say() { echo; echo "=== $* ==="; }
finish() {
  echo
  echo "*** Bringing the GUI back..."
  bash $EGPU/gfx.sh
  echo "######## driverdiff.sh done - $(date) ########"
}
abort() { echo; echo "*** ABORT: $*"; exit 1; }
trap finish EXIT

find_bdf() { # $1 = "egpu"|"twin"
  local d want
  for d in /sys/bus/pci/devices/*; do
    [ "$(cat $d/vendor 2>/dev/null)" = "0x1002" ] || continue
    [ "$(cat $d/device 2>/dev/null)" = "0x67df" ] || continue
    if [ "$(cat $d/subsystem_device 2>/dev/null)" = "0xa14c" ]; then want=egpu; else want=twin; fi
    [ "$want" = "$1" ] && { basename "$d"; return 0; }
  done
  return 1
}

say "1. Wait for the eGPU to arrive (up to 60 s)"
EGPU_BDF=""
for i in $(seq 1 60); do EGPU_BDF=$(find_bdf egpu) && break; sleep 1; done
[ -n "$EGPU_BDF" ] || abort "eGPU never appeared in sysfs"
echo "eGPU at $EGPU_BDF"
TWIN_BDF=$(find_bdf twin) || abort "twin not found"
echo "twin at $TWIN_BDF"

say "2. barfix2 (assign the BARs BEFORE the driver attempts the card)"
[ -f $EGPU/barfix2.sh ] || abort "barfix2.sh missing from $EGPU"
bash $EGPU/barfix2.sh || abort "barfix2 failed"
EGPU_BDF=$(find_bdf egpu) || abort "eGPU lost after barfix2"
echo "eGPU now at $EGPU_BDF"

say "3. Wake the card"
setpci -s $EGPU_BDF COMMAND=0006
echo on > /sys/bus/pci/devices/$EGPU_BDF/power/control

say "4. modprobe amdgpu (screen will grey - expected)"
modprobe amdgpu || abort "modprobe failed"

say "5. Wait for the twin to bind (up to 120 s)"
ok=0
for i in $(seq 1 60); do
  [ -e /sys/bus/pci/devices/$TWIN_BDF/driver ] && { ok=1; break; }
  sleep 2
done
[ $ok = 1 ] || abort "twin never bound to a driver"
echo "twin bound after ~$((i * 2)) s"

say "6. Settle 90 s (the driver's eGPU attempt runs its course)"
sleep 90

say "7. Register diff"
python3 $EGPU/asicdiff.py || abort "asicdiff failed"
echo "asicdiff.txt written next to the script in $EGPU"
