# EFI Shim Spec — Blackmagic eGPU on iMac19,1 under Linux

**Status:** drafted 2026-11-08 at the author's direction. **v0.1 logger BUILT
2026-11-08** (the author's go): GNU-EFI, QEMU/OVMF-verified end to end (see the
kit's TEST-EVIDENCE.md; two bugs found by the VM test and fixed before
shipping). Kit: `files/egpu-shim-logger-v0.1.zip`, SHA-256 of the shipped
BOOTX64.EFI `40432d34299adbe9dc5ac6431963d90454bc6c8620318db881437188f24aa034`.
Hardware run (run sheet Step 3) still pending the author's session. Every later
version below remains a separate go/no-go decision for the author.
**Host:** iMac19,1 (2019 27″ Intel — **no T2 chip, and no T1 either: the
regular iMac line had no Apple security chip until the 2020 model**),
macOS Sequoia native + Kubuntu 26.04 dual-boot (Option-key; Apple's ESP
untouched; Kubuntu has its own ESP).
**Correction (2026-11-08, the author's catch):** the research mission and the
first draft of this spec mislabelled the 19,1 as a T2 machine. The author
established on 2026-11-05 that it is not (a pasted generic AI answer got
this wrong the same way). Consequences, both directions: GOOD — no T2
Startup Security gate, no secure-boot policy standing between a USB EFI
volume and chainloading, so the pre-boot side of this project is *easier*
on this machine than on the T2 donors. CAUTION — every donor in §3 worked
on T2-generation firmware, so their addresses, windows, and chain layouts
are evidence about a *sibling* platform, never numbers to copy onto this
one. Everything load-bearing on the 19,1 gets measured on the 19,1 —
which is precisely what the v0.1 logger (§6) is for.
**Device:** Blackmagic eGPU, Radeon Pro 580 (Ellesmere, `1002:67df`,
subsystem Blackmagic `a14c`), Thunderbolt 3, permanently authorized
(enrolled) under Linux.
**Companions:** `blackmagic-egpu-linux-findings.md` (the measured wall),
`egpu-firmware-skirt-recon-sheet.md` (the Linux-side read-only recon — still
unrun, and still the cheapest first data), research report
`~/workspace/research_notes/efi-preboot-pci-bar-egpu-20261108-2150/report.md`
(the 2026-11-08 internet scan summarized in §3).

---

## 1. The problem, in one paragraph

Under Kubuntu the card enumerates at `44:00.0`, amdgpu fetches its VBIOS,
loads firmware, and discovers its IP blocks — then dies at
`Problem resizing BAR0 (-22)` → `gmc_v8_0 sw_init failed -19`, because
`/sys/bus/pci/devices/0000:44:00.0/resource` shows **BAR0–BAR4 all `0x0`
(never assigned)**; only BAR5 (256 KB) and the ROM BAR (128 KB) received
addresses from the Thunderbolt tunnel window. Apple's firmware/ACPI gives
the Thunderbolt bridge chain a prefetchable window with no room for the
card's ≥256 MB BAR0. `pci=realloc` was verified live on the GRUB cmdline
and changed nothing. The card, the tunnel, and the Linux driver are all
healthy; the address space simply is not assigned. Full evidence:
findings file, 2026-11-05.

## 2. The idea

A small **chainloaded EFI application** — NOT a firmware replacement —
that runs after Apple's firmware and before GRUB/Linux:

```
Apple UEFI firmware (untouched, never written to)
  → Option-key boot from a removable USB EFI volume   ← the shim lives here
    → shim (this spec)
      → chainloads Kubuntu's GRUB from Kubuntu's own ESP
        → Linux → amdgpu
```

Why the timing is the whole point (established by the research in §3):
Linux drivers do not read hardware registers for resource state — they
read the kernel's cached `struct resource` tree, built at the kernel's
first PCI scan. Post-boot `setpci` writes are therefore ignored unless
the resource tree is also patched. A write that lands **before** that
first scan should be built into the tree from the start. That inference
is the technical bet of this project. It is labelled inference: no source
has tested it on a Mac.

## 3. What the internet scan found (2026-11-08) — and what it changes

**Exact match: none.** No public report, repo, or thread describes a
custom EFI/UEFI pre-boot program that reallocates Thunderbolt bridge
windows or assigns BARs to rescue an eGPU — on any Mac, any machine,
any OS. If this works, it is (publicly) first-of-kind. Treat every
precedent below as an analogue, not a proof.

| Precedent | What it actually does | OS / target | Confidence |
|---|---|---|---|
| **t2-gmux-uefi** | Chainloaded UEFI loader on T2 MacBook Pros (Startup Security = No Security). Right before chainloading Windows: measures the *real* size of every 64-bit BAR (firmware's numbers not trusted), finds a free size-aligned slot in the root bridge's above-4 GB window, disables decode, rewrites BARs **and the prefetchable windows of all bridges above the GPU**, read-back verifies, rolls back on mismatch | Windows only; *internal* dGPU, no Thunderbolt in path | Single-author 2026 repo, no independent corroboration |
| **t2-egpu-linux** | Runtime fix on Mac mini 2018 (T2): `setpci` moves every bridge's prefetch window to `0x4010000000–0x401FFFFFFF` + BAR0 into it, **plus a kernel module patching the kernel resource tree** (`bar0->parent` included), then loads amdgpu with `rebar=0`. Diagnosis: firmware gives the whole TB domain only 224 MB prefetch vs the 256 MB BAR0 need. The chosen address sits inside the host bridge's 64-bit window `[0x4000000000–0x7fffffffff]` — on that machine `_CRS` was NOT the constraint | Linux (Ubuntu/T2 kernel); AKiTiO Node Titan + RX 6800 | Single-author 2026, uncorroborated; notes AI-assisted drafting |
| **LKML, Mar 2026** | The *identical failure* on Mac mini 2018 + Razer Core X (NVIDIA and AMD cards): enumerates, authorized, `BAR [256 MB pref] can't assign; no space`, BAR at 0. Failed there: `pci=realloc=on`, `hpmemsize`/`hpiosize`/`hpbussize`, ASPM off, `nommconf`, `noaer`. Maintainer (Ilpo Järvinen) pointed to new in-kernel PCI resource patches; still reproducible on 6.17 as of Mar 24 | Linux; open kernel bug | Primary source (kernel list) |
| **ReBarUEFI** (xCuri0) | Flashed DXE driver hooking `PciHostBridgeResourceAllocationProtocol` *during* firmware enumeration so firmware's own allocator assigns the resized BAR. Author first proved the concept as a chainloaded app (set BAR + bridge windows, then load bootmgfw.efi) and warns: writing BAR/bridge values after host-bridge allocation crashes unless the window is moved too | Win + Linux; internal GPU on flashable PC boards — an iMac BootROM cannot be patched this way | Established project, widely used |
| **DIY eGPU Setup 1.x** (nando4) | DOS-era pre-boot PCI compaction: generated `setpci` scripts + optional in-memory DSDT override, chainloads Windows. Fixed Error 12 widely, incl. on Macs — but compaction *alone* often still failed: the 32-bit root-bridge `_CRS` capped allocation regardless of register writes | Windows only; not UEFI (MBR-era) | Years of user reports |
| **EFI Shell `mm`** (2012 rMBP) | `startup.nsh` under rEFIt wrote a PCI bridge *control bit* pre-boot; Windows kept it. Proves EFI-stage config-space writes on a Mac persist into an OS — control bits only, never windows/BARs | Windows; TB1 eGPU | Period forum reports |
| **16× RTX 5060 Ti server** | Standalone EFI app sets ReBAR size + clears old BAR addresses, returns to GRUB; **Linux then allocates** BARs and bridge windows itself. Board's host windows were already adequate | Linux; not an eGPU | Single detailed report |

**Design changes this research forces into the spec:**

1. **Tunnel timing is (mostly) answered.** For a device plugged in at
   boot, Apple's EFI creates the Thunderbolt tunnel and the kernel
   discovers the pre-existing tunnel (Lukas Wunner, kernel lists). The
   card enumerating at `44:00.0` under Linux is itself evidence the
   tunnel predates Linux. The logger still must verify the card is
   *visible at EFI stage* — that is its single most important datum.
2. **The iMac's root-bridge window is the key unknown.** On the Mac
   mini the host window was 256 GB and not the constraint; the
   Thunderbolt chain's windows were. On Windows, `_CRS` *did* cap
   everything. Which regime the iMac19,1 is in decides whether an EFI
   shim can work at all. The logger must capture the data to answer
   this (ACPI dump + root-bridge apertures, §6).
3. **Measure every BAR; trust no firmware size.** t2-gmux-uefi found
   Apple firmware mis-reporting a BAR's size (1 MB claimed vs 2 MB
   real), which alone caused Code 12. Any future write version must
   size BARs by probing, not by reading firmware tables.
4. **Linux may re-shrink the windows.** A kernel patch in the v7.1
   line ("PCI: Prevent shrinking bridge window from its required
   size") documents Linux shrinking a TB eGPU's downstream window to
   fit the root port's window during hotplug resource distribution.
   An EFI-enlarged window can be re-derived on a Thunderbolt (re)scan.
   Mitigations are kernel-side (recent kernel, patches), not EFI-side
   — so a **recent-kernel test is a parallel cheap experiment**, not
   an afterthought.
5. **The runtime route is a real competitor.** t2-egpu-linux's
   approach (setpci + resource-tree patch module) targets this exact
   failure class on a sibling T2 Mac and needs no EFI work at all.
   This spec does not bury that: §9 keeps it as the comparison route,
   and the EFI project should be re-justified against it once the
   recon data exists.

Sources (full report in the research folder): t2-gmux-uefi
github.com/Osaul4ik/t2-gmux-uefi · t2-egpu-linux
github.com/philmcneely/t2-egpu-linux · LKML thread
lkml.indiana.edu/2603.1/13905.html · ReBarUEFI
github.com/xCuri0/ReBarUEFI


## 4. The donor-adaptation framing (the author's formulation, 2026-11-08)

The author: *"We just need to redo whatever the T2 guys did, for the
pre-T2 infrastructure."* Substantially right, with the seams named:

- **From t2-gmux-uefi, take the engine:** a chainloaded UEFI loader that,
  after firmware enumeration and before the OS, measures real BAR sizes,
  finds a free aligned slot in the root bridge's above-4 GB window,
  disables decode, rewrites BARs + bridge prefetch windows, verifies by
  read-back, and rolls back on mismatch. That engine is the shim.
- **Re-aim it at a different target:** gmux aimed at an *internal* dGPU
  behind one root port, chainloading *Windows*. Ours aims at a GPU at
  the end of a *Thunderbolt bridge chain* (root port → TB upstream →
  TB downstream → `44:00.0`, exact BDFs to be measured), chainloading
  *Linux GRUB*. The chain is longer, and every bridge in it must nest
  the enlarged window — the chain is only as wide as its narrowest link.
- **From t2-egpu-linux, take the Linux lessons:** the concrete window
  arithmetic for a TB chain on Apple firmware, the `rebar=0` guard (do
  not let amdgpu re-resize BAR0 past the windows we built), and the
  proof-of-concept that a ~256 MB-aligned placement inside the host
  bridge's large 64-bit window is the shape of a working answer on
  Apple firmware of this era. Take none of its addresses.
- **What the pre-T2 platform changes:** no Startup Security setting to
  lower (that gate is a T2 feature — the 19,1 has no equivalent standing
  in the way); the boot path is the classic Option-key / rEFInd-era
  path that the 2012 EFI-shell precedent already proved on a Mac.
  What does NOT transfer automatically: T2 machines' firmware PCI
  allocation behaviour may differ from the 19,1's in exactly the
  window sizes this project lives on. The logger exists to close
  that gap with this machine's own numbers before any write is designed.

## 5. Design principles (all versions, non-negotiable)

1. **Apple's firmware and Apple's ESP are never written to.** The shim
   lives only on removable USB media until a write version has earned
   promotion, and even then only onto Kubuntu's own ESP.
2. **Read-only first.** v0.1 performs zero PCI config-space writes —
   including no BAR-size probing (sizing writes all-ones to a BAR and
   restores it; that is still a write, so it belongs to a later,
   explicitly-approved version).
3. **Log before action, flush after every section.** Any future version
   that writes registers writes its intent to the log file and flushes
   (UEFI `File.Flush`) *before* touching hardware, and logs read-back
   values after. A hang must still leave a complete account of what
   was attempted, readable by pulling the USB stick.
4. **Fail safe, never loop.** On any error — missing chainload target,
   missing config, unexpected device tree — the shim logs the error,
   waits for a keypress if a console is present, and exits to the
   firmware boot manager. Removing the USB stick always restores the
   machine's completely stock boot behaviour.
5. **Rollback is designed in from the first write version** (the
   t2-gmux pattern): save every register before changing it; verify by
   read-back; on any mismatch, restore all saved values, log, and
   chainload anyway (a failed attempt must still boot the machine).
6. **Config, not code, for machine specifics.** Target BDFs, window
   addresses, and the chainload path live in a plain-text config file
   beside the binary, so iterating on measurements never requires a
   rebuild — and a wrong config can be fixed from any other machine.
7. **One variable per version.** Logger → planner → single-write →
   full placement. No version skips a gate (§8).

## 6. v0.1 — the read-only logger (the first build this spec asks for)

### 6.1 Purpose

Answer, with the iMac19,1's own pre-boot numbers, the four questions
every later version depends on:

- **Q1.** Is the Blackmagic card visible at EFI stage at all (i.e. did
  Apple firmware create the Thunderbolt tunnel and enumerate the card
  before any OS)? If it is absent pre-boot, the whole EFI approach
  dies here, cheaply.
- **Q2.** What is the exact bridge chain above the card at EFI stage —
  BDFs, and each bridge's memory / prefetchable base+limit (including
  upper-32 registers)?
- **Q3.** How large is the root bridge's decode aperture (the host
  window everything must fit inside)? Answered two ways: the PCI root
  bridge resource descriptors via UEFI protocols, and the ACPI tables
  dumped raw for offline `_CRS` parsing.
- **Q4.** Does a free, suitably-aligned ≥256 MB region exist inside
  that aperture, given everything firmware already placed there?
  (Answered offline from Q2+Q3 data — the logger only collects.)

### 6.2 Form and toolchain

- A single UEFI application (`egpushim.efi`), C, built with EDK II
  (preferred; the protocols in §6.4 are native to it) or GNU-EFI if
  EDK II setup fights back. Decision at build time; the spec's
  behavioural requirements are identical either way.
- Developed and smoke-tested first under QEMU/OVMF on the Kubuntu side
  (logger logic, file output, chainload mechanics) before it ever
  runs on the iMac — the iMac never sees a build that hasn't booted
  a VM cleanly.
- Deployed as `EFI/BOOT/BOOTX64.EFI` on a dedicated FAT32 USB stick,
  booted via the Option-key menu. Stick contents:
  `EFI/BOOT/BOOTX64.EFI` (the logger), `shim.cfg` (plain-text config:
  chainload target path, log verbosity, console-pause flag),
  `LOGS/` (created by the logger).

### 6.3 Boot flow

1. Open/create this boot's log file FIRST (`LOGS/egpushim-YYYYMMDD-HHMMSS.log`,
   timestamp from the EFI real-time clock; if the clock read fails,
   fall back to a counter filename). Flush after the header.
2. Log firmware vendor/revision, UEFI revision, and the shim's own
   version string and build mode (`MODE=LOGGER, WRITES=NONE`).
3. Dump the EFI memory map (raw, plus a decoded summary by type).
4. Enumerate PCI (see §6.4) and log the full tree.
5. Dump ACPI tables (see §6.5).
6. Flush and close all files.
7. Chainload the GRUB binary named in `shim.cfg` (expected:
   Kubuntu's `grubx64.efi` on Kubuntu's ESP, located by scanning
   handles for the configured path; if not found, try the fallback
   path on the USB stick itself if present). On chainload failure:
   log, keypress-wait, exit to firmware (§5.4).

### 6.4 PCI enumeration — what gets logged, per device

Access via `EFI_PCI_IO_PROTOCOL` where available, with segment/bus/
device/function recorded for every node; bridges additionally via their
header type. For EVERY device/function found:

- Vendor/device ID, class code, header type, command/status registers
- All six BAR registers, raw values as firmware left them (no probing)
- Expansion ROM BAR, raw
- Capability list walk: PCIe capability — link status/speed/width;
  Resizable BAR capability *presence and its size-selector field as
  read only* (recorded, never changed)

For every BRIDGE additionally:

- Primary / secondary / subordinate bus numbers
- I/O base+limit, memory base+limit, prefetchable base+limit
  **including the upper-32 prefetchable registers** (this is where
  the ≥256 MB question lives)

And specifically flagged in the log (by ID match, all read-only):

- The Blackmagic card: `1002:67df`, subsystem `a14c` — PRESENT or
  ABSENT at EFI stage (Q1's answer, printed as its own line:
  `EGPU_AT_EFI_STAGE: YES/NO`), plus its audio function (`1002:aaf0`)
- The internal Ellesmere at what Linux calls `01:00.0` — the control
  sample: a GPU Apple firmware *does* fully assign, whose BARs and
  bridge windows show what "working" looks like on this firmware
- Thunderbolt host/NHI controllers and every bridge on the path to
  the card, assembled into an explicit ordered chain (Q2's answer)

### 6.5 ACPI capture

- Locate the RSDP via the EFI configuration table; walk RSDT/XSDT.
- For each table: log signature, length, checksum status — and copy
  the raw table to `LOGS/` as its own file (`acpi-DSDT.aml`, etc.),
  DSDT and SSDTs included. Parsing (incl. the root bridge `_CRS`)
  happens OFFLINE on the Kubuntu side with standard acpica tools,
  never in the shim.

### 6.6 Hard invariants (code-auditable before any hardware run)

- The source contains **no PCI config-space write call** in logger
  mode — verified by grep + review, and the binary's log header
  states the mode. File writes go only to the logger's own volume.
- No NVRAM variable writes. No BootOrder changes. The shim runs only
  when a human boots it from the Option-key menu.
- Total added boot time target: under ~5 seconds excluding any
  configured console pause (default: no pause).

### 6.7 Acceptance criteria for v0.1

1. Three consecutive Option-key boots via the logger → Kubuntu boots
   normally each time; Wednesday comes up untouched.
2. Kubuntu's `lspci` tree after a logger boot is IDENTICAL to the
   stock baseline (proves the logger changed nothing).
3. One macOS boot interleaved, unaffected. USB stick removed → stock
   boot behaviour, byte for byte.
4. The three logs + ACPI dumps are complete and parse offline, and
   Q1–Q4 (§6.1) can each be answered from them in writing.
5. The answers get filed as an addendum to the findings file before
   any v0.2 discussion.

## 7. Analysis the logger data feeds (offline, no new hardware risk)

- Diff three allocations of the same hardware: **EFI-stage** (this
  logger) vs **macOS** (IORegistry/System Information BAR values) vs
  **Linux** (the unrun recon sheet's `lspci`/`/proc/iomem` capture).
  Where macOS places the card is the strongest hint of where Linux
  placement should go — macOS demonstrably makes this firmware +
  this card work.
- Parse the dumped DSDT `_CRS` for the root bridge(s): is the host
  window Mac-mini-like (huge, not the constraint) or Windows-like
  (the cap)? This is the go/no-go datum for the whole write project.
- Produce the placement plan ON PAPER: candidate window, alignment,
  which bridges need widening, expected BAR values — reviewed before
  any code that writes exists.

## 8. Version ladder and gates (later versions are sketches, not specs)

- **v0.2 — Planner (still zero writes).** The shim computes the §7
  placement plan at runtime from live values, logs the exact register
  writes it *would* perform, and chainloads untouched. Gate: planner
  output matches the hand-derived plan from v0.1 data.
- **Gate A — the runtime cross-check.** Before any EFI write version,
  the same plan is proven at runtime from Linux (the recon sheet's
  door 2, `setpci` + the resource-tree lesson from t2-egpu-linux —
  at runtime, a resource-tree patch or manual bind may be needed;
  at EFI stage it should not be). If the plan cannot work at runtime
  where debugging is easy, it will not work pre-boot where debugging
  is blind. A reboot erases a failed runtime trial; nothing here is
  persistent.
- **v0.3 — First write build.** Applies ONLY the bridge-window
  widening (no BAR assignment), with §5.5 rollback, read-back
  verification, and full logging. Gate: Kubuntu boots, `lspci` shows
  the widened windows, card state otherwise unchanged, three clean
  boots.
- **v0.4 — Full placement.** Adds BAR assignment (+ `rebar` guard on
  the kernel side) per the proven plan. Success definition: BAR0
  holds a real address in `/sys/.../resource`, amdgpu completes init
  (`Fatal error during GPU init` gone), and the card survives a
  Thunderbolt rescan / Plasma session without the kernel re-shrinking
  the windows (the §3.4 risk — if it bites, the fight moves to the
  recent-kernel/patch track, and the shim's job becomes holding the
  placement *plus* documenting the shrink).
- **Promotion rule.** Only after v0.4 survives a week of daily boots
  may the shim move from USB to Kubuntu's own ESP — as an *additional*
  boot entry, never replacing the stock GRUB entry.

## 9. The competing route, kept honest

The EFI shim is not the only live candidate, and this spec should not
be read as a verdict against the others:

- **Runtime route (t2-egpu-linux adapted):** setpci + resource-tree
  kernel module from a systemd unit. Pros: debuggable, no pre-boot
  risk, precedent on a sibling machine. Cons: a custom kernel module
  to maintain across kernel updates; runs on T2-machine numbers until
  re-measured here.
- **Recent-kernel route:** the LKML maintainer's pointer — the v7.1
  resource patches may improve or fix allocation outright. Cheapest
  test of all: boot a current mainline kernel carrying the new PCI
  resource patches once the recon exists. If the kernel fixes it,
  both other routes are moot.
- **EFI shim (this spec):** the only route that fixes the state
  *before* the kernel's resource tree is built, and the only one
  that, if it works, is a single self-contained binary with no
  kernel-version coupling. Highest build effort, blindest debugging.

Sequencing the author already has on file still holds: **Linux-side recon
first (free, read-only, Wednesday stays up), logger second, placement
plan third — and only then a decision between runtime proof, kernel
test, and EFI writes, made on this machine's own numbers.**

## 10. Risk register (v0.1 scope)

| Risk | v0.1 exposure | Mitigation |
|---|---|---|
| Brick / unbootable iMac | None credible: no firmware, ESP, or register writes; removable media only | Option-key stock paths untouched; remove stick = stock |
| Logger bug hangs pre-boot | Possible (new code) | QEMU/OVMF smoke test first; fail-safe exits (§5.4); power cycle recovers |
| Card absent at EFI stage | Informational, not a failure of the logger | It IS Q1 — a NO answer closes the EFI door on evidence and redirects effort to §9's runtime route |
| `_CRS` window too small on the 19,1 | Informational | Q3 answers it from data; a small window redirects to the DSDT-override door (findings file, lever 2) instead |
| Donor numbers don't transfer from T2 machines | Assumed throughout | Nothing is copied; every address in later versions derives from this machine's logger + recon data |

---

*Drafted 2026-11-08 (Wednesday, cloud) from the measured findings of
2026-11-05, the recon sheet of 2026-11-06, and the internet scan of
2026-11-08. Next action when the author says go: run the Linux-side recon
sheet (15 read-only minutes, Kubuntu side), then build v0.1 to §6.*
