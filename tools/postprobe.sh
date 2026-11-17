#!/usr/bin/env bash
# postprobe.sh - the atomrun discriminator, ONE command, typed while the
# screen is visible. Question it answers (open since 2026-11-09): when
# the userspace POST (atomrun, ASIC_Init table) wedges at its display
# clock poll, is the executor mis-reading (case b) or did the run's own
# writes change the real register (case a)?
#   peek 0x12344 (TX_CLOCK_CNTL, expect 0xf1, bit 2 clear) BEFORE,
#   run atomrun --live, then peek 0x12344 again IMMEDIATELY:
#   bit 2 now set   -> the run changed real state (case a)
#   still 0xf1      -> the executor mis-reads mid-run (case b)
# No dance, no load, no modprobe - the POST runs on a driver-untouched
# card. Logs everything to postprobe.log; GUI restored via gfx.sh at
# the end on success AND on abort.
#
# BEFORE running: virgin cycle (BOTH cables out while the machine is
# down), boot the STOCK kernel (GRUB default), log in at the TTY, make
# sure atomrun.py, regpeek5.py and egpu-rom.bin sit in ~/egpu, then:
#     sudo bash /home/wednesday/egpu/postprobe.sh
set -u
EGPU=/home/wednesday/egpu
LOG=$EGPU/postprobe.log
exec > >(tee -a "$LOG") 2>&1
echo
echo "######## postprobe.sh - $(date) ########"
echo "kernel: $(uname -r)"
[ "$(id -u)" = "0" ] || { echo "*** run with sudo: sudo bash $EGPU/postprobe.sh"; exit 1; }
[ -f $EGPU/atomrun.py ] || { echo "*** atomrun.py missing from $EGPU"; exit 1; }
[ -f $EGPU/regpeek5.py ] || { echo "*** regpeek5.py missing from $EGPU"; exit 1; }
[ -f $EGPU/egpu-rom.bin ] || { echo "*** egpu-rom.bin missing from $EGPU"; exit 1; }

say() { echo; echo "=== $* ==="; }
finish() {
  echo
  echo "*** Bringing the GUI back..."
  bash $EGPU/gfx.sh
  echo "######## postprobe.sh done - $(date) ########"
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

say "2. barfix2 + wake (BAR5 must be assigned and decoding)"
bash $EGPU/barfix2.sh || abort "barfix2 failed"
DEV=$(find_egpu) || abort "eGPU lost after barfix2"
setpci -s $DEV COMMAND=0006
echo on > /sys/bus/pci/devices/$DEV/power/control
sleep 1

say "3. Peek TX_CLOCK_CNTL (dword 0x48d1, byte 0x12344) BEFORE - expect f1"
python3 $EGPU/regpeek5.py $DEV 12344
python3 $EGPU/regpeek5.py $DEV 12344

say "4. atomrun --live (ASIC_Init POST, trace to postprobe-trace.txt)"
timeout 180 python3 $EGPU/atomrun.py --rom $EGPU/egpu-rom.bin --live --bdf $DEV \
  --trace $EGPU/postprobe-trace.txt || echo "(atomrun exited $?)"

say "5. Peek TX_CLOCK_CNTL IMMEDIATELY after - the discriminator"
python3 $EGPU/regpeek5.py $DEV 12344
python3 $EGPU/regpeek5.py $DEV 12344
python3 $EGPU/regpeek5.py $DEV 12344
echo "verdict key: bit 2 of the value above set (e.g. f5) = case (a) the run changed real state;"
echo "             still f1 = case (b) the executor mis-reads during the run."
