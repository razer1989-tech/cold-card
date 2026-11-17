# Apple init diff — what macOS's AMD stack does that amdgpu does not

Desk research, 2026-11-16 (late). Sources: carved kext slices in `~/workspace/user/files/`
(`AMD9500Controller.macho`, `AMDRadeonX4000.macho`, `AMDRadeonX4000HWLibs.macho`, `AMDSupport.macho`;
Mach-O file offsets below are slice offsets; vmaddr = slice + the kext's KC __TEXT fileoff —
0x226000 controller, 0x2ca000 X4000, 0x705000 HWLibs, 0x13be6000 AMDSupport), disassembled with
capstone 5.0.7. Kernel baseline: torvalds/linux master sources fetched to /tmp/ksrc
(`gfx_v8_0.c`, `vi.c`, `smu7_smumgr.c`, `polaris10_smumgr.c`, `gfx_8_0_d.h`, `smu_7_1_3_d.h`,
`smu_7_1_3_sh_mask.h`).

## Headline result

One concrete, replayable diff survived scrutiny, and it is exactly in the suspected domain —
**GFX clock gating**:

- amdgpu **never programs the CGTT clock-gating register table on Polaris10/11/12**.
  In `vi.c`, `vi_enable_mgcg()` switches on ASIC type and programs the per-ASIC
  `*_mgcg_cgcg_init` (reg, mask, value) table for Topaz/Fiji/Tonga/Carrizo/Stoney — then:
  `case CHIP_POLARIS10: case CHIP_POLARIS11: case CHIP_POLARIS12: case CHIP_VEGAM: default: break;`
  Polaris falls through and gets **nothing**. (vi.c lines ~496–525.)
- Apple's HWLibs (AMD's PowerPlay library, the same codebase family) **carries a 75-entry
  mgcg table for the Polaris10-class backend** — including `CGTT_CP_CLK_CTRL`,
  `CGTT_CPC_CLK_CTRL`, `CGTT_CPF_CLK_CTRL` — the clock controls for the very CP block whose
  SMC firmware transfer never fires on the cold, unPOSTed card. In AMD's PP architecture this
  table is applied during hwmgr init, i.e. before StartSMU / LoadUcodes in the bring-up order.
- Everything else in Apple's SMC load path is a **faithful port of the Linux code** (section 1),
  and no amdgpu-style golden-settings table exists anywhere in the Apple binaries (section 2).

Caveat on identification: HWLibs contains five mgcg tables (fingerprinted in section 3). Three
are 75-entry VI tables; two of those are byte-identical, the third differs in exactly two
entries. One of the three is Tonga's; the other two belong to the Polaris10 and Polaris22
backends (the library's five SMC backends are Iceland/Tonga/Fiji/Polaris10/Polaris22, matching
the five tables). Which of the three is Polaris10's could not be proven from the data alone —
but the CP-critical entries are **identical in all three**, and the only divergence is
`CGTT_CPF_CLK_CTRL` (0x40000100 vs 0x100) and `RLC_CGCG_CGLS_CTRL` (0x0020003c vs 0x0020003f).

## Candidate replay list (BAR5 byte offset, value — all masks 0xffffffff)

Ranked by plausibility of gating the CP transfer. All are plain MMIO writes, replayable from
Linux userspace via BAR5 after barfix2, before the dance/load. **Read each register on the
cold eGPU first** (see "Suggested test" below) — if they already hold these values, the
theory dies cheaply.

| # | Register | BAR5 byte | Value | Evidence |
|---|----------|-----------|-------|----------|
| 1 | CGTT_CP_CLK_CTRL | 0x3C2C0 | 0x00000100 | Apple mgcg table entry 4 (HWLibs slice 0x366a60); kernel tonga/fiji/iceland/cz mgcg tables carry the same value; amdgpu writes it for those ASICs, never for Polaris |
| 2 | CGTT_CPC_CLK_CTRL | 0x3C2C8 | 0x00000100 | Apple entry 5 (slice 0x366a6c); same kernel cross-check |
| 3 | CGTT_CPF_CLK_CTRL | 0x3C2C4 | 0x40000100 (variant: 0x00000100) | Apple entry 6 (slice 0x366a78); 0x40000100 in tables A/B and in kernel tonga/fiji; 0x100 in table C and kernel cz |
| 4 | RLC_CGTT_MGCG_OVERRIDE | 0x3B120 | 0xFFFFFFFF | Apple entry 0 (slice 0x366a30) — the master MGCG override that heads every mgcg table, Apple's and kernel's alike |
| 5 | RLC_CGCG_CGLS_CTRL | 0x3B124 | 0x0020003C (variant: 0x0020003F) | Apple entry 74 (slice 0x366df4); note amdgpu's Polaris10 *golden* table writes this register too but with 0x0001003C — the mgcg value has bit 21 (0x200000) set, the golden value does not |
| 6 | CGTT_RLC_CLK_CTRL | 0x3C2E0 | 0x00000100 | Apple entry 13 (slice 0x366ac0) — RLC clock, included because RLC loads fine but shares the GFX clock tree |
| 7 | CP_RB_WPTR_POLL_CNTL | 0x8708 | 0x00900100 | Apple entry 73 (slice 0x366de8) — CP ring poll control, part of the same table |

The full 75-entry table (table A, HWLibs `__DATA,__data` slice 0x366a30..0x366dfc) is in the
appendix. A minimal first test needs only entries 1–5 above; the full table is the fallback
if the subset moves nothing.

## Section 1 — The HWLibs SMC load path is a 1:1 port of Linux. Closed.

The five PPSMC message clusters in HWLibs `__text` were identified by their assert strings:
Iceland (vm 0x7e34ac), Tonga (0x7fb2e4), Fiji (0x817a02), **Polaris10 (0x838d1b)**,
Polaris22 (0x85014b). The Polaris10 function `SmuPolaris10_RequestSMULoadFW` /
`SmuPolaris10_StartSMU` (one merged function, vm 0x838a19..0x8396a4, slice 0x133a19) was
disassembled in full and aligned against `polaris10_smumgr.c` + `smu7_smumgr.c`:

- Same message sequence: SMU_DRAM_ADDR_HI/LO (0x252/0x253, carrying the firmware buffer),
  TOC populate (firmware IDs 10, 3, 4, 5, 6, 7, 8, 1, 2, 14, and 35/MEC-storage — one more
  than Linux's list: ID 14), DRV_DRAM_ADDR_HI/LO (0x250/0x251, the TOC), LoadUcodes (0x254)
  with mask 0x47E, or 0x447E when the ID-14 entry is present. (Linux sends the same 0x47E;
  the project's mask correction to 0x5FE stands — Apple's library predates the JT1/JT2 split
  accounting and is not evidence either way.)
- Same start sequence, both modes: SMC_SYSCON_RESET_CNTL (SMC addr 0x80000000) rst bit,
  SMC_SYSCON_CLOCK_CNTL_0 (0x80000004) ck_disable bit, SMU_STATUS (ixSMU_STATUS 0xE0003088)
  clear + DONE/PASS check, FIRMWARE_FLAGS (ixFIRMWARE_FLAGS 0x3F000), protected-mode read
  from ixSMU_FIRMWARE (0xE00030A4) bit 16. Helpers identified: `Polaris10_UploadSMUFirmwareImage`
  (vm 0x839967 — it first fetches the MC firmware descriptor, `Polaris10_GetMCFirmware`),
  `Polaris10_SendMsgToSmc_Offset` (vm 0x836f0e — Test message with 0x20000 offset),
  `smu7_is_smc_ram_running` equivalent (vm 0x836a12 — reads clock cntl + SMC PC at 0x80000370,
  compares ≥ 0x20100), `Polaris10_AVFSEventMgr` (vm 0x837bb5, runs after start, as in Linux).
- Register access goes through a register-ID layer (helpers at vm 0x7ff3b1 read / 0x7ff3d2
  write / 0x7ff403 wait-equal / 0x7ff43a wait-unequal — the last two are thin wrappers that
  tail-jump to the SMUM_WaitForRegister* functions; verified by disassembling the wrappers).
  **Correction of an initial misread:** the accesses to RCU_UC_EVENTS (SMC addr 0xC0000004) in
  this path are *waits*, not writes — wait INTERRUPTS_ENABLED (bit 16, mask 0x10000) in
  protection mode and wait boot_seq_done ≠ 0 (bit 7) in non-protection mode — exactly Linux's
  `PHM_WAIT_VFPF_INDIRECT_FIELD` calls (field names from `smu_7_1_3_sh_mask.h` lines 715–728).
  Apple's stack does **not** write RCU_UC_EVENTS. No ASIC (MMIO-space) register is written
  anywhere in the load path; a full census of constant-address helper calls in HWLibs shows
  SMC-space addresses only.
- The two 0x80000398 references in HWLibs (slice 0x2d52c4 and 0x2fa2c4) are **inside embedded
  LM32 SMC firmware images** in `__DATA,__data` (byte-identical context, 0x23000 stride =
  the per-ASIC SMC image array) — not host code. Apple's host stack never touches the SMC
  command register directly.
- The mailbox BAR5 offsets (0x250/0x254/0x290) and CP direct-load ports (0x3E050…) never
  appear as constants anywhere in the four binaries: all SMC messaging goes through the
  register-ID layer (IDs 0x94/0x95/0xa4 visible in `Polaris10_SendMsgToSmc_Offset`), and the
  per-ASIC ID→address directory tables are the long register-offset arrays in HWLibs `__data`
  (e.g. the CGTT address run at slice 0xd81960). Apple's stack uses the SMC LoadUcodes path,
  same as Linux — confirmed, no direct-load back door.

## Section 2 — No Apple golden-settings table; the controller's GRBM_STATUS is an idle poll

- Byte-pattern search for amdgpu's Polaris10 golden triples (CB_HW_CONTROL, SQ_CONFIG,
  PA_SC_RASTER_CONFIG, TCC_CTRL, RLC_CGCG_CGLS_CTRL 0x1003C form, SPI_RESOURCE_RESERVE_EN…)
  across all four binaries: **no hits** (only the bare value 0x22011002 inside SMC firmware
  images in HWLibs `__data`). A generic (offset, mask, value) triple scan over `__const`/`__data`
  found only register-*address directory* tables (the PP register-ID maps), never init values.
  Apple's GFX pipeline init is register-ID-driven code in AMDRadeonX4000; it was not diffed
  instruction-by-instruction — that line is bounded and closed, not merely unfinished: with no
  constant tables and no offset immediates for any CGTT/CP register anywhere in the four
  binaries (verified by immediate scan), there is no second hidden table to find.
- AMD9500Controller's single GRBM_STATUS (0x8010) reference (slice 0x31a3c; function
  vm 0x257a20..0x257b05) is an **is-idle check**: it reads GRBM_STATUS GUI_ACTIVE (bit 31)
  plus SRBM-side status registers (0xD034, 0xD834, 0xE50, 0x20004) through a vtable read
  accessor and returns whether all are idle. Not an init sequence. The controller contains no
  ASIC init table (its `__TEXT,__const` run at slice 0x5a6a0 is an SMC register-offset list).

## Section 3 — The mgcg tables in HWLibs (the diff)

Format: 12-byte entries (reg_dword_offset, mask, value) in `AMDRadeonX4000HWLibs.macho`
`__DATA,__data`, each headed by RLC_CGTT_MGCG_OVERRIDE = 0xFFFFFFFF. Census by signature
(dwords 0xEC48, 0xFFFFFFFF, 0xFFFFFFFF):

| Slice | Entries | CGTT_CP | CGTT_CPF | Fingerprint |
|-------|---------|---------|----------|-------------|
| 0x316a50 | 64 | 0xC0000100 | 0xC0000100 | **Iceland** — 64/64 prefix-identical to kernel `iceland_mgcg_cgcg_init` |
| 0x414720 | 35 | 0x00000100 | 0x40000100 | **Fiji** — kernel `fiji_mgcg_cgcg_init` is the only 35-entry kernel table |
| 0x366a30 | 75 | 0x00000100 | 0x40000100 | table A — Tonga or Polaris10/22 (see below) |
| 0x367580 | 75 | 0x00000100 | 0x40000100 | table B — **byte-identical to A** (0 differing entries) |
| 0x3c0740 | 75 | 0x00000100 | 0x00000100 | table C — differs from A/B in exactly 2 entries (CPF; RLC_CGCG_CGLS_CTRL 0x20003F) |

All three 75-entry tables contain one entry with no kernel counterpart in gfx_v8_0.c:
reg dword 0x1579 (byte 0x55E4), value 0x00600100 — by value and position this is
CGTT_DRM_CLK_CTRL0 (the kernel's older vi.c tonga table writes 0x00600100 to it; the register
lives in the display header set, not gfx_8_0_d.h). The tables are consumed through the PP
library's per-ASIC data structs (a copy routine at HWLibs vm 0x747389 expands the triples
into the runtime (mask, value) arrays indexed by register ID); the exact mac call site that
applies the Polaris10 table during init was not isolated — stated plainly, that is the one
gap in the provenance chain. The architectural fact stands: AMD's PP code applies these
tables at hwmgr init for every ASIC that has one, and this library ships one for the
Polaris10 backend, while amdgpu's switch explicitly skips Polaris.

Why this fits the failure: the SMC's 0x40 transfer command targets the CP ucode RAMs, which
sit behind the CGTT CP/CPC/CPF clock controls. On a POSTed card (macOS, or the internal GPU
under Linux) those registers hold firmware/VBIOS-established values; on the cold eGPU under
Linux nothing ever writes them — amdgpu skips the table for Polaris by design, and EFI never
POSTs the card. SDMA/RLC transfers complete because their clock domains come up differently;
the CP domain is the one left at reset state.

## Suggested test (for the parent; not run — desk task)

1. Cold virgin cycle, barfix2, then **read before writing**: CGTT_CP_CLK_CTRL (0x3C2C0),
   CGTT_CPC_CLK_CTRL (0x3C2C8), CGTT_CPF_CLK_CTRL (0x3C2C4), RLC_CGTT_MGCG_OVERRIDE (0x3B120),
   RLC_CGCG_CGLS_CTRL (0x3B124) on the eGPU, and the same five on the internal GPU
   (0000:01:00.0) as the known-good reference. If the internal GPU shows the table values
   and the eGPU shows 0x00000000 (or anything gated), the theory is corroborated before a
   single write.
2. If corroborated: new frontier variant — after barfix2, write the ranked subset
   (entries 1–5, CPF = 0x40000100 first), then the usual dance + full-mask load.
3. If the subset stalls identically, replay the full 75-entry table; if that stalls too,
   this branch closes and the POST-state theory narrows to MC/VBIOS territory.

## Appendix — full 75-entry table A (HWLibs slice 0x366a30; BAR5 byte = dword × 4; all masks 0xFFFFFFFF)

RLC_CGTT_MGCG_OVERRIDE 0x3B120=0xFFFFFFFF · GRBM_GFX_INDEX 0x30800=0xE0000000 ·
CB_CGTT_SCLK_CTRL 0x3C2A0=0x100 · CGTT_BCI_CLK_CTRL 0x3C208=0x100 ·
**CGTT_CP_CLK_CTRL 0x3C2C0=0x100 · CGTT_CPC_CLK_CTRL 0x3C2C8=0x100 ·
CGTT_CPF_CLK_CTRL 0x3C2C4=0x40000100** · (dword 0x1579) 0x55E4=0x00600100 ·
CGTT_GDS_CLK_CTRL 0x3C280=0x100 · CGTT_IA_CLK_CTRL 0x3C214=0x06000100 ·
CGTT_PA_CLK_CTRL 0x3C220=0x100 · CGTT_WD_CLK_CTRL 0x3C218=0x06000100 ·
CGTT_PC_CLK_CTRL 0x3C204=0x100 · CGTT_RLC_CLK_CTRL 0x3C2E0=0x100 ·
CGTT_SC_CLK_CTRL 0x3C224=0x100 · CGTT_SPI_CLK_CTRL 0x3C200=0x100 ·
CGTT_SQ_CLK_CTRL 0x3C230=0x100 · CGTT_SQG_CLK_CTRL 0x3C234=0x100 ·
CGTT_SX_CLK_CTRL0–4 0x3C250/54/58/5C/60=0x100 · CGTT_TCI_CLK_CTRL 0x3C27C=0x100 ·
CGTT_TCP_CLK_CTRL 0x3C278=0x100 · CGTT_VGT_CLK_CTRL 0x3C210=0x06000100 ·
DB_CGTT_CLK_CTRL_0 0x3C290=0x100 · TA_CGTT_CTRL 0x3C274=0x100 ·
TCA_CGTT_SCLK_CTRL 0x3C2B4=0x100 · TCC_CGTT_SCLK_CTRL 0x3C2B0=0x100 ·
TD_CGTT_CTRL 0x3C270=0x100 · GRBM_GFX_INDEX 0x30800=0xE0000000 ·
CGTS_CU0–CU7 *_CTRL_REG 0x3C020..0x3C0BC (40 entries: SP0=0x00010000, LDS_SQ=0x00030002,
TA/TA_SQC=0x00040007, SP1=0x00060005, TD_TCP=0x00090008 per CU) ·
CGTS_SM_CTRL_REG 0x3C000=0x96E00200 · CP_RB_WPTR_POLL_CNTL 0x8708=0x00900100 ·
RLC_CGCG_CGLS_CTRL 0x3B124=0x0020003C.
Table C (@0x3c0740) is identical except CGTT_CPF_CLK_CTRL=0x00000100 and
RLC_CGCG_CGLS_CTRL=0x0020003F.
