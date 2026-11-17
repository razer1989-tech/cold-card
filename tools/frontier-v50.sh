#!/usr/bin/env bash
# frontier.sh v5.0 - the whole eGPU frontier + SMC load, ONE command,
# v5.0 change vs v4.9: the loader returns to smcload15 (mask 0x5FE) -
# the v4.5 baseline - because tonight's variable under test is the
# EFI STAGE (egpushim v0.4 full placement + COMMAND enable before
# GRUB), not the TOC. Same dance (Apple SMC image), same PASS=1 gate.
# (TOC gains SMU_SK id 14 + MEC_STORAGE id 35; LoadUcodes mask
# 0x447E, exactly as the macOS stack sends). Dance unchanged (Apple
# genuine image). Gate hardened: PASS=1 required (v4.7 bug fix).
# NO blind typing. Run it while you can still see the screen; after
# launch it needs zero input. The grey screen can do what it wants -
# everything is logged to the frontier log, every gate is checked by
# the script itself, and the GUI comes back on its own at the end
# (success or abort) via gfx.sh.
#
# v3 fix: all paths are pinned to EGPU=/home/wednesday/egpu. Under
# `sudo bash`, ~ expands to /root - v2's ~/egpu paths all pointed at
# a folder that does not exist and the script aborted instantly.
#
# BEFORE running: virgin cycle (BOTH cables out while the machine is
# down), boot the 7.0.0-42-generic kernel (GRUB > Advanced options),
# log in at the TTY, then:
#     sudo bash /home/wednesday/egpu/frontier.sh
# (sudo once, up front, while you can see the password prompt.)
# Full run takes ~10-12 minutes. Do not touch the keyboard meanwhile.
set -u
EGPU=/home/wednesday/egpu
DEV=0000:44:00.0
SYS=/sys/bus/pci/devices/$DEV
LOG=$EGPU/frontier.log
exec > >(tee -a "$LOG") 2>&1
echo
echo "######## frontier.sh v5.0 - $(date) ########"

say() { echo; echo "=== $* ==="; }
abort() {
  echo
  echo "*** ABORT: $*"
  echo "*** Bringing the GUI back..."
  bash $EGPU/gfx.sh
  echo "######## frontier.sh aborted - $(date) ########"
  exit 1
}

say "1. Wake block"
echo 1 > /sys/bus/pci/rescan
BAR0=$(sed -n '1p' $SYS/resource | awk '{print $1}')
BAR5=$(sed -n '6p' $SYS/resource | awk '{print $1}')
echo "BAR0 starts at $BAR0 - BAR5 starts at $BAR5"
if [ "$BAR0" = "0x0000000000000000" ] || [ "$BAR5" = "0x0000000000000000" ]; then
  echo "BARs unassigned - running barfix2 NOW, before any regpeek (the all-zeros trap)"
  bash $EGPU/barfix2.sh || abort "barfix2 failed"
  BARFIX_DONE=1
else
  BARFIX_DONE=0
fi
setpci -s 44:00.0 COMMAND=0002
echo on > $SYS/power/control
echo "regpeek gate (informational - the dance is the hard gate): want f1, then 0"
python3 $EGPU/regpeek5.py $DEV 12344 2024
python3 $EGPU/regpeek5.py $DEV 12344 2024

say "2. BAR map"
if [ $BARFIX_DONE -eq 0 ]; then
  bash $EGPU/barfix2.sh || abort "barfix2 failed"
else
  echo "already applied in step 1"
fi

say "3. Module gate"
MPATH=$(modinfo -n amdgpu)
echo "modinfo: $MPATH"
case "$MPATH" in
  *7.0.0-42-generic/updates/*) echo "patched module - good" ;;
  *) abort "wrong module - need the -42 updates/ build; check which kernel you booted" ;;
esac

say "4. POST-and-stop"
modprobe amdgpu || abort "modprobe failed"
if dmesg | grep -qi "stopping before IP init"; then
  echo "guard line found - POST-and-stop confirmed"
else
  echo "guard line NOT found; last amdgpu lines in dmesg:"
  dmesg | grep -i amdgpu | tail -5
  abort "POST-and-stop guard did not fire - not safe to dance"
fi
echo "(grey screen from here on is expected - nobody needs to type anything)"

say "5. The dance"
python3 $EGPU/smcdance5.py $EGPU/polaris10_smc_apple.bin | tee /tmp/dance.out
if grep -q "DONE" /tmp/dance.out && grep -q "PASS=1" /tmp/dance.out; then
  echo "dance DONE+PASS confirmed"
else
  abort "dance did not report DONE+PASS"
fi

say "6. Bus master"
setpci -s 44:00.0 COMMAND=0006
CMD=$(setpci -s 44:00.0 COMMAND)
echo "COMMAND readback: $CMD"
[ "$CMD" = "0006" ] || abort "COMMAND is $CMD, expected 0006"

say "7. The load"
python3 $EGPU/smcload15.py 0x5FE

say "Done - bringing the GUI back"
bash $EGPU/gfx.sh
echo "######## frontier.sh finished - $(date) ########"
echo "Full log: $LOG - the step 7 output is what Wednesday needs."
