# Tools catalogue

Every tool in this repository exists because a specific question needed an instrument. They are listed in roughly the order they were built. Most are single-purpose, several are dangerous in the specific ways noted, and all of them were run on the one machine described in the README. Read FINDINGS.md for the runs themselves.

## Boot and PCI plumbing

### `tools/barfix2.sh`
**What:** removes the Thunderbolt subtree from the root port (`00:1c.4`), hand-programs the bridge prefetch windows into the above-4 GB root window, rescans, so the kernel assigns the eGPU's BARs (best map: BAR0 `0x4010000000`, BAR2 `0x4020000000`, BAR5 `0x80400000`).
**Why:** Apple firmware leaves BAR0/BAR2 unassigned (Wall #1). The kernel ignores setpci-programmed windows on a rescan of *existing* bridges, but a full remove+rescan forces re-derivation — with `pci=realloc` armed it assigns everything itself.
**Cautions:** run it *before* any probe or register peek (an unassigned-BAR card reads garbage), and never after the SMC dance — map surgery disturbs a running SMC (measured: a post-dance barfix2 parked a healthy firmware).

### `tools/egpu-boot-setup.sh`
**What:** one-time, idempotent Kubuntu hardening: strips `pci=realloc`/`hpmemsize=*` from the GRUB cmdline (so the kernel can't redo firmware-stage placement), adds `pcie_aspm=off` + `amdgpu.runpm=0`, and blacklists `sdhci_pci`/`sdhci` + rebuilds initramfs.
**Why:** runtime suspend on this tunnel is unrecoverable without a reset (the card "forgets everything"), and the iMac's built-in Broadcom SD reader wedges Linux boots when a card is seated (its interrupt never arrives; the SDHCI register-dump loop can wallpaper the console).

## The SMC instruments

### The dance — `smcdance` (driven by `frontier.sh`, step 5)
**What:** uploads `polaris10_smc.bin` to the SMC's SRAM (`0x20000`) over the register-indirect path, verifies it, and starts it: blast write, then an explicit rewrite of every dword (the "repair storm"), clear `SMU_STATUS`, send `PPSMC_MSG_Test`, then the ownership tail (clear FIRMWARE_FLAGS → pulse the SMC reset → wait for flags).
**Why:** the card's resident SMC firmware never answers the host mailbox, and the driver's own start is skipped by its "already running" heuristic. The dance only works from the **virgin state** (enclosure power-cycled, no driver POST first), because a running firmware's SRAM is sealed by protection mode, and any reset permanently disarms the boot ROM's mailbox. Pass 1 of the upload loses ~30,490 of 32,490 dwords through the indirect path — the verify-and-repair loop is not paranoia, it is the mechanism that makes the upload land byte-clean.
**Proof it works:** DONE+PASS (`SMU_STATUS 0x3`), Test answered in 0.2 s, PC roaming the SRAM range — and the identical procedure run on the internal twin works perfectly, which is what validated the *method* and isolated the fault to this card.

### The loaders — `smcload4` … `smcload16` (driven by `frontier.sh`, step 7)
**What:** userspace replications of the driver's `smu7_request_smu_load_fw`: stage firmware images into VRAM, build the `SMU_DRAMData_TOC` table of contents, send `SMU_DRAM_ADDR` → `DRV_DRAM_ADDR` → `LoadUcodes`, poll `UcodeLoadStatus`.
**Why:** to ask the SMC to load firmware *without* the driver — the driver's probe tramples the SMC state (its failure-path teardown kills the firmware's message service, proven by same-boot A/B), so the load had to be tested against a dance-started firmware on a POSTed card.
**Evolution, each version one measured defect:** v4 whole-file staging → payload-only staging with the header ABI pinned from source; v5 chunked double-pass VRAM staging (the Thunderbolt write-combining artifact); v6 TOC parity fixes (RLC `flags=1`, digest-size exclusion, status zeroing); v7 live `FB_LOCATION`-based addresses (theory died — the base was right all along); v8–v10 the full 9-entry driver replication (this is where SDMA0/SDMA1 first loaded); v11 mask-selectable bisect (CE isolated as the wall; RLC completion proven at mask `0x406`); v12 CP-state dump during the stall; v13/v14 the completion **assist** (host clears the stuck `0x40` byte — the SMC re-issues it; transfer proven absent, not unreported); v15 the state harvester; v16 Apple's exact 11-entry TOC.
**End state:** a byte-faithful replication of both OSes' load flows. It loads SDMA+RLC and stalls at CP — exactly like the stock driver.

### `tools/frontier-v50.sh` (the frontier runner; earlier versions v3.2–v4.9)
**What:** the entire end-state procedure as **one command**: wake/BAR check (barfix2 first if needed) → register peek → hard module gate (the POST-and-stop `amdgpu` must be the one in `updates/`) → modprobe (the displays go grey here) → the dance (hard gate on `PASS=1`) → bus-mastering on → the loader → GUI restore on *every* exit path. Everything logs to a file.
**Why:** the procedure crosses a phase where the operator **cannot see the console** (the eGPU displays go grey after the modprobe). Any flow that needs typing at that point is a design failure — the operator types exactly one command while the screen is visible, and the script checks its own gates and brings the desktop back by itself. This rule ("no blind typing") was learned the hard way and shaped every runner in this repo.
**Gate lesson baked in:** the dance gate matches `PASS=1` explicitly — an earlier version grepped for the substring `PASS`, which also matches `PASS=0`, and waved a rejected firmware through.

## The patched driver

Three guards were built into `amdgpu` over the investigation (subsystem-guarded to `BDBD:A14C`, so no other card is affected):

1. **No-reset guard** (`vi_need_reset_on_init` → false): the driver's config reset cannot complete over the Thunderbolt path and leaves the ASIC's register aperture dead.
2. **POST-and-stop guard** (return `-ENODEV` right after the POST block in `amdgpu_device_init`): the kernel performs the VBIOS POST — DRAM training included — and stops *before* IP init, leaving the SMC untouched in download-wait. This manufactures the "frontier state" (POSTed + trained + virgin SMC) deterministically.
3. **Force-start guard** (`polaris10_start_smu` ignores the "already running" heuristic): makes every bind re-run the full verified SMC start. Proved the driver's own upload is clean under reset — and that the load still stalls afterward.

**Build lesson:** build against the headers tree (the DKMS route). The hand-prepared in-tree build produced modules the strict x86 loader refused (`Invalid relocation target, existing value is nonzero` — ftrace patchable-entry slots pre-filled); the same source built via `KDIR=/lib/modules/$(uname -r)/build` (with one symlink for the trace header's root-relative include) loads cleanly. Always verify with `modinfo -n amdgpu` that the `updates/` module is the one being loaded *before* counting a probe as a patch test — one "patched" probe in the record was silently the stock module.

## The register census

### `tools/asicdiff.py` (+ `tools/driverdiff.sh`)
**What:** reads ~1,160 registers per card (BAR5 sweeps, SMC-indirect sweeps, SRAM soft registers, the SMU hardware block), each twice 0.5 s apart, and reports only values that are *stable within a card but different across cards*. `driverdiff.sh` is the one-command wrapper: root check, barfix2, wake, modprobe, settle, diff, GUI restore.
**Why:** to answer "what state does the working card have that the cold card lacks?" exhaustively instead of by hunch. The v2 wake block and all-FF guard exist because run 1 read a runtime-suspended card and produced 629 garbage "differences." The v3 SMU-block sweep was the last unread register territory.
**Verdict it delivered:** virgin == virgin (8 differences, all chip identity); post-attempt differences are all consequences of the stall, chip identity, or the one exonerated clock-gating pair. The register road is closed — by this tool.

## The AtomBIOS executor

### `tools/roms/atomrun.py` (+ `tools/atomrun_cap.py`, `tools/postprobe.sh`, `tools/postbisect.sh`, `tools/posttwin.sh`)
**What:** a faithful userspace port of the kernel's Atom parser (`atom.c`) that executes a VBIOS ROM's command tables against live hardware via BAR5 (with a shadow-register dry mode). `atomrun_cap.py` adds `--max-writes` to stop a run after exactly *N* register writes. The three shell scripts are one-command harnesses: peek sentinels before, run capped, peek after, restore the GUI, log everything.
**Why:** to run the card's own startup program (`ASIC_Init`) without a driver — the one POST this card never gets under Linux — and later, to bisect *which write* in that program drives the card's register file dark.
**Hard-won lessons:** a real bug shipped in it for weeks (a fabricated `DST2SRC` field-resolution table — every partial-field write was potentially to the wrong field) that dry runs could not reveal and code diffs missed because the table was assumed, never compared; the kernel's own debug narration (`amdgpu_atom_debug=1`) found it in one log. Even fixed, the executor's runs darken the card aperture-wide by write 217, and the twin control wedged the whole machine. **Do not run userspace POST on a card you care about, on a machine you need.** Included because the failure mode is a finding, and because the next person's first idea will be this one.

## The EFI shim — `tools/shim/` (spec + source)

### `egpushim` v0.1 → v0.7.4
**What:** a UEFI application that boots from removable media before GRUB. It grew rung by rung: read-only logger (dump the PCI tree, bridge windows, memory map, ACPI tables) → placement planner → placement **writer** (bridge windows + GPU BARs + `COMMAND`, under a save/read-back/restore-all protocol, armed only by config) → Thunderbolt driver-disconnect probe → ICM claim attempt (kernel-faithful ring-0 `DRIVER_READY` exchange) → config-space snapshot over a *borrowed* Apple ring 0.
**Why:** to test whether the card could be prepared before Linux — and then, when placement proved perfect and still vanished, to find out *why* nothing survives the handover.
**Design invariants:** exactly **one** PCI config-write call site in the source (audited at every version); the writer refuses unless its filtered write list is exactly the expected size; originals are logged and flushed before the first write; first mismatch restores everything.
**What it proved:** EFI-stage placement is stable inside EFI (10 s recheck) and gone before the kernel's first scan; EFI driver teardown is irrelevant (the tunnel is firmware-held); there is no ICM on this machine (Apple's EFI runs a native connection manager on ring 0); the tunnel's path state is not in host-readable config space. The EFI road is closed — by this tool, from both ends.
**Operational rule it taught:** never boot the shim from a reader behind the Thunderbolt tree it rewrites (the volume dies mid-write when the root-port window moves). Built-in SD slot or a rear USB-A port only.

## The firmware workbench

### `tools/lm32/lm32dis.py`
**What:** a hand-built disassembler for the LatticeMico32 core inside AMD's SMU7, from the Ghidra LM32 Sleigh spec (62 opcodes).
**Why:** to read the SMC's stall instead of guessing at it. It decoded the work-list walker (command bytes `0x30`/`0x20`/`0x40` to SMC register `0x80000398`), identified the old `0x33824` wedge as a deliberate `bi 0` panic trap, and verified that Apple's SMC build runs the identical walker logic shifted by `0xCB0`.

### The macOS carve (described in FINDINGS.md; reports in `docs/`)
**What:** a Python fileset carver that extracted the AMD kexts from the Sequoia kernel collection, plus the byte-unswap that recovered Apple's SMC firmware image `0x1f0e00` from `AMDRadeonX4000HWLibs`, plus a full carve of the iMac's `IM191.fd` firmware (460 files) that located Apple's four display-only Radeon EFI drivers.
**Why:** the author's standing rule — read what the working OS does. It exonerated the SMC firmware, the driver sequence, and the EFI stage, which is most of the map.

## Small instruments

- **`regpeek5.py`** — BAR5 register reads via the sysfs resource file (with the PCIe indirect window for offsets past 256 KB). The referee for nearly every experiment; its sentinel set (`0x12344` display, `0x8010` GRBM, `0x250` SMC mailbox, `0x86d8` CP) is the bisect's verdict panel.
- **`smustat.py`** — one-shot SMC health readout (status, load status, PC samples, Test ping) for either card.
- **`cpprobe.py`** — proved the CP direct-load ports are not live RAM windows in the frontier state.
- **`vramprobe` / `vramstage_test1–21`** — the VRAM staging investigation: write-combining, the 256-byte segment-leader artifact, per-line/per-dword/fenced/acknowledged writes, read-pattern forensics. Twenty-one versions to prove the staging was innocent.
- **`smc*` one-shots** (`smckick`, `smcfirst`, `smcpeek`, `smcpoke`, `smchold`, `smcrestart`, `smcprime`, `smcdone`) — each a single question asked of the SMC: the ownership-tail kick (worked — the PC roamed), the reset-hold test (the reset genuinely gates the core), the first-speaker test in a pristine boot (found the virgin state), the image-corruption attempt (protection mode seals a running firmware's SRAM — writes never land, held or not).
