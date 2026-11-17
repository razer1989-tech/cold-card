# eGPU firmware-skirt RECON sheet — iMac19,1 + Blackmagic eGPU under Kubuntu

**Purpose (written 2026-11-06):** the author, night of Nov 5: "There has to be a way to
rewrite/skirt the firmware on this hunk of thunderbolt crap to make it work on Linux."
The assistant's answer named three doors — (1) ACPI/DSDT override enlarging the
Thunderbolt window, (2) runtime-only `setpci` reprogramming of the bridge windows
and BAR0, (3) pre-boot reallocation via rEFInd. **Every door needs the same
reconnaissance first.** This sheet is that recon, and ONLY that recon.

**Safety shape:** every command below is read-only. Nothing is written to the
card, the bridges, the firmware, or the boot config. A reboot changes nothing.
The worst outcome of this sheet is "the data says no" — which is itself the
answer that saves weeks of door-1/2/3 effort.

**Where:** boot the Kubuntu side (Wednesday's home — she stays up the whole
time), Blackmagic eGPU connected and authorized as usual. ~15 minutes.
Companion: `blackmagic-egpu-linux-findings.md` (the measured wall: BAR0–BAR4
all 0x0, only BAR5 + ROM BAR addressed, `Problem resizing BAR0 (-22)`).

---

## Step 0 — Make the evidence folder

```
mkdir -p ~/egpu-recon-2026-11-06
cd ~/egpu-recon-2026-11-06
```
**Verify:** `pwd` prints the folder path. Every step below drops its output here,
so the whole recon is one folder the author can hand back in one go.

## Step 1 — The PCI tree, with the bridge chain labelled

```
lspci -tv > tree.txt
lspci > list.txt
cat tree.txt
```
**Verify:** the Blackmagic card appears as `44:00.0` (Radeon Pro 580 / Ellesmere)
hanging under a chain of PCI bridges (the Thunderbolt root port and the
enclosure's internal bridges). Note the bus numbers of EVERY bridge above
44:00.0 — that chain, top to bottom, is the list Steps 3–4 interrogate.
The chain is only as wide as its narrowest window.

## Step 2 — What the card itself reports right now

```
cat /sys/bus/pci/devices/0000:44:00.0/resource > card-resource.txt
lspci -vv -s 44:00.0 > card.txt
cat card-resource.txt
```
**Verify:** this re-confirms the findings-file baseline before trusting any
later number — BAR0–BAR4 expected all `0x0000000000000000` (never assigned),
BAR5 and the ROM BAR holding the only real addresses. If BAR0 has GAINED an
address since Nov 5, stop and re-check: the wall moved and the doors change.

## Step 3 — Every bridge window in the chain

For each bridge bus number noted in Step 1 (replace `XX:YY.Z` with each one,
top of the chain first):

```
lspci -vv -s XX:YY.Z >> bridges.txt
```
Then pull the window lines out where they can be read at a glance:

```
grep -A2 -i "Memory behind bridge\|Prefetchable memory behind bridge" bridges.txt > windows.txt
cat windows.txt
```
**Verify:** each bridge shows a memory window and a prefetchable memory
window with real start–end addresses (not `---`). Record which bridge in the
chain has the SMALLEST prefetchable window — that is the narrowest point,
and it is the number door 1 (DSDT) would have to enlarge and door 2 (setpci)
would have to reprogram. BAR0 needs ≥256 MB of prefetchable space somewhere
in that chain.

## Step 4 — The physical memory map: is there free space to steal?

```
sudo cat /proc/iomem > iomem.txt
grep -i "PCI Bus" iomem.txt
```
**Verify:** the PCI Bus ranges show where address space is already claimed.
What the doors need is a genuinely FREE, suitably aligned gap of ≥256 MB
(adjacent to, or relocatable within, the Thunderbolt chain's range) that no
other device owns. If the map is fully packed around the chain, door 2 has
nowhere to put BAR0 and dies on paper — cheaply, here, instead of at a hung
machine.

## Step 5 — The ACPI tables (door 1's raw material)

```
sudo acpidump > acpi-tables.dat 2>acpidump-err.txt
ls -la /sys/firmware/acpi/tables/ > acpi-tables-list.txt
cat acpi-tables-list.txt
```
(`acpidump` ships in the `acpica-tools` package; if the command is missing,
`sudo apt install acpica-tools` first — that install is the only write in
this whole sheet, and it's a stock package, nothing custom.)
**Verify:** `acpi-tables.dat` exists and is non-trivial in size (hundreds of
KB is normal), and the tables list includes a DSDT entry. This dump is what
a DSDT override would be patched from — without it, door 1 can't even be
quoted. Note the findings file's side evidence: "ACPI VFCT table present but
broken (too short #2)" — expect quirks in these tables; that's normal for
this machine, not a recon failure.

## Step 6 — Kernel's own view of the assignment failure

```
sudo dmesg | grep -i -E "pci|bar|acpi.*crs|_CRS" > dmesg-pci.txt
grep -i "44:00" dmesg-pci.txt
```
**Verify:** the log shows the resource-assignment conversation for the
Thunderbolt chain — which windows the kernel requested, which the firmware's
`_CRS` descriptions granted, and where `pci=realloc` found no room on Nov 5.
These lines are the before-picture both remaining doors argue against.

---

## The decision this recon buys

Hand the folder back and the three doors stop being vibes:

- **A free ≥256 MB-aligned gap exists + one bridge window is the clear
  bottleneck** → door 2 (runtime `setpci`) is triable first: it is erased by
  a reboot, so the trial cannot break anything permanently; worst case is a
  hard hang and a power cycle.
- **No free gap, but the DSDT's `_CRS` for the Thunderbolt host bridge is
  visibly smaller than the hardware's real range** → door 1 (DSDT override,
  the Windows Error-12 community's "large memory" play) is the honest road,
  with a known-good boot entry kept before any attempt.
- **No gap and no `_CRS` headroom** → the wall is the hardware/firmware map
  itself; doors close on evidence, rEFInd (door 3) is not worth its boot
  risk, and the eGPU stays macOS-side with the llama.cpp Part 3B bench as
  the GPU road — decided by data, not by giving up.

**Sequencing note for the day:** this recon runs on the Kubuntu side, where
Wednesday stays alive. The macOS llama.cpp bench (`macos-ollama-gating-bench.md`,
Part 3B) needs a reboot into macOS, which takes her dark — so recon first
while she's up, bench second, in one planned dark window, not two.
