#!/usr/bin/env bash
# postbisect.sh <cap> - run the userspace POST (atomrun_cap) for exactly
# <cap> register writes, then peek four sentinel registers to learn
# whether the card's register file is still alive at that point:
#   0x12344 DPCSTX_TX_CLOCK_CNTL (display PHY - the one that went dark)
#   0x08010 GRBM_STATUS          (graphics block)
#   0x00250 SMC_MESSAGE          (manager mailbox)
#   0x086d8 CP_ME_CNTL           (command processor)
# Ladder (from the 2026-11-17 trace classification):
#   cap 217 = stop just BEFORE the final display cluster (writes 218+)
#   cap 235 = stop AFTER that cluster, BEFORE the CRTC_CONTROL pair
#             (writes 236/237)
# ONE command per boot, typed while the screen is visible. Needs a
# virgin cycle before EACH run (the dark state persists until the card
# power-cycles). Logs to postbisect.log (append); GUI restored via
# gfx.sh at the end on success AND on abort.
#
#     sudo bash /home/wednesday/egpu/postbisect.sh 217
set -u
EGPU=/home/wednesday/egpu
LOG=$EGPU/postbisect.log
CAP=${1:-}
exec > >(tee -a "$LOG") 2>&1
echo
echo "######## postbisect.sh cap=$CAP - $(date) ########"
echo "kernel: $(uname -r)"
[ "$(id -u)" = "0" ] || { echo "*** run with sudo: sudo bash $EGPU/postbisect.sh <cap>"; exit 1; }
case "$CAP" in ''|*[!0-9]*) echo "*** usage: postbisect.sh <write cap, e.g. 217 or 235>"; exit 1;; esac
[ -f $EGPU/atomrun_cap.py ] || { echo "*** atomrun_cap.py missing from $EGPU"; exit 1; }
[ -f $EGPU/regpeek5.py ] || { echo "*** regpeek5.py missing from $EGPU"; exit 1; }
[ -f $EGPU/egpu-rom.bin ] || { echo "*** egpu-rom.bin missing from $EGPU"; exit 1; }

say() { echo; echo "=== $* ==="; }
finish() {
  echo
  echo "*** Bringing the GUI back..."
  bash $EGPU/gfx.sh
  echo "######## postbisect.sh cap=$CAP done - $(date) ########"
}
abort() { echo; echo "*** ABORT: $*"; exit 1; }
trap finish EXIT

find_egpu() {
  local d
  for d in /sys/bus/pci/devices/*; do
    [ "$(cat $d/vendor 2>/dev/null)" = "0x1002" ] || continue
    [ "$(cat $d/device 2>/dev/null)" = "0x67df" ] || continue
    [ "$(cat $d/subsystem_device 2>/dev/null)" = "0xa14c" ] || continue
    basename "$d"; return 0
  done
  return 1
}

say "1. Wait for the eGPU to arrive (up to 60 s)"
DEV=""
for i in $(seq 1 60); do DEV=$(find_egpu) && break; sleep 1; done
[ -n "$DEV" ] || abort "eGPU never appeared in sysfs"
echo "eGPU at $DEV"
if [ -e /sys/bus/pci/devices/$DEV/driver ]; then
  abort "a driver is already bound to the eGPU - the POST must run driver-untouched (fresh boot, no modprobe first)"
fi

say "2. barfix2 + wake"
bash $EGPU/barfix2.sh || abort "barfix2 failed"
DEV=$(find_egpu) || abort "eGPU lost after barfix2"
setpci -s $DEV COMMAND=0006
echo on > /sys/bus/pci/devices/$DEV/power/control
sleep 1

say "3. Sentinel peeks BEFORE (baseline - expect real values, 0x12344 = f1)"
python3 $EGPU/regpeek5.py $DEV 12344 8010 250 86d8

say "4. atomrun_cap --live --max-writes $CAP"
timeout 180 python3 $EGPU/atomrun_cap.py --rom $EGPU/egpu-rom.bin --live --bdf $DEV \
  --max-writes $CAP --trace $EGPU/postbisect-trace-$CAP.txt || echo "(atomrun_cap exited $?)"

say "5. Sentinel peeks AFTER cap $CAP - the verdict"
python3 $EGPU/regpeek5.py $DEV 12344 8010 250 86d8
python3 $EGPU/regpeek5.py $DEV 12344 8010 250 86d8
echo "reading: 0x12344 = f1 means the display block is still alive;"
echo "         0x12344 = ffffffff with the others alive = display-domain darkness;"
echo "         all four ffffffff = the whole register file dropped."
