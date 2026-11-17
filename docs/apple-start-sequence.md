# Apple start sequence — phase 2: controller/CAIL trace + bypass tracks

**Date:** 2026-11-16 (evening). **Scope:** desk-only RE of the carved macOS AMD kexts, hunting for any ASIC-side step between device start and the SMC `LoadUcodes` call that amdgpu does not perform — plus three bypass tracks opened by the author's expanded mandate ("dig deep — even if it is a working bypass/overwrite"). **Phase 1** (`apple-init-diff.md`) already established: the HWLibs SMC-load cluster is a 1:1 port of Linux `polaris10_smumgr.c`/`smu7_smumgr.c`, and the MGCG table theory died on hardware (frontier v4.6).

**Binaries** (all offsets below are *vm* = carved-file offset + the KC `__TEXT` fileoff of that binary: AMD9500Controller +0x226000, AMDRadeonX4000 +0x2ca000, AMDRadeonX4000HWLibs +0x705000, AMDSupport +0x13be6000). **Tooling:** capstone full-text sweeps of all four binaries (HWLibs = 504,761 real instructions, 3,191 call targets), string indexes with vm-corrected addresses, LM32 disassembly of both SMC images (`files/lm32/lm32dis.py`). Kernel baselines: `vi.c`, `gfx_v8_0.c`, `smu7_smumgr.c`, `polaris10_smumgr.c`, `smu_ucode_xfer_vi.h` (torvalds/linux, fetched this session; the ID/mask header verified identical across v4.14, v5.10, and master).

**Headline verdict:** the proven macOS start path is a faithful port of amdgpu's order — **no Apple-only step exists between device start and the SMC load in any call chain that could be proven from the carves.** One init-shaped function that also pulses the ASIC soft-reset could not be proven to run at start (or not to). The bypass tracks produced one genuinely promising experiment: a **one-byte patch to the SMC image re-routing the CP transfer command (0x40 → 0x30)**, gated by a one-byte patch-viability test. Details and evidence below.

---

## 1. The macOS start path, as traced

**AMD9500Controller** contains no CAIL/Falcon strings at all — it is the OS-facing shell (IOService matching, power management, display plumbing). Its only `GRBM_STATUS` code reference outside the known is-idle poll (slice 0x31a3c, phase 1) is one further read site (vm 0x243fd7), a status *read* by context; a read cannot gate a transfer and it was not individually traced.

**AMDRadeonX4000** carries the CAIL bridge strings: `setupCail` (vm 0x3c79b3), `initCail` (0x3c79bd), `cailPowerUp` (0x3c7a4a), `cail_properties` (0x3ce952), `CAIL_DisableUVDPowerGating` (0x3cfece). Its only two `GRBM_SOFT_RESET` (dword 0x2008) immediates were individually inspected: vm 0x344dac is a generic register-accessor thunk (loads a function pointer from the object vtable +0x268 and tail-calls it with reg = 0x2008, value = 1); vm 0x36bfeb passes 0x2008 as a **buffer size** to a memset-like helper — a false positive. X4000 does no start-path soft-reset programming of its own.

**HWLibs — the CAIL wrapper class** (`AMDRadeonHWLibsCAIL`; constructor vm 0x773920, vtable ≈ 0x79A8B0; factory 0x7738f0 allocates a 0x20498-byte context). Its `initializeAdapter` method (**F1, vm 0x773a30**) executes, in order:

1. Store the MCIL (register-access) callbacks into the context.
2. Fetch the VBIOS image via IORegistry property `ATY,bin_image` (vtable call +0x2b8) and check the 55/AA signature.
3. **CAILEarlyASICInit** (cail.a 0x707fd1; called at 0x773c5e).
4. **CAILPostVBIOS** (wrapper 0x706763; called at 0x773e26; subcalls 0x71418a then 0x71442d) — the software POST, the step that substitutes for the VBIOS POST this card never gets on a Linux boot.
5. **CAILInitialize** (0x7067d3; at 0x773f39).
6. **CAILQueryASICInfo** (0x706bab), **CAILQuerySystemInfo** (0x7071a8).
7. **CAILReserveMCAddressRange** (0x707b03) + local helpers 0x774180/0x774280 (queryCAILReservedBlock).
8. **F2 (vm 0x774490)**, called at 0x774149.
9. IRI service setup (`ATY,CAIL_IRI`; helper 0x7079d5 ×4).

**F2 (vm 0x774490)** performs, in order: **CAILFixChipsetBugs** (0x70723b) → **CAILNoBiosInitializeAdapter** (0x706a70; arguments taken from ctx+0x20400) → **CAIL_ASICSetup** (0x7076c7) → CAILQueryASICInfo → CAILQuerySystemInfo. The "NoBios" branch is the not-POSTed-by-BIOS bring-up — exactly this card's situation — and it is a normal, always-present part of Apple's start, not an eGPU special case.

**CAIL_ASICSetup (0x7076c7)**: family-gated substeps (family queries via helper 0x70b7f0 with IDs 0xc2/0x10f/0x112): calls 0x71e430, one indirect call via [rbx+0xfc0], then **0x712c4a unconditionally** (at 0x707794), then family-gated 0x72b146 / 0x72b130. No SMC messaging exists at the CAIL layer — CAIL talks to ASIC registers only, through the MCIL callbacks; all PPSMC traffic lives in the powerplay cluster (phase 1).

After CAIL init, the powerplay stack starts exactly as amdgpu's does (phase 1): `is_smc_ram_running` check → dance if needed → Test (0x100) → TOC upload → LoadUcodes (0x254). Nothing sits between CAIL ASIC setup and that sequence in either stack.

## 2. The five audit questions

**(a) Any SMC messages before LoadUcodes, beyond the shared sequence? NO.** Census evidence: the SMC-indirect immediates `ixSMU_STATUS` (0xe0003088) and `ixFIRMWARE_FLAGS` (0x3f000) appear in HWLibs **only** inside the five per-ASIC SMU clusters (Iceland ≈0x7e27xx, Tonga ≈0x7fa8xx/0x7fb8xx, Fiji 0x817fxx–0x8182xx, Polaris10 0x838bxx–0x8392xx, Polaris22 0x84fff9–0x850axx). The mailbox BAR5 offsets never appear as constants anywhere (all messaging goes through the register-ID layer), and the Polaris10 cluster's send helpers (0x836e12 send-with-parameter, 0x836f0e) belong to the ported smumgr sequence, whose first message is Test — identical to Linux. There is no start/enable/power PPSMC message in either stack before LoadUcodes.

**(b) Any ASIC soft-reset programming in the start path? MACHINERY EXISTS; start-path use UNPROVEN.** CAIL contains a complete soft-reset implementation: function **0x71f5cc** translates a hung-block bitmask ([rdi+8]) into GRBM/SRBM reset masks; the pulse code at **0x71f711+** reads **GRBM_SOFT_RESET (dword 0x2008)** through the register helpers (read 0x70a1ff / write 0x70a2a4 / delay 0x716dec), ORs the mask, writes, delays, reads back, ANDs the mask off, writes back — a textbook pulse — then repeats the identical pulse on **dword 0x398** in CAIL's register numbering (see the numbering caveat in §7). A pre-helper (**0x71f544**) toggles bits in registers 0x186/0x180 through the same helpers. These are composed by **0x71ef11** (helpers 0x71f1d5, 0x71f444, 0x71f544 in sequence, twice, plus 0x71f823). The wrapper-class methods that use this machinery — `CAILResetEngine` (string ref at method vm 0x775f02), `CAILPerEngineReset` (0x775fd8), `CAILSoftResetHungBlocks` (0x776159) — are **not** called by initializeAdapter or F2; they serve the runtime hang/recovery path. The composite 0x71ef11 has exactly one caller chain: from 0x71312d, inside function **0x712fc8** — which also calls the PostVBIOS subcall 0x71442d, a NoBiosInitializeAdapter dispatch target (0x71554d), and CAIL_ASICSetup (0x7076c7), plus an indirect call via [rbx+0x1238]. That call mix is *init-shaped*, but 0x712fc8 has **no direct callers and no static pointers** in the carve (LINKEDIT/symtab were not carved, and the function would be reached via a runtime-filled table), so whether it is the start orchestrator or the reset/recovery orchestrator **cannot be proven from the available binaries**. This is the one open thread — and it converts into a cheap hardware experiment (§6, B1) rather than more desk work. One false positive closed along the way: `or ecx, 0x80000000` at 0x70731e (inside CAILFixChipsetBugs' region) is a chipset-ID flag computation (compares PCI IDs 0x157c1022 / 0x524310b9), not an SMC write.

**(c) Any SMC-indirect writes outside the load cluster? NO.** Beyond the census in (a): the three remaining `0x80000000` immediates in the CAIL region were individually inspected — 0x70ae4f and 0x70b096 OR a flag bit into a value stored in an MCIL data structure ([ctx+0x328]), and 0x70df51 stores 0x80000000 into a struct field ([r13+0x24]). All three are data-structure flag writes; no SMC_IND_INDEX/DATA sequence follows any of them.

**(d) Any PCIe link-speed/retrain programming? NO.** CAILFixChipsetBugs (0x70723b) — the only init-path function with "chipset" in its remit — reads capability flags from the context and matches **host** chipset IDs (0x157c1022 = AMD host bridge, 0x524310b9 = ALi), setting flag bits in the CAIL context. It performs no device register writes at all in its traced body. No link-control programming was found anywhere in the start path.

**(e) Any Thunderbolt/enclosure interaction? NO.** String search across all four carved binaries for thunderbolt / eGPU / enclosure / removable: **zero hits**. The macOS AMD stack is enclosure-blind; there is no enclosure-side step to replicate.

## 3. Deltas vs amdgpu — the honest list

Kernel baseline (fetched sources): `vi_set_ip_blocks` (vi.c:2053+) orders the Polaris10 blocks vi_common → gmc → ih → … → pp_smu → gfx; the SMC firmware load happens inside the SMU block's hw_init, before gfx_v8_0 hw_init consumes the CP. In the probe path, amdgpu performs **no** GRBM/SRBM soft-reset pulse before the SMC load: gfx_v8_0's `GRBM_SOFT_RESET` writes live in the hang-recovery op (`gfx_v8_0_soft_reset`, gfx_v8_0.c:4911+, GRBM mask = RLC|GFX|CP|CPF|CPC|CPG, SRBM mask = GRBM|SEM, pulse at 4927–4973) and an RLC-only pulse in CP teardown (4066). The project's own stock-driver probe (Nov 16) already proved the full stock init — including `amdgpu_device_asic_init` (the AtomBIOS POST) — lands register-for-register where our replication lands.

Genuine Apple-vs-amdgpu differences found by this phase and phase 1, complete list:

1. **TOC contents and LoadUcodes mask** — Apple adds SMU_SK (id 14) and MEC_STORAGE (id 35); mask 0x447E vs Linux's 0x47E. Assessed in §5: structurally incapable of unblocking CE; one cheap replay run at most.
2. **MGCG clock-gating table application** — phase 1; **falsified on hardware** (frontier v4.6: the cold card already holds the values).
3. **The unproven soft-reset orchestrator** (function 0x712fc8 → composite 0x71ef11) — §2(b). If it runs at start, Apple pulses GRBM/SRBM soft reset during bring-up and amdgpu never does at probe. Unproven either way; testable from Linux for almost nothing (§6, B1).

**Verdict on the original question: a clean negative.** In every call chain provable from the carves, Apple's start sequence is a faithful port of amdgpu's order, with no ASIC-side step between device start and the SMC load that amdgpu lacks. The wall is not a missing host step that this trace can see — which is precisely what makes the bypass tracks below the main event.

## 4. Track 1 — SMC firmware patch surface

We control the image the dance uploads (proven with Apple's 0x1f0e00 build, frontier v4.5). This section decodes the load walker completely, in both images, and assesses what a patched image could and could not do. Linux-image SRAM addresses are used; Apple's build is the identical code at **−0xcb0** (verified instruction-for-instruction below, including the frontier-v4.5 stall PCs).

### 4.1 Walker anatomy

The transfer routine sits at SRAM **0x2de78** (Apple: 0x2d1c8 region; its command blocks at 0x2d260–0x2d330). It is the shared body of **five loader tasks** whose entry wrappers sit at SRAM 0x20120–0x201bc: each wrapper saves context (calli 0x2028c), loads **r1 = its task number (1, 2, 3, 4, 5)**, calls 0x2de78, then returns through a context-restore path (0x20318 / 0x2039c). So the routine's "state" (r5) is a property of the *task*, not of the firmware entry: the LoadUcodes handler distributes TOC entries across five per-class task queues, and each task issues its class's hardware command.

Each work item is 8 bytes: **+0** a dword address tag; **+4..+7** four channel-match flag bytes. For tasks whose state ∉ {1, 3}, a pre-step (0x2de80) first zeroes the flag bytes, then scans the 4-entry channel-base table at SMC **0x80000338**, comparing each entry (low 2 bits cleared — they are tag bits) against the item's address; on a match at index *i* it sets flag byte *i*. Tasks 1 and 3 skip the pre-step (their flags are written by the enqueueing handler).

Dispatch (0x2defc), per task state:

- **State 2 → command byte 0x30** (block 0x2df04; issue RMW at 0x2df1c–0x2df24, poll loop 0x2df24–0x2df2c).
- **State 4 → command byte 0x40** (block 0x2df3c; issue at 0x2df54, poll loop **0x2df5c–0x2df64** — the observed stall).
- **State 5 → command byte 0x20** (block 0x2df78; issue at 0x2df90, poll 0x2df98–0x2dfa0).

All three blocks do the same thing: read SMC register **0x80000398**, clear the low byte, OR in the command byte, write back, then poll `lw / andi 0xff / bne` until the low byte reads **0** — completion means *the hardware cleared the byte*. Then `bi 0x2e14c` (advance the queue cursor by 8, return). **There is no verification read, checksum, or status write anywhere in these blocks** — the SMC trusts the byte completely.

- **Any other state (incl. tasks 1 and 3) → the CSR path** (0x2dfa8): read 0x80000398 and test bit 0x100. If set: manipulate a LM32 CSR with the bit's value, issue byte **0x06**, poll until the low byte **equals 6**; then re-test bit 0x100 (still set → advance; clear → set a CSR flag and advance). If bit 0x100 is clear (0x2e018): walk the item's flag bytes in order; for the first set byte *k* (0–3), load channel base `table[0x80000338 + 4k]` into **CSR 16+k** via `wcsr`, issue byte **k+1** (blocks at 0x2e020/0x2e060/0x2e0a0/0x2e0e0), and poll until the low byte **equals k+1**. If no flag byte is set: issue byte **0x05** (0x2e118), poll until it equals 5.

So there are two transfer classes: the **byte-command class** (0x20/0x30/0x40 — fire a single command at the SMC's DMA command register and wait for the hardware to clear it) and the **CSR channel class** (bytes 1–6 — the SMC programs one of four DMA channel base registers, CSRs 16–19, from the 0x80000338 table, and the command byte *persists* at its value on completion). Apple's image is identical in structure and order: 0x30 issue at 0x2d26c (poll 0x2d274), state-4 → 0x40 issue at **0x2d2a4**, poll at **0x2d2ac–0x2d2b4** — precisely the PCs frontier v4.5 caught Apple's SMC flickering between — then 0x20 at 0x2d2e0, then the same bit-0x100/byte-6 fall-through at 0x2d2f8+.

### 4.2 Which firmware routes to which byte

- **CP_CE (id 3) → state 4 / byte 0x40 — PROVEN.** The bisect record: mask 0xE (SDMA0, SDMA1, CE) sets status bits for the SDMA pair and stalls; the SMC's parked PC is inside the state-4 poll loop in both firmware builds; the handler walks entries in ID order, so CE is the first state-4 entry reached.
- **SDMA0 (1), SDMA1 (2), RLC_G (10) → not state 4 — PROVEN** (mask 0x406 = SDMA pair + RLC completes fully, RESP 0x1, ping answered).
- **CP_PFP (4), CP_ME (5), CP_MEC (6), JT1 (7), JT2 (8) → state 4 — INFERENCE, labelled as such.** They were never individually bisected (CE blocks the walk first), but they are one RAM complex (the CP family's ucode stores), they share one task class in every observable, and no other byte-command class remains for them. The internal GPU — same SMC architecture, POSTed — completes all of them, so all five task classes function on a healthy card.
- **SMU_SK (14, Apple TOC only) → CSR class by construction**: it is an SMC-internal image (the skeleton that runs the SMC when the full image is not resident); its "transfer" is SRAM-side, which is what the CSR channel class exists for. It is never reached in our stalls: CE (id 3) precedes it in the walk and task 4 never finishes.

### 4.3 Why the v4.2 host assist could never work — now proven from code

The transfer routine's complete instruction inventory touches only: the command register 0x80000398, the 0x80000338 table (read + tag-clearing stores), CSRs, and the item's own flag bytes. **It never writes UcodeLoadStatus.** The status bits are set by a hardware-completion path outside this routine. Host-clearing the byte (smcload14) fakes the *poll exit*, not the *completion*: the entry is never marked complete at the level that owns the status, the walker re-issues the 0x40 order (observed: 8 re-issues, status frozen at 0x6 for 120 s), and the load can never report success. Any "fake the wait" idea has to recon with this: the wait is not where completion lives.

### 4.4 Patch options, concretely

**P0 — the gate experiment (do first, costs one dance).** Patch exactly one byte in dead padding of the image, dance it, and check DONE+PASS + Test ping + PC roaming. This decides whether the host-upload path verifies the image (SMU7-era images carry a checksum field, not a PSP-style signature — but no patched image has ever been danced on this bench, so this is untested, not known). **If P0 fails, the entire patch track is dead and that is a clean, valuable negative.** If the check is a header checksum, the patch tooling must recompute it; the dance reads only version/size/array_off from the header, so the check — if any — is the SMC's own, over the uploaded SRAM.

**P1 — skip the polls (cosmetic; relocates the failure).** Replace the three poll-loop `bne`s (Linux 0x2df2c, 0x2df64, 0x2dfa0) with fall-throughs. The SMC would issue every command and move on; the handler would complete and the SMC would go idle. What would **still be missing**: (i) the **CP RAM contents** — the 0x40 engine never executes its copy on this card, and the uncleared byte would sit at 0x40 colliding with subsequent commands; (ii) the **UcodeLoadStatus CP bits**, which are set by hardware completion, not by the poll — so even the stock driver's `check_fw_load_finish` would still time out unless the completion path is also patched. End state if forced through: the driver unhalts a CP whose ME/PFP/MEC execute power-on garbage → CP faults at ring test. Diagnostic value only.

**P2 — re-route the CP command byte (the one patch with a physical path to a working card).** One byte: at Linux **0x2df54**, `ori r2, r2, 0x40` → `0x30` (or 0x20). The question it gambles on: does the command byte select the **transfer engine** (with the destination carried by the work item / TOC entry), or the **destination block**? If engine: CP images get copied by the class that demonstrably completes transfers on this card (the SDMA/RLC classes complete), and the CP family loads. If destination: the images land in the wrong RAM and CP stays empty. **The decisive readout is not the status bits** (they may set under either outcome, since completion is reported per item) — it is whether a subsequent full driver bind passes CP init and the gfx ring test. Protocol: patched dance → mask 0x8 (CE only) → full mask 0x5FE → bind the poststop module and read dmesg. Blast radius is one virgin-cycle frontier run: a wrong-RAM write lands in SDMA/RLC SRAM, erased by the next power cycle. This is the cheapest decisive experiment in the entire project right now.

**P3 — fake UcodeLoadStatus in the SMC (dead end, alone).** Patching the completion path to set status bits without hardware completion is P1 with extra steps: the host check passes, CP RAM is still empty, and the only alternate way to fill CP RAM — the direct ADDR/DATA ports — was **proven dead on this card** (cpprobe, frontier v4.0: writes never land; the GFX register shell answers while its interior is gated). P3 is only meaningful if P2 works, in which case it is unnecessary.

**Track 1 bottom line:** P0 gates, P2 is the bet, P1/P3 are cosmetics that move the failure downstream. No patch to the SMC's *waiting* logic can put bytes into CP RAM; only re-routing the *transfer* can.

## 5. Track 2 — replaying Apple's exact TOC

Apple's TOC, recovered instruction-exact from HWLibs' merged `SmuPolaris10_StartSMU`/`RequestSMULoadFW` (vm 0x838a19; populate helper 0x839f4f; SMU_Entry = 28 B, TOC header 8 B). Populate order:

**10 (RLC_G), 3 (CP_CE), 4 (CP_PFP), 5 (CP_ME), 6 (CP_MEC), 7 (JT1), 8 (JT2), 1 (SDMA0), 2 (SDMA1), 14 (SMU_SK)**

The default mask **0x47E** is stored at 0x83901b; the code then scans the TOC backward for an entry with id 14 and, if found, sets the mask to **0x447E** (at 0x839470) — i.e. 0x47E plus the SMU_SK bit 0x4000; the JT bits (0x180) are in neither mask, matching Linux (the SMC loads the jump tables under MEC's bit). `DRV_DRAM_ADDR_HI` (msg 0x250) is sent at 0x83947f, after the mask fix-up. Entry **35 (MEC_STORAGE)** is populated at 0x839599, gated by a runtime predicate (call 0x793299 returning ≠ 0), after the DRV HI message but before the TOC-upload helper (0x798231, called at 0x8395af) and the LoadUcodes send (msg 0x254 at 0x8395bf). ID namespace per `smu_ucode_xfer_vi.h` (verified identical v4.14/v5.10/master): SMU 0, SDMA0 1, SDMA1 2, CE 3, PFP 4, ME 5, MEC 6, JT1 7, JT2 8, GMCON_RENG 9, RLC_G 10, IH_REG_RESTORE 11, VBIOS 12, MISC_METADATA 13, **SMU_SK 14**, RLC_SCRATCH 32, RLC_SRM_ARAM 33, RLC_SRM_DRAM 34, **MEC_STORAGE 35**, VBIOS_PARAMETERS 36; masks are (1 << id).

Linux comparison: bare-metal TOC = the first nine entries only, in the same order (`smu7_smumgr.c`); Polaris mask 0x47E. MEC_STORAGE is added only under the VF predicate (`hwmgr->not_vf` false) — never on bare metal. SMU_SK never appears in Linux's TOC at all.

**Assessment: replay is buildable in an afternoon (our loader already constructs the TOC dword-by-dword), and the prior is LOW, for two structural reasons.** (1) MEC_STORAGE has **no mask bit** and is not walked by LoadUcodes — it is a TOC-resident pointer the SMC consults later (MEC power-gating storage); it cannot influence the CE transfer. (2) SMU_SK (id 14) is walked **after** every CP entry in ID order and serviced by a different task class; the stall at CE precedes it. The one mechanism that could have made SK matter — the skeleton being the code that executes CP transfers — is refuted by the internal GPU, which loads CP fine under Linux's SK-less TOC on the same SMC architecture. Worth exactly one frontier run because it is nearly free; predicted outcome: no change. Do not build theory on it.

## 6. Track 3 — amdgpu-side bypasses

**B1 — soft-reset pulse before the load (best module-side candidate).** Insert a GRBM_SOFT_RESET pulse (CP|CPF|CPC|CPG|GFX bits, the gfx_v8_0.c:4919–4925 mask) plus the SRBM pulse (GRBM|SEM) into the poststop module's probe path — or, cheaper, into the userspace loader before the dance — on the theory that Apple's unproven orchestrator (0x712fc8 → 0x71ef11) does exactly this during bring-up. It is the only module-side lever that changes the ASIC state task 4 faces. Honest prior: low-to-moderate — the CP was already found in its textbook load-ready state (CP_ME_CNTL 0x15000000, HALT bits set, CP_BUSY = 0), which argues a reset is not the missing ingredient. Still nearly free to test inside a frontier run.

**B2 — partial mask + proceed = a bankable partial win.** Patch the driver to accept UcodeLoadStatus 0x406 (SDMA pair + RLC — the mask that *completes* on this card) and continue the probe with the gfx/CP blocks skipped. DCE scanout does not need the CP; SDMA works for buffer moves. Ceiling: **the eGPU as a working display adapter under Linux** — a real deliverable even if CP never loads. The patch surface is bounded but real: gfx_v8_0 hw_init must be skipped selectively (SDMA stays), and powerplay's later CP power-gating toggles must be tolerated. This is the fallback to bank if P2 fails.

**B3 — deferred CP retry after gfx init: REJECTED on the evidence.** Nothing in gfx init changes the state of the SMC's task-4 engine; the stall has proven state-independent across every ASIC state presented so far (cold, MGCG-programmed, Apple firmware, warm-crossing). A retry hits the same wall.

**B4 — driver-side direct CP load: REJECTED, already falsified.** gfx_v8_0's own CP firmware path writes the same CP ADDR/DATA ports that cpprobe proved dead on this card (frontier v4.0).

## 7. Caveats and numbering notes

- **CAIL register numbering:** the reset pulse's second register is dword **0x398** in CAIL's register-ID space. The kernel's `SRBM_SOFT_RESET` is dword 0xE63 (byte 0x398C) in gfx_8_0 numbering, and 0x2008 *does* match GRBM_SOFT_RESET's dword index in the kernel numbering — so CAIL's space agrees with the kernel for GRBM but not, on the surface, for the second register. The `srbm_1_0_offset.h` header could not be fetched to reconcile this (path 404s), and the carves carry no symbol for it. Labelled unresolved; it does not affect any verdict above, since B1 uses the kernel's own masks and offsets.
- **Task-brief correction:** "GRBM_SOFT_RESET byte offset 0x8028" is GRBM_DEBUG_DATA; GRBM_SOFT_RESET is dword 0x2008 = byte **0x8020**. SRBM_SOFT_RESET byte 0x398C is correct as briefed.
- Function 0x712fc8's role (§2b) is the one place this report says "unknown" instead of guessing: the carve lacks LINKEDIT, so pointer-table reachability cannot be fully reconstructed.
- The LoadUcodes handler's enqueue code (which writes the CSR-class flag bytes for tasks 1/3) was characterized through the dispatch tree and the transfer routine's own reads rather than line-by-line; the routing claims in §4.2 rest on the bisect record + stall PCs + the routine's code, which are independent of the handler's internals.

## 8. Ranked next experiments (all frontier-shaped, all cheap)

1. **P0 + P2 combined run:** one-byte padding patch (gate) and, in the same image, 0x40 → 0x30 at the state-4 issue instruction. Readout = full bind + CP ring test in dmesg, *not* status bits alone.
2. **B1:** GRBM/SRBM soft-reset pulse inserted pre-dance in the loader; standard load; readout = UcodeLoadStatus.
3. **Track 2 replay:** Apple's exact TOC + mask 0x447E in our loader. Predicted null; one run, then close the branch either way.
4. **B2 (fallback win):** partial-mask module patch → display-only bind. Bank it if 1–3 fail.
