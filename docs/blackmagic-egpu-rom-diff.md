# Blackmagic eGPU vs internal twin — VBIOS ROM teardown and ASIC_Init diff

Date: 2026-11-09. Analyst: cloud Muse (Wednesday), from dumps the author pulled off the iMac.
Images: `roms/egpu/egpu-rom.bin` (Blackmagic eGPU, BDBD:A14C) and `roms/twin-rom.bin` (internal Ellesmere, 106B:019E). Both 60,928 bytes, PCI checksum valid (sum = 0), PCIR device 0x67DF in both, each flagged "last image" (no chained UEFI image in either). Table identification and opcode semantics taken from the mainline kernel's `atombios.h` / `atom.c` (v6.12 reference copies in `roms/ref/`); disassembler in `roms/dis.py`, parsers in `roms/romparse.py` / `roms/romdiff.py`.

Note on method: the two images place their tables at different absolute offsets (different build layout), so a raw offset-by-offset compare shows ~95% difference and means nothing. All comparisons below are per-table, by master-list index. (Early in the analysis the master command/data pointers were read swapped — ATOM_ROM_HEADER carries usMasterCommandTableOffset at +0x1E and usMasterDataTableOffset at +0x20 — the assignments were corrected against the header struct and every result below uses the corrected mapping.)

## The init tree, table by table

| Table | eGPU | Twin | Verdict |
|---|---|---|---|
| cmd[0] ASIC_Init | 131 B | 133 B | Same program. Twin adds one call pair (`MOVE` + `CALL Gfx_Harvesting`); both end with the identical register OR at byte 0x1740. Voltage parameter differs (see FirmwareInfo). |
| cmd[2] ASIC_RegistersInit | 183 B | 192 B | **Identical instruction stream**, including every MC indirect-port (SETPORT 3) MASK write. Twin appends a `GPIOPinControl` call (GPIO plumbing). |
| cmd[5] MemoryControllerInit | 275 B | 275 B | **Byte-identical.** Its behaviour is driven entirely by data tables (below). |
| cmd[29] SMC_Init | stub | stub | Points at a zero-size table in **both** images. Neither Apple POST initialises the SMC via VBIOS. |
| cmd[64] MemoryTraining | 256 B | 282 B | Same head and tail; middle section differs (training parameters for different DRAM). Called by MemoryControllerInit in both. Twin's version selects data block 29; eGPU's never does (see data[29]). |
| data[4] FirmwareInfo | 108 B | 108 B | **One byte differs** (+0x4E: 0x52 vs 0x6B) — the VDDC voltage value (850 vs 875 in table units) that ASIC_Init passes to SetVoltage. All clock fields identical. |
| data[8] SMU_Info | — | — | **Identical.** The SMC's own data table is the same on both cards. |
| data[24] MC_InitParameter | 724 B | 724 B | **The 117-entry (register, value) pair list is 100% identical** — same registers, same values. Only difference: two header dwords at +0x08/+0x0C (eGPU 0x00B4D360 / 0x00038000, twin zero). The pair list is what MemoryControllerInit actually programs; the header fields are outside it and undocumented in the kernel headers. |
| data[28] VRAM_Info | 2,688 B | 1,910 B | Different, correctly, per card. eGPU modules: Samsung K4G80325FB, Hynix H5GC8H24MJR, Micron MT51J256M32HF (all 8 Gbit). Twin: Samsung K4G41325FE, Hynix H5GC4H24AJR (4 Gbit). Each table describes its own board's DRAM. |
| data[29] MemoryTrainingInfo | **absent** | present | The one structural omission. Present on the twin and selected by its MemoryTraining table; absent from the eGPU image, whose MemoryTraining never references it. Concerns DRAM pattern training; the eGPU's DRAM demonstrably works for CPU staging and GPU traffic after POST. |
| cmd[32] Gfx_Harvesting | absent | 209 B | Twin-only; called from the twin's ASIC_Init. CU harvesting config — not on the SMC path. |
| cmd[37] EnableVGA_Render | 44 B | absent | eGPU-only legacy VGA stub. Irrelevant. |
| data[15] PowerPlayInfo, data[22] Object_Header, data[12] GPIO_Pin_LUT, data[32] VoltageObjectInfo, cmd[12] SetPixelClock, cmd[67] SetVoltage, cmd[71] ClockSource, cmd[77] DIG2TransmitterControl, etc. | differ | differ | Board/display/power plumbing differences, expected between an internal panel card and a Thunderbolt enclosure card. None touch the SMC or the memory controller's SMC client. |

## Conclusion

**There is no missing register write in Apple's POST.** The two VBIOS init trees are functionally the same program — identical MC register programming, identical SMU data, identical SMC treatment (none) — fed with each board's correct DRAM data. The "Apple's ASIC_Init omits the write that arms the SMC's DRAM path" theory is dead in its register form, with the full write lists as evidence. The MemoryTrainingInfo omission is real but is about DRAM pattern training, and the eGPU's DRAM works for every other client after POST.

Reframing forced by this result plus earlier evidence: the twin's standard SMC firmware was already running — with all ucodes loaded (UcodeLoadStatus 0x5FE) — in a no-driver boot. Nothing in Linux started it; **Apple's own UEFI stack starts the standard SMC firmware on the internal card at machine POST.** On the external card, under a Linux boot, the SMC sits in boot-ROM download-wait (virgin): no Apple firmware ever starts the standard firmware on it, in any OS (macOS starts Apple's own channel firmware instead). The working-vs-dead difference therefore lives in the **start procedure**, not in VBIOS data — in whatever Apple's EFI SMU driver does around the firmware start that neither the Linux driver's start nor the userspace dance replicates.

## Remaining roads

1. **Retail Ellesmere VBIOS teardown (offline, no machine time).** Both Apple images stub SMC_Init; a retail RX 470/480/570/580 image (publicly archived, e.g. TechPowerUp's VGA BIOS database) shows what a normal card's init tree does around the SMC. If retail's SMC_Init is a real table sending PPSMC messages, that sequence is a candidate for the missing start step — replayable from userspace via the mailbox.
2. **The frontier test (machine time, still untested):** dance-start on a trained ASIC with FB_LOCATION + CHMAP poked — the one start-procedure variant not yet tried. Nothing in the ROM result kills it; the ROM result says the answer is in start-procedure space, which is exactly where this test lives.
3. Loose end, low weight: the two MC_InitParameter header dwords (eGPU 0x00B4D360 / 0x00038000 vs twin 0 / 0), undocumented; noted for completeness.
