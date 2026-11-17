#!/bin/bash
# egpu-boot-setup.sh - run ONCE in Kubuntu (sudo bash ~/egpu/egpu-boot-setup.sh)
#
# Makes Kubuntu PRESERVE what the EFI shim (egpushim v0.4) sets up and
# keeps the eGPU awake once it works:
#   1. Removes  pci=realloc  and  hpmemsize=*  from the kernel cmdline -
#      those exist to let the KERNEL redo PCI placement, which would
#      override the shim's EFI-stage placement on every boot.
#   2. Adds  pcie_aspm=off  - the PCIe link (incl. the Thunderbolt
#      tunnel) never drops into low-power link states.
#   3. Adds  amdgpu.runpm=0  - the GPU never runtime-suspends. A TB
#      eGPU that enters D3cold cannot wake without a full reset -
#      that is the "goes to sleep and forgets its state" failure.
# Backs up the original to /etc/default/grub.egpu-orig (first run
# only). Idempotent: safe to re-run. Prints BEFORE/AFTER for the log.
set -euo pipefail
[ "$(id -u)" = "0" ] || { echo "run me with sudo"; exit 1; }
GRUB=/etc/default/grub
[ -f "$GRUB" ] || { echo "$GRUB not found - not a GRUB system?"; exit 1; }
[ -f "$GRUB.egpu-orig" ] || cp "$GRUB" "$GRUB.egpu-orig"
echo "== BEFORE =="; grep -E "^GRUB_CMDLINE_LINUX" "$GRUB" || true
python3 - "$GRUB" <<'PYEOF'
import re, sys
path = sys.argv[1]
drop = re.compile(r'^(pci=realloc(=.*)?|hpmemsize=.*)$')
add = ["pcie_aspm=off", "amdgpu.runpm=0"]
out = []
for line in open(path):
    m = re.match(r'^(GRUB_CMDLINE_LINUX(?:_DEFAULT)?=)(["\']?)(.*)\2\s*$', line.rstrip('\n'))
    if not m:
        out.append(line); continue
    var, q, val = m.group(1), m.group(2) or '"', m.group(3)
    toks = [t for t in val.split() if not drop.match(t)]
    for a in add:
        key = a.split('=')[0]
        toks = [t for t in toks if t.split('=')[0] != key] + [a]
    out.append('%s%s%s%s\n' % (var, q, ' '.join(toks), q))
open(path, 'w').writelines(out)
PYEOF
echo "== AFTER =="; grep -E "^GRUB_CMDLINE_LINUX" "$GRUB"
update-grub
echo "== effective kernel line for the -42 entry in grub.cfg =="
grep -m1 "7\.0\.0-42" /boot/grub/grub.cfg | sed 's/^ *//' || echo "(no -42 entry found - tell Wednesday)"
# ------------------------------------------------------------------
# Built-in SD card reader: its Broadcom SDHCI controller wedges under
# Linux (interrupts never arrive) and floods the console with register
# dumps whenever a card is in the slot - including the egpushim boot
# card, whose logs are fetched via macOS anyway. Blacklist the driver
# so Kubuntu never binds it. macOS is unaffected. Reversible: delete
# /etc/modprobe.d/egpu-nomcc.conf and re-run update-initramfs -u -k all.
BL=/etc/modprobe.d/egpu-nomcc.conf
if [ ! -f "$BL" ]; then
  printf 'blacklist sdhci_pci\nblacklist sdhci\n' > "$BL"
  echo "reader blacklist written: $BL"
else
  echo "reader blacklist already present: $BL"
fi
update-initramfs -u -k all
echo "DONE. The next boot uses the new cmdline."
