#!/usr/bin/env bash
# barfix2.sh - rehabilitate a dud boot where the Blackmagic eGPU's BARs
# came up unassigned. Rebuilt 2026-11-17 from the project record of the
# original (created 2026-11-09; the original lived only on the iMac):
#   1. program the 512 MB 64-bit prefetch window
#      0x4020000000-0x403FFFFFFF into every bridge on the path from the
#      root port down to the GPU (setpci 24.W/26.W/28.L/2C.L - the exact
#      registers and values whose readbacks are in the Nov 9 findings),
#   2. remove the Thunderbolt root port 0000:00:1c.4 and rescan, forcing
#      a top-down rebuild of the whole subtree (the step that actually
#      makes the kernel re-derive and assign the BARs),
#   3. report the resulting map and fail loudly if BAR0/BAR5 are still 0.
# The middle bridge BDFs are discovered from the GPU's sysfs path instead
# of hardcoded - they change between boots (42/43 on Nov 9, 09/0A on
# Nov 16/17). Expected result map: BAR0 0x4010000000, BAR2 0x4020000000,
# BAR5 0x80400000 (BAR5's exact address may vary).
# Config-space surgery only - the ASIC/SMC are never touched (verified
# 2026-11-09: the SMC's virgin state survived this intact).
# Run: sudo bash ~/egpu/barfix2.sh
set -u
[ "$(id -u)" = "0" ] || { echo "barfix2: run with sudo"; exit 1; }

find_egpu() {
  local d
  for d in /sys/bus/pci/devices/*; do
    [ "$(cat $d/vendor 2>/dev/null)" = "0x1002" ] || continue
    [ "$(cat $d/device 2>/dev/null)" = "0x67df" ] || continue
    [ "$(cat $d/subsystem_device 2>/dev/null)" = "0xa14c" ] || continue
    basename "$d"
    return 0
  done
  return 1
}

GPU=$(find_egpu) || { echo "barfix2: eGPU not found - has it arrived yet? (give it a minute after boot)"; exit 1; }
echo "barfix2: eGPU at $GPU"
DEV=/sys/bus/pci/devices/$GPU

echo "before:"
awk 'NR==1||NR==3||NR==6 {printf "  resource%d start %s\n", NR-1, $1}' $DEV/resource

# 1. prefetch window into every ancestor bridge (root port included)
CANON=$(readlink -f "$DEV")
BRIDGES=$(echo "$CANON" | grep -o '[0-9a-f]\{4\}:[0-9a-f]\{2\}:[0-9a-f]\{2\}\.[0-9]' | head -n -1)
for b in $BRIDGES; do
  cls=$(cat /sys/bus/pci/devices/$b/class 2>/dev/null)
  case "$cls" in
    0x0604*) setpci -s $b 24.W=2001 26.W=3ff1 28.L=00000040 2C.L=00000040
             echo "  window -> bridge $b" ;;
  esac
done

# 2. remove the root port, rescan: full subtree rebuild
echo "barfix2: removing root port 0000:00:1c.4 and rescanning..."
echo 1 > /sys/bus/pci/devices/0000:00:1c.4/remove
sleep 1
echo 1 > /sys/bus/pci/rescan

# 3. wait for the card to come back (its BDF may change), then report
for i in $(seq 1 30); do
  GPU=$(find_egpu) && break
  sleep 1
done
GPU=$(find_egpu) || { echo "barfix2: eGPU did not come back after rescan"; exit 1; }
DEV=/sys/bus/pci/devices/$GPU
BAR0=$(sed -n '1p' $DEV/resource | awk '{print $1}')
BAR2=$(sed -n '3p' $DEV/resource | awk '{print $1}')
BAR5=$(sed -n '6p' $DEV/resource | awk '{print $1}')
echo "after: eGPU at $GPU"
echo "  BAR0 $BAR0"
echo "  BAR2 $BAR2"
echo "  BAR5 $BAR5"
if [ "$BAR0" = "0x0000000000000000" ] || [ "$BAR5" = "0x0000000000000000" ]; then
  echo "barfix2: FAILED - BARs still unassigned"
  exit 1
fi
echo "barfix2: OK"
