#!/usr/bin/env bash
# posttwin.sh - the control experiment: run the SAME capped userspace
# POST (atomrun_cap, 217 writes) on the INTERNAL TWIN, then peek the
# same four sentinels. Question: does userspace POST darken ANY card
# (executor/environment effect) or only the eGPU (card-specific)?
#   twin alive after  -> eGPU darkness is card-specific
#   twin dark after   -> userspace POST darkens both; road closes
# No virgin cycle needed (the twin lives its normal EFI-posted life).
# Boot the STOCK kernel, log in at the TTY (no desktop, no modprobe),
# then ONE command:
#     sudo bash /home/wednesday/egpu/posttwin.sh
# If the twin does darken, the internal display may stay dark until
# gfx.sh runs at the end; if it survives even that, a normal reboot
# restores it (EFI POSTs the twin every boot). Logs to posttwin.log.
set -u
EGPU=/home/wednesday/egpu
LOG=$EGPU/posttwin.log
exec > >(tee -a "$LOG") 2>&1
echo
echo "######## posttwin.sh - $(date) ########"
echo "kernel: $(uname -r)"
[ "$(id -u)" = "0" ] || { echo "*** run with sudo: sudo bash $EGPU/posttwin.sh"; exit 1; }
[ -f $EGPU/atomrun_cap.py ] || { echo "*** atomrun_cap.py missing from $EGPU"; exit 1; }
[ -f $EGPU/regpeek5.py ] || { echo "*** regpeek5.py missing from $EGPU"; exit 1; }
[ -f $EGPU/twin-rom.bin ] || { echo "*** twin-rom.bin missing from $EGPU"; exit 1; }

say() { echo; echo "=== $* ==="; }
finish() {
  echo
  echo "*** Bringing the GUI back..."
  bash $EGPU/gfx.sh
  echo "######## posttwin.sh done - $(date) ########"
}
abort() { echo; echo "*** ABORT: $*"; exit 1; }
trap finish EXIT

find_twin() {
  local d
  for d in /sys/bus/pci/devices/*; do
    [ "$(cat $d/vendor 2>/dev/null)" = "0x1002" ] || continue
    [ "$(cat $d/device 2>/dev/null)" = "0x67df" ] || continue
    [ "$(cat $d/subsystem_device 2>/dev/null)" = "0xa14c" ] && continue
    basename "$d"; return 0
  done
  return 1
}

say "1. Find the twin"
DEV=$(find_twin) || abort "twin not found"
echo "twin at $DEV"
if [ -e /sys/bus/pci/devices/$DEV/driver ]; then
  abort "a driver is already bound to the twin - run this from a fresh TTY boot, before the desktop/modprobe"
fi
setpci -s $DEV COMMAND=0006
echo on > /sys/bus/pci/devices/$DEV/power/control
sleep 1

say "2. Sentinel peeks BEFORE (baseline)"
python3 $EGPU/regpeek5.py $DEV 12344 8010 250 86d8

say "3. atomrun_cap --live --max-writes 217 (twin ROM)"
timeout 180 python3 $EGPU/atomrun_cap.py --rom $EGPU/twin-rom.bin --live --bdf $DEV \
  --max-writes 217 --trace $EGPU/posttwin-trace.txt || echo "(atomrun_cap exited $?)"

say "4. Sentinel peeks AFTER - the verdict"
python3 $EGPU/regpeek5.py $DEV 12344 8010 250 86d8
python3 $EGPU/regpeek5.py $DEV 12344 8010 250 86d8
echo "reading: real values = the twin survived userspace POST (eGPU darkness is card-specific);"
echo "         all ffffffff = userspace POST darkens both cards (environment effect)."
