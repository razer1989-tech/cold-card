# The Blackmagic eGPU under Linux: a complete investigation

**Can you run a Blackmagic eGPU (Radeon Pro 580) under Linux on a 2019 iMac?**

Short answer: the card enumerates, the driver probes deep, and two thirds of its firmware loads — but one hardware transfer inside the card never completes, and after nine days of systematic testing (2026-11-09 to 2026-11-17) across every layer a host can reach, we could not make it complete. This repository documents *everything* — every test, every win, every failure, every subsystem we sniffed, poked, reprogrammed, or reverse-engineered, and every tool we built and why — so the next person starts from our finish line instead of our start line.

> **The one-sentence version of the wall:** the card's power-management processor (the SMU/SMC) loads the SDMA and RLC firmware instantly, then orders a hardware transfer for the graphics-core (CP) firmware — command byte `0x40` written to its own register `0x80000398` — and on a card that reached Linux cold through the Thunderbolt tunnel, that transfer never happens. The SMC waits forever. The stock driver times out and reports `SMU load firmware failed`, probe error `-22`.


---

## The hardware

| | |
|---|---|
| Host | Apple iMac19,1 (2019, 27″, Intel Coffee Lake, **no T2 chip**) |
| OS | Kubuntu 26.04, kernels 7.0.0-42 / 7.0.0-43, dual-boot with macOS Sequoia 15.7.7 |
| eGPU | Blackmagic Design eGPU — AMD Radeon Pro 580 (Ellesmere/Polaris 10, PCI `1002:67DF`, revision `0xC0`, subsystem `BDBD:A14C`), 8 GB GDDR5, VBIOS `113-C94002A1XTA-004` |
| Built-in control card | The iMac's internal Radeon Pro 570X — the **same Ellesmere silicon** (`1002:67DF`, rev `0xC5`, subsystem `106B:019E`, VBIOS `113-D0008A14GL-003`), which works perfectly under Linux. Nearly every experiment in this repo is a comparison against this twin. |
| Connection | Thunderbolt 3 (DSL6540 host controller), enclosure authorized/trusted, link Gen3 x4 |

Under macOS the enclosure works flawlessly. The silicon is fully supported by Linux's `amdgpu` driver — the same card family runs in countless PCs. Everything that blocks it lives in the gap between "generic Ellesmere" and "this enclosure, on this Mac, arriving cold."

---

## Wall #1: the card arrives with no address space (SOLVED)

Apple's firmware gives the Thunderbolt bridge chain a prefetch window too small for the card's 256 MB BAR0, so on a stock boot BAR0/BAR2 are simply never assigned (`0x0` in sysfs) and the probe dies early at `gmc_v8_0` (`Problem resizing BAR0 (-22)`).

**What solved it** (three independent ways, all verified):

1. **Runtime surgery** — [`tools/barfix2.sh`](tools/barfix2.sh): remove the Thunderbolt subtree from the PCI root port, hand-program the bridge prefetch windows into the above-4 GB root window the Mac's ACPI *does* advertise (`0x4000000000–0x7fffffffff`, 256 GB), rescan. The kernel assigns BAR0 at `0x4010000000` (256 MB), BAR2 at `0x4020000000`, BAR5 at `0x80400000`.
2. **Boot parameters** — `pci=realloc,hpmemsize=512M amdgpu.rebar=0` on the GRUB line: the kernel sizes the hotplug bridges' windows itself and assigns both BARs at the ~29 s Thunderbolt hot-add.
3. **EFI-stage placement** — our `egpushim` writer (below) placed everything perfectly before the OS loaded… and it didn't matter, for reasons in Wall #3.

Also required: `pcie_aspm=off` and `amdgpu.runpm=0` (a runtime-suspended card on this tunnel reads back all-ones and is unrecoverable without a reset), and never `amdgpu.dc=0` on this dual-Ellesmere machine (it breaks the healthy internal card's console).

## Wall #2: the SMC firmware-load stall (THE WALL)

With BARs assigned, the stock probe runs deep — IP blocks discovered, VBIOS fetched, VRAM/GART set up, UVD/VCE firmware found — then goes silent for ~10.4 s and fails: `SMU load firmware failed`. Register-level probing showed why:

- The card's SMU (System Management Unit, a LatticeMico32-core "Falcon" running AMD's SMU7 firmware, `polaris10_smc.bin`) **self-starts at power-on** and passes its own self-check (`SMU_STATUS = 0x3`, DONE+PASS) — but in its VBIOS-resident form it **never answers the host mailbox** (`SMC_RESP_0` stays 0 through every driver message).
- The driver's own start sequence is skipped because the driver's "is the SMC already running?" test (`ck_disable == 0 && PC >= 0x20100`) passes on the parked firmware. The driver proceeds to request the firmware load; the SMC never completes it.

### The virgin state and "the dance"

The card's SMC has a state nobody documented before: after a **full enclosure power-cycle** (both Thunderbolt *and* AC pulled while the host is down — the SMC rides through warm host reboots with its state intact), with no driver allowed to POST the card first, the SMC sits in its **mask-ROM download-wait state**: mailbox armed (`RESP = 1`), PC executing in ROM space, waiting for a host download like a brand-new card.

In that state you can upload the standard `polaris10_smc.bin` from userspace — if you do it the way this card demands. The naive upload loses ~30,490 of 32,490 dwords through the register-indirect SRAM path (a write-loss pattern we measured repeatedly); a **verify-and-repair storm** (blast, then rewrite every dword explicitly) lands it byte-clean. Clear `SMU_STATUS`, send `PPSMC_MSG_Test`, and the card answers in 0.2 s with `SMU_STATUS = 0x3` — DONE+PASS — and its program counter roams the SRAM execution range exactly like the healthy twin. **The standard SMC firmware runs on this card.** That was the first breakthrough (2026-11-08 20:51).

It was not enough. Every subsequent load request — ours or the stock driver's — stalls at the same place.

### The load, block by block

The SMC loads nine firmware images for the ASIC's sub-blocks. Using a hand-built userspace loader that replicates the driver's `LoadUcodes` flow exactly (table of contents in VRAM, address handshake messages, one mask), the map is exact:

| Firmware block | Result |
|---|---|
| SDMA0, SDMA1 (DMA engines) | ✅ loads instantly |
| RLC_G (run-list controller) | ✅ loads instantly |
| CP_CE (graphics command processor) | ❌ **never completes** |
| CP_PFP, CP_ME, CP_MEC + jump tables | ⛔ never reached — the handler stalls at CE |

`UcodeLoadStatus` reaches `0x6` (SDMA pair) within 0.5 s and never moves again. The healthy twin reads `0x5FE` — all nine.

### Inside the SMC: the 0x40 order

We disassembled the SMC firmware (hand-built LM32 disassembler, [`tools/lm32/`](tools/lm32/)) and decoded the stall. During a load, the SMC walks a work list; certain items order a hardware transfer by writing a **command byte to its own CPU-block register `0x80000398`** and polling for the hardware to clear it:

- SDMA items use byte `0x30`, RLC uses `0x20` — these clear instantly.
- The CP-path item uses byte **`0x40`** — on this card it is **never cleared**. The SMC sits in a 3-instruction poll loop (`PC 0x2df5c ↔ 0x2df64`) forever. We confirmed the stuck byte live on hardware (`0x80000398 = 0x40` during every stall; `0x0` on the twin).

We also tested whether the completion signal was merely *lost*: a loader variant cleared the byte from the host whenever it sat unchanged for 5 s. The SMC immediately **re-issued the order** and kept waiting — after clearing, it checks the actual operation, finds it undone, and retries. **The transfer itself never occurs.** The signal is not lost; the work is not done.

### Everything that was ruled out — by experiment, not argument

Each of these was tested on hardware, most of them several ways:

- **The SMC firmware build.** Apple's own SMC firmware (build `0x1f0e00`, extracted from the macOS kernel collection — see "Sniffing macOS" below) was uploaded and run. It stalls at the identical loop (its PCs sit `0xCB0` lower). The SMC is innocent, any build.
- **The driver sequence.** Apple's macOS load path is a 1:1 port of Linux's `polaris10_smumgr.c` — same messages, same order, same waits. Our userspace loader replicates the driver's flow byte-for-byte (verified against source, including the TOC entry layout, the RLC digest-size exclusion, and the `flags=1` unhalt bits). The stock driver itself, on a clean boot, lands register-for-register where our loader lands.
- **The load table (TOC).** Single-entry, 9-entry driver-faithful, and Apple's exact 11-entry variant (with SMU_SK and MEC_STORAGE entries, mask `0x447E`) — identical outcome.
- **VRAM staging integrity.** 21 versions of staging tests (`vramstage_test1–21`) chased a real Thunderbolt write-combining artifact: through the BAR0 mmap, the first 64-byte line of every 256-byte segment mis-verifies. We eventually proved (chunked double-pass staging, verified per chunk; separate-process reads; the SMC as oracle) that the payload the SMC was handed was good — and the wedge was identical with provably clean staging. Staging is exonerated.
- **Registers — all of them.** ~1,160 registers compared between the cold eGPU and the warm twin, in both virgin and post-driver-attempt states (BAR5 ASIC space, the SMC CPU block, the SMC SRAM soft registers, and the SMU hardware block `0xE0003xxx`): every difference is chip identity data, a *consequence* of one card finishing and the other stalling, or the one clock-gating pair below. **No writable missing precondition exists anywhere a host can read.**
- **Clock gating.** Apple's one real driver difference — a 75-entry MGCG clock-gating table `amdgpu` skips for Polaris — was read first on the cold card: it already held Apple's values (the card programs them itself at power-on). Written anyway and held through a load: no change.
- **Resets.** Enclosure power-cycle, secondary-bus reset via `setpci`, and a `GRBM_SOFT_RESET` pulse with the kernel's own mask — the CP block was already in its post-reset state (halted, idle, `CP_ME_CNTL = 0x15000000`); a reset is not the missing step.
- **The CP direct-load ports.** The VI-era register ports for loading CP microcode directly (`0x3E050…0x3E06C`) are not live RAM windows in this state — writes don't read back. No bypass there.
- **Bus mastering, BAR addresses, FB window base, message IDs/order, digest handling** — each varied, each exonerated.
- **POST state.** The driver's own POST (`ASIC_Init`) runs on this card every stock boot; loads on those POSTed cards stall identically. A patched driver that POSTs and stops *before* touching the SMC (so our dance could start the firmware on a kernel-POSTed, DRAM-trained card — the "frontier" state) also stalls identically.

## Wall #3: nothing survives the handover from firmware to OS

A parallel investigation asked whether the card could be prepared *before* Linux — Apple's own boot chain initializes the internal twin somehow, and the twin is the one that works. We built **egpushim**, a UEFI application (audited to exactly one PCI config-write call site) that grew from a read-only logger through placement writer, Thunderbolt probes, an ICM claim attempt, and a config-space snapshot tool. Findings, all measured:

- At the EFI stage the eGPU is enumerated but **never initialized**: `COMMAND = 0x0000`, BAR0/BAR2 unassigned, ROM disabled. Apple's four bundled Radeon EFI drivers (carved out of the iMac's `IM191.fd` firmware image — Ellesmere's is GUID `f0140fc0…`) are **display-only**: no AtomBIOS interpreter, no ASIC init, no firmware loading, no SMC contact in any of them. The internal card's EFI life comes from its legacy VBIOS via Apple's CSM path. **The eGPU receives no EFI-stage initialization at all.**
- The shim's full placement (25 writes: bridge windows + GPU BARs + `COMMAND = 0x0007`) **provably existed** at the EFI stage — read back clean 10 s later, inside EFI — and **provably vanished** before the kernel's first read. The card is **absent from the kernel's initial PCI scan** and appears only at **~29.5 s** via Thunderbolt hotplug, virgin. The author's ear caught it first: the enclosure's fans stop exactly at GRUB's "loading initial ramdisk" line. The PCIe tunnel is down at the `ExitBootServices` handover; the enclosure power-cycles the card; **no EFI-stage state can survive.**
- It isn't EFI driver teardown: disconnecting all ten EFI Thunderbolt drivers changed nothing — the tunnel is held by the DSL6540's internal firmware under Apple's **native connection manager** (Apple's EFI stack programs ring 0 directly; there is **no Intel ICM running** on this machine — `FW_STS ICM_EN = 0` — so there is nothing to "claim").
- The tunnel's path state is **not in host-readable config space** either: borrowing Apple's live ring 0, we completed the first config exchanges of the project and captured the host switch's header — and every real port and hop space refused with `INVALID_CONFIG_SPACE`. The path state lives inside the Thunderbolt chip's firmware. There is nothing to snapshot and replay.

So the twin's advantage is precisely: *its own VBIOS POST, executed by Apple's firmware, and never reset afterward.* Linux meets the eGPU only after a power cycle, in the one state where the CP transfer never fires.

## The userspace POST, and why it eats cards

The last road was executing the card's VBIOS startup program (`ASIC_Init` AtomBIOS tables) from Linux userspace, with a faithful port of the kernel's Atom parser ([`tools/roms/atomrun.py`](tools/roms/atomrun.py)). It never completed — and the failure is itself a finding:

- The run wedges at a display-PHY poll — but the register it polls reads healthy (`0xF1`, waited-for bit already clear) *before* the run and **all-ones after it**. The run's own writes drive the card's register file dark mid-startup; the poll then waits forever on a block that no longer answers.
- A write-capped bisect (stop the startup after exactly *N* writes) showed the darkness is **aperture-wide** (display, graphics, SMC mailbox, and CP sentinels all read `0xFFFFFFFF`) and **early**: already present at write **217 of 237**, before the display power-down writes we first suspected. The executor's delays are genuinely honoured (it really sleeps) and no poll loop had even been entered — those theories were checked and cleared.
- The control experiment — the same capped startup on the **internal twin** — **wedged the entire machine** within seconds (fans to maximum, keyboard dead, log never reached disk; recovered by power-off, and the twin was fine after a normal boot, as EFI POSTs it fresh). Userspace POST is destructive on both cards, differently. The kernel's execution of the identical tables completes every boot; what differs about its environment remains unknown, and since kernel-POSTed cards stall at the `0x40` wall anyway, a completed userspace POST was never likely to be the answer.

## Sniffing macOS (the reference implementation)

The author's direction — "read what the working OS does with the card" — was followed to the bottom:

- Captured the full macOS registry state (`ioreg`), display/PCI profiler data, and the AMD kext inventory for both cards.
- macOS kexts are codeless on disk; we carved `AMD9500Controller`, `AMDRadeonX4000`, `AMDRadeonX4000HWLibs`, and `AMDSupport` out of the Sequoia kernel collection with a Python fileset carver, extracted Apple's SMC firmware build `0x1f0e00` (stored dword-byte-swapped in `AMDRadeonX4000HWLibs`), and disassembled its load walker: **instruction-for-instruction identical** to Linux's build. The builds differ in data tables, not transfer logic — and Apple's build stalls identically (above).
- Traced the whole macOS start path (controller → CAIL `initializeAdapter`: EarlyASICInit → PostVBIOS → Initialize → ReserveMCAddressRange → FixChipsetBugs → NoBiosInitializeAdapter → ASICSetup): **no Apple-only step** before the SMC load — no extra messages, no extra register writes, no Thunderbolt awareness anywhere in any binary.
- Diffed Apple's published per-card data (`PP_StaticMappingInfo1–6`): five of six byte-identical between the cards; the sixth is a per-card address Apple's driver computes at enumeration, present in neither card's ROM. **There is no macOS-side data table fed to this card that Linux lacks.**

## Patching the firmware: cryptographically closed

The one-byte idea — re-route the SMC's CP command byte `0x40` to a byte whose transfer completes — died at the SMC's boot self-check. A one-byte-patched image uploads byte-clean and is **rejected**: `SMU_STATUS` DONE=1, **PASS=0**, image never runs. A desk dig over 13 genuine SMC images showed the header's `Digest[5]` is a **keyed, authenticated AMD digest** verified by the mask ROM against a hardware-resident key — no checksum, CRC, or hash recomputation matches, and no fix-up exists. The manager's firmware cannot be altered. (The card's only other persistent writable store, its VBIOS ROM chip, was assessed as the last "alter the design" target: nothing in nine days of measurement implicates its tables — the kernel executes them to completion every boot and the load still stalls — and a bad flash bricks the card, with recovery requiring a hardware programmer inside a sealed enclosure. We did not flash it.)

## What we built (tools)

Every tool exists because a specific question needed an instrument. Full catalogue with rationale: **[TOOLS.md](TOOLS.md)**. The highlights:

- **`frontier.sh`** — the whole end-state procedure as one command (wake/BAR fix → POST-and-stop module → the dance → the loader → GUI restore), built because the procedure crosses a grey-screen phase where the operator *cannot see the console*: one command typed while the screen is visible, the script checks its own gates, logs everything, and restores the desktop itself on success *and* abort.
- **The dance (`smcdance`)** — userspace SMC firmware upload with verify-and-repair, the tool that first ran standard firmware on this card.
- **The loaders (`smcload4–16`)** — faithful userspace replications of the driver's `LoadUcodes` flow, evolving through every protocol defect found (staging, flags, digest size, address base, TOC composition) until the full 9-entry load partially completed — the first firmware this SMC ever accepted under Linux.
- **`egpushim`** — the UEFI shim family (logger → planner → writer → TB probe → ICM claim → config snapshot), one audited config-write call site throughout.
- **A patched `amdgpu`** — three guards over the investigation: skip the config reset for this subsystem, POST-and-stop before IP init, and force the full SMC start. Built via the headers-tree (DKMS-style) route after the in-tree build produced modules the x86 loader refused.
- **`atomrun`** — a userspace AtomBIOS executor (faithful kernel `atom.c` port), plus its write-capped bisect variant.
- **`asicdiff` / `driverdiff`** — stability-filtered register census of both cards (~1,160 registers), the tool that closed the register road.
- **An LM32 disassembler** — hand-built from the Sleigh spec, used to decode the SMC's stall loop and Apple's walker.

## The end state of knowledge

Measured and closed: BAR assignment (solved), the EFI stage (no init exists; nothing survives the handover), the ICM claim (no ICM), config snapshot/replay (state not host-readable), kexec (a second kernel inherits the same stalled state — closed on the user's own correct objection), the driver sequence (a faithful port on both OSes), the SMC firmware (both builds stall; unforgeable), ~1,160 registers in two states (no missing precondition), the load protocol (replicated to the byte), VRAM staging (exonerated), clock gating (self-programmed), resets (already reset), the CP direct ports (dead), the userspace POST (destroys both cards), and macOS's published data (nothing extra).

What remains unexplained is a single hardware data movement inside the card: the SMC orders the CP-family transfer and, on a card that arrived cold through the Thunderbolt tunnel, the hardware never performs it — while performing the identical class of transfer for SDMA and RLC in the same load, on the same card, in the same second. No host-visible state explains it. Whatever macOS's boot path gives this card, it is given before any OS driver runs, it does not survive a restart, it is not in any table, firmware image, or register we can read — and the enclosure's designers arranged for the card to be power-cycled at exactly the boundary where it would have to be carried across.

If you have vendor documentation for the Blackmagic enclosure's power/clock sequencing, or an Ellesmere SMU erratum that matches "CP ucode transfer never completes on a cold card," you know something nine days of measurement couldn't reach. Open an issue.

---

## Repository layout

- **[FINDINGS.md](FINDINGS.md)** — the complete chronological technical record, 2026-11-05 → 2026-11-17: every run, every log reading, every correction and retraction, in the order it happened. This README is the summary; FINDINGS.md is the evidence.
- **[TOOLS.md](TOOLS.md)** — every tool, what it does, and why it was built.
- **[docs/](docs/)** — the deep-dive reports: the VBIOS ROM teardown, Apple's init/start-sequence diffs, the SMC self-check analysis, the ICM decision brief, the firmware recon sheets.
- **[tools/](tools/)** — the tools themselves: the frontier runner and loaders' harness scripts, the register census, the AtomBIOS executor, the LM32 disassembler, and the egpushim source and spec.

*Hardware note: everything here was measured on one iMac19,1 + one Blackmagic eGPU. The internal-twin comparisons are this machine's gift — most systems don't have one. Numbers (BAR maps, timings, PCs) are from the record in FINDINGS.md.*

---

## Conclusion: this is what anti-ownership engineering looks like

Strip away nine days of register dumps and the story is simple.

The graphics chip in this enclosure is a bog-standard AMD Radeon Pro 580. Linux supports it. Windows supports it. The driver that fails to load it under Linux is, line for line, the same driver that loads it under macOS — we know, because we carved Apple's driver out of their own firmware and read it. The firmware protocol is published in open-source kernel code. There is no technical mystery about how this card is supposed to work, and no technical reason it shouldn't work here.

It doesn't work because it was **built not to**.

Look at the layers, because each one was a decision somebody made:

- **The card's own manager firmware is cryptographically signed against its owner.** We tried changing a single byte — one byte, in a device sitting on our own desk, that we paid for — and the card's boot ROM rejected it, because the firmware carries a keyed AMD digest checked against a key fused into the hardware that no owner will ever hold. You cannot modify the software that runs the hardware you bought. That is not security. There is no attacker in this story. The only party locked out is the owner.
- **The boot chain erases the card at the exact moment another operating system takes over.** Apple's firmware never initializes this card, and at the handover boundary the Thunderbolt tunnel drops and the enclosure power-cycles it — so the card can only ever be met cold, in the one state where it will not fully start under a foreign OS. State that would let it work under Linux cannot survive the crossing *by construction*. A designed lock does not need a padlock; it just needs to make sure the key never exists on the other side of the door.
- **The one OS it works under is the one the enclosure was sold to serve.** Inside macOS, flawless. Anywhere else, a nine-day forensic investigation that ends at a hardware transfer no host is permitted to complete. The product was not engineered to be an eGPU. It was engineered to be an *Apple accessory* that happens to contain an eGPU.

This is the modern shape of anti-repair, anti-ownership design. Nothing is visibly locked. There is no sticker that says "Linux forbidden," no error message that admits intent — just a stack of individually deniable engineering choices that sum, precisely, to: *you may use what you bought only in the way you were sold it.* Right-to-repair is not only about screws and schematics. It is about whether the thing on your desk answers to you or to its manufacturer, permanently, in silicon. This card answers to its manufacturer.

We document it here so that the next owner doesn't have to spend nine days learning that the wall was the product.
