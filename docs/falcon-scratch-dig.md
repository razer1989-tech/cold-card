# Falcon / SMU_DRAM dig — the scratch region and the two Falcon properties

**Date:** 2026-11-16 (night). **Scope:** desk-only. Two undug corners of Apple's stack:
the SMU_DRAM scratch region (PPSMC 0x252/0x253) and the ioreg properties
`PP_EnableLoadFalconSmcFirmware=1` / `PP_Falcon_QuickTransition_Enable=1`.
**Sources:** the four carved kexts in `~/workspace/user/files/` (HWLibs vm = carved
offset + 0x705000), `smcload16.py`, and torvalds/linux master fetched this session
(`smu7_smumgr.c`, `polaris10_smumgr.c`, `smu7_hwmgr.c`). Method: capstone
disassembly of the Polaris10 SMU cluster and its init/upload helpers; string-xref
scan over all 1,224 call sites of the PPLib property getter (0x78e32c).

**Headline: both corners are dead ends, now with receipts. Nothing here warrants
a frontier run.**

---

## (a) The SMU_DRAM scratch region — EXONERATED

**What the region is.** A host-owned VRAM buffer whose address the driver hands
to the SMC with SMU_DRAM_ADDR_HI/LO (0x252/0x253) so the SMC has DRAM workspace
of its own. It is *SMC-private scratch*, not a config channel.

**Linux writes nothing into it — ever.** `smu7_smumgr.c` (master):

- `smu7_init` (line 520): `smu_buffer.data_size = 200*4096` (0xC8000 = 800 KB),
  allocated as a VRAM BO via `amdgpu_bo_create_kernel`. Allocated only on bare
  metal (`hwmgr->not_vf`); the header/TOC buffer (one page) is allocated beside it.
- `smu7_request_smu_load_fw` (lines 297–308): the buffer's `mc_addr` is sent via
  0x252/0x253 — and that is the buffer's *only* other appearance in the file
  (plus its free in `smu7_smu_fini`). No `memcpy`, no `memset`, no field writes
  to `smu_buffer.kaddr` anywhere in `smu7_smumgr.c`; `polaris10_smumgr.c` and
  `smu7_hwmgr.c` contain **zero** references to `smu_buffer`. The SMC is handed
  an untouched 800 KB and initializes whatever it keeps there itself.

**Apple writes nothing into it either — and allocates the identical size.**
The Polaris10 backend init (HWLibs vm 0x838415+, immediately before the StartSMU
cluster) calls its VRAM allocator (0x796b13) three times:

1. size **0x1000** → host ptr at backend+0x0, mc_addr at backend+0x18/+0x1c —
   the header/TOC buffer (Linux: one page, same).
2. size **0xC8000** → mc_addr at backend+0x10/+0x14 — **the SMU buffer, byte-for-byte
   Linux's 200×4096**. After the call, the init stores only the address fields
   and moves on: no memset, no header, no table — nothing is written into it.
3. (only when the VF predicate 0x793299 returns ≠ 0) a third 0x1000 buffer at
   backend+0x28 — the VF-variant extra; not allocated on bare metal.

In the StartSMU cluster itself (vm 0x838a19, read in full), the region appears
exactly once: at 0x838d13–0x838e0f its address fields (backend+0x10/+0x14) are
loaded and sent as 0x252/0x253, in the same position in the sequence as Linux
(after the UcodeLoadStatus zero at 0x838ce8, before TOC populate and the
DRV_DRAM_ADDR sends at 0x83947f/0x8394fd). No store targets the region.

**The TOC-upload helper (0x798231)** — the one remaining suspect — is a 9-instruction
thunk: it calls a device vtable method (+0xb8) with a single argument, **0x160
(352)**, and returns whether it succeeded. 352 bytes is the TOC (344-byte struct,
padded) and nothing else; it uploads the TOC into the *header* buffer (the
DRV_DRAM region), and never touches the SMU region. (Linux builds the TOC in a
kzalloc'd host struct and `memcpy_toio`s it into its header buffer; Apple builds
it in the header buffer's host mapping and flushes 352 bytes through the OS
layer. Same bytes, same destination.)

**Our loader vs. both stacks:** smcload16 points 0x252/0x253 at a **zeroed** 1 MB
scratch. Linux hands over 800 KB of *uninitialized* VRAM; Apple hands over
800 KB it never writes. Zeroed is strictly the cleanest of the three. There is
no config block, feature mask, or table in this region in either stack, pre-load
or at any later point in the load path.

**One correction to phase 2, surfaced by this read:** the predicate gating
MEC_STORAGE (id 35) is the VF predicate — in the cluster (0x839573) the entry is
populated only when 0x793299 returns ≠ 0, i.e. **on bare metal Apple's TOC does
not contain MEC_STORAGE at all** (matching Linux: populated only when
`!hwmgr->not_vf`). The v4.9 replay included it unconditionally; harmless (no
mask bit, never walked) and the null result stands, but the record should be
accurate: Apple's bare-metal TOC is the nine entries + SMU_SK, mask 0x447E.

**Verdict (a): EXONERATED.** The scratch region is a red herring — an
SMC-private workspace both stacks hand over empty.

---

## (b) The Falcon properties — EXONERATED (one real but inert delta)

The two strings exist **only in HWLibs** (one occurrence each), and both are
consumed — in two different places, doing two different, both harmless, things.

### PP_Falcon_QuickTransition_Enable → a DPM-time platform cap. Nothing to do with loading.

- Consumed at HWLibs vm **0x77934b** and **0x77d4f5**, inside PPLib's
  registry-override init (two near-identical routines that read a long list of
  `PP_*` properties through the getter 0x78e32c into the platform-caps struct).
  The value lands in the caps byte immediately after `PP_ACDCGpioDisabled` —
  the two are a pair, and the pairing is the tell.
- The shared-codebase semantic is visible in Linux: `smu7_hwmgr.c` line 1313,
  the cap's **only** use in the file — inside `smu7_enable_sclk_mclk_dpm`
  (DPM start, long after StartSMU and the firmware load):

  ```c
  if (phm_cap_enabled(hwmgr->platform_descriptor.platformCaps,
              PHM_PlatformCaps_Falcon_QuickTransition))
      smum_send_msg_to_smc(hwmgr, PPSMC_MSG_EnableACDCGPIOInterrupt, NULL);
  ```

- So "QuickTransition" switches exactly one thing: whether, at DPM start, the
  driver tells the SMC to enable the **AC/DC GPIO interrupt** — the SMC's fast
  handling of AC↔DC power-source transitions. It is not an upload method, not a
  ROM-loader path, not an SMC image header flag (no runtime code in either
  stack modifies image header flags; the images are byte-identical artifacts),
  and not a message in the load sequence. On macOS the cap is sourced from the
  registry property instead of the VBIOS platform descriptor; same flag, same
  single consumer, same position — after the load is already over.

### PP_EnableLoadFalconSmcFirmware → gates a delegated upload path that is OFF on this machine

- Consumed at HWLibs vm **0x839a9b**, inside `Polaris10_UploadSMUFirmwareImage`
  (0x839967) itself: the getter reads the property into a stack dword that then
  steers the upload (logic at 0x839d0d–0x839d9c).
- What it steers: PPLib has **two** ways to get the SMC image in. The default
  (property absent/0 **and** a second predicate clear) first tries a *delegated*
  load — helper 0x797c66 builds a load descriptor (image ptr, size, and the MC
  firmware info fields) on the stack and calls out through the device context
  (`call [rbx+0x98]`) to the OS/driver layer to perform the load. If that call
  doesn't return success, it falls back to pushing the image itself, in a
  header-trimmed slice form (image+0x100, size−0x200).
- The second predicate is helper 0x793ccf: an ACPI **'ATPX'** property probe
  (PowerXpress/switchable-graphics presence). The gate is an OR: if *either*
  the ATPX probe *or* `PP_EnableLoadFalconSmcFirmware` is nonzero, the
  delegated path is skipped entirely.
- **On this machine the property is published as 1** (ioreg, both controller
  instances), so the OR is nonzero regardless of ATPX: macOS takes the direct
  path — PPLib raw-uploads the image itself, in the full descriptor form.
- And that raw upload (helper 0x839db1) is **instruction-equivalent to Linux's**
  `smu7_upload_smc_firmware_data`: set SMC_IND_INDEX_11 (reg space 0x1ac) to
  0x20000, read SMC_IND_ACCESS_CNTL (reg-id 0x92), OR in 0x800
  (AUTO_INCREMENT_IND_11, bit 11), write back, push the image dword-by-dword
  **bswap'd** through SMC_IND_DATA_11 (reg space 0x1ad), then clear the
  auto-increment bit. The surrounding start sequences match
  `polaris10_start_smu_in_protection_mode` / `_non_protection_mode` piece for
  piece (RESET_CNTL/CLOCK_CNTL/FIRMWARE_FLAGS manipulation, RCU waits, Test
  message, and in non-protection mode the 4-byte jump-on-start written via the
  copy helper 0x8366d5 from a `__data` constant — Linux's
  `smu7_program_jump_on_start`).
- Hardware cross-check already on record: frontier v4.5 danced Apple's own
  0x1f0e00 image through **our** Linux-style upload and got DONE+PASS — the
  upload Apple actually uses on this machine and the upload we perform are the
  same mechanism moving the same bytes.

**Verdict (b): EXONERATED.** QuickTransition is a post-load DPM feature (AC/DC
GPIO interrupt enable). EnableLoadFalconSmcFirmware selects between a delegated
OS-layer load and PPLib's own push — and as published on this machine (=1) it
selects the push, which is the Linux path our dance already replicates. The
delegated variant is a genuine Apple-only code path, but it is gated off here,
and its receiving layer owns the same SMC_IND register interface; it is not a
replayable delta. No frontier run warranted.

---

## (c) Any other pre-LoadUcodes host→card memory writes — EXONERATED (complete inventory)

Every host→card memory write in Apple's traced start path, from device start to
the LoadUcodes send (0x8395bf), each mapped to our replication:

| # | Write | Where in Apple's path | Our equivalent |
|---|-------|-----------------------|----------------|
| 1 | SMC firmware image → SMC SRAM 0x20000 | Upload helper 0x839db1 (SMC_IND auto-increment push) | The dance (smcdance5) — proven byte-clean, DONE+PASS |
| 2 | Jump-on-start dword {0xE0,0x00,0x80,0x40} → SMC SRAM 0 | Cluster 0x8390c7, **non-protection mode only** | Not applicable on this card: it reports protected=1 (SMU_FIRMWARE 0x0003001e), so neither stack writes it here; Linux gates it identically (`polaris10_start_smu_in_non_protection_mode`) |
| 3 | UcodeLoadStatus = 0 → SMC SRAM soft-regs | Cluster 0x838ce8 (soft_regs + 0x6c via the SMC window) | smcload16 zeroes it before the load ("driver parity" line in its output) |
| 4 | TOC (352 B) → VRAM header buffer | Helper 0x798231 flush, called at 0x8395af | smcload16 stages the TOC at TOC_OFF and points DRV_DRAM_ADDR at it |
| 5 | Firmware images → VRAM ucode area | The ucode layer outside PPLib (TOC entries carry their mc_addr from the firmware-info fetch, populate helper 0x839f4f) | smcload16 stages all images chunked + verified, same addresses-in-TOC scheme |
| 6 | **SMU_DRAM region: no write exists** | Address send only (0x838d13) | Address send only; ours is additionally zeroed |

SMC *register-space* writes in the start path (RESET_CNTL, CLOCK_CNTL_0,
FIRMWARE_FLAGS, SMU_STATUS clear, RCU waits) are the dance's ownership tail —
phase 1 established they are a 1:1 port of Linux's, and the project's dance
replicates them (the SMC comes up, roams, and answers Test in our runs). No
writes to the third (VF-only) buffer, no writes through any other vtable
method, and no other memory-writing helper is called anywhere in the cluster
between device start and LoadUcodes. The AVFS event manager call (0x837bb5)
and the soft-regs fetch (0x83669c) in the already-running branch are reads /
post-start bookkeeping, not pre-load writes.

**Verdict (c): EXONERATED.** The inventory is complete and every entry is
already replicated by the dance + smcload16.

---

## Bottom line

- (a) SMU_DRAM scratch: **exonerated** — 800 KB of SMC-private workspace, handed
  over unwritten by both stacks (identical size, identical position in the
  sequence); our zeroed 1 MB is a superset.
- (b) Falcon properties: **exonerated** — QuickTransition is a DPM-start
  platform cap (AC/DC GPIO interrupt message, post-load); EnableLoadFalcon
  gates a delegated upload path that the published value (=1) switches *off*
  on this machine, leaving the Linux-identical SMC_IND push our dance performs.
- (c) Other pre-load memory writes: **exonerated** — six-item inventory, all
  replicated; the only conditional one (jump-on-start) is gated off by this
  card's protected mode in both stacks.

No real delta found; no frontier run is warranted by this dig. Side product
worth keeping: the bare-metal correction to phase 2's TOC record (MEC_STORAGE
is VF-only in Apple's stack too).
