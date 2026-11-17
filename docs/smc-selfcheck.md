# SMC boot self-check — where it lives, what it compares, and why a patched image cannot be fixed up

Desk research, 2026-11-16 (late). Trigger: frontier v4.7 danced a Linux SMC image
with one byte changed (CP transfer command 0x40→0x30 at image file offset 0xE054).
Upload was byte-clean, but SMU_STATUS read back **0x1 (DONE=yes, PASS=0)** and the
image never ran (frontier-11.log: post-dance PC = 0x2950). Genuine images (Linux
0x171a00, Apple 0x1f0e00) yield SMU_STATUS 0x3 (DONE+PASS).

## 1. The check is NOT in the image — it is the SMU mask-ROM bootloader

Three independent lines of evidence:

- **Boot-path disassembly (Linux image, lm32dis.py).** Entry at SRAM 0x20100
  (the header's EntryPoint field): CSR setup, `bi 0x20428` → stack set
  (r28 = 0x3effc), BSS clear loop over 0x3c7a8–0x3c7c4, `calli 0x20a14` (main),
  then the idle loop at 0x20504. There is **no checksum/hash loop anywhere in
  the boot path** — nothing walks the 130 KB image accumulating anything before
  main. (Logically it could not be otherwise: a self-check inside the image
  would have to run before the image is trusted to execute.)
- **No crypto machinery in the image.** Byte search of both genuine images
  (Linux + Apple), both byte orders, for SHA-1 initial/round constants
  (0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0, 0x5A827999,
  0x6ED9EBA1, 0x8F1BBCDC, 0xCA62C1D6), SHA-256 constants (0x6A09E667,
  0x428A2F98), MD5 constant 0xD76AA478, CRC32 polynomial 0xEDB88320 and CRC32C
  0x82F63B78: **zero hits in either image.** The image contains no hash
  implementation and no CRC table.
- **Hardware behaviour (frontier-11.log).** Before any dance, the SMC's PC idles
  at 0x2a34–0x2b58 — the mask-ROM's own address space, below the image base
  0x20000. After a genuine dance the PC roams image space (0x205xx/0x23fxx).
  After the v4.7 patched dance the PC stayed at **0x2950 — still inside ROM
  space**: the image never executed one instruction. The PASS verdict
  (SMU_STATUS bit 1) was rendered by the ROM bootloader before jumping to the
  image, exactly as the DONE bit (bit 0) is.

## 2. The stored expected value: Digest[5] at image offset 0

The image header is the kernel-documented `struct SMU7_Firmware_Header`
(drivers/gpu/drm/amd/amdgpu/smu7.h, SMU7_FIRMWARE_HEADER_LOCATION 0x20000):

| Image offset | SRAM | Field | Linux 0x171a00 | Apple 0x1f0e00 |
|---|---|---|---|---|
| 0x00–0x13 | 0x20000 | **Digest[5]** | c46fe4a6 11d2826d 69955c5e ba8092e7 790c579b | 9ce2cd7b a7f7532b 049195e0 2e5b395a eb5e5b82 |
| 0x14 | 0x20014 | Version | 0x00171a00 | 0x001f0e00 |
| 0x18 | 0x20018 | HeaderSize | 0xb4 | 0xb4 |
| 0x1c | 0x2001c | Flags | 0 | 0 |
| 0x20 | 0x20020 | EntryPoint | 0x20100 | 0x20100 |
| 0x24 | 0x20024 | CodeSize | 0x1d80c | 0x1ccd4 |
| 0x28 | 0x20028 | ImageSize | 0x1fba8 | 0x1fb94 |
| 0x30 | 0x20030 | SoftRegisters | 0x3fa28 | 0x3fa14 |
| 0xb0 | 0x200b0 | "Signature" | 0xaa55aa55 | 0xaa55aa55 |

Notes: the header's trailing "Signature" dword (+0xb0, the last dword of the
0xb4 header) is the **constant 0xaa55aa55 in all 13 genuine images examined** —
a format magic, not a per-image cryptographic signature (a 0xb4-byte header has
no room for one in any case). The dance-file *outer* header field [0x1c] is the
amdgpu container CRC and is not the gate (proven on hardware: the Apple clone
carried 0 there and PASSED).

## 3. The digest is not recomputable — exhaustive negative across 13 genuine images

Corpus: the Linux polaris10 image, the Apple polaris10 image, and **11 further
genuine SMC images extracted from AMDRadeonX4000HWLibs `__data`** (stored
dword-byte-swapped at slice offsets 0xb79b40…0xcb8520, stride ~0x1fb00–0x23000;
versions 0x171a03, 0x1f0e00, 0x1d1f05, 0x201100, 0x221a00, 0x230c00 and others —
the Apple one's extracted digest matches the known Apple image byte-for-byte,
validating the extraction). Each carries its own Digest[5].

For every image, tested: **SHA-1, SHA-256, SHA-224, SHA-512, MD5, RIPEMD-160**
over 15+ ranges (whole image; digest field zeroed; digest+signature zeroed;
header excluded [0xb4, end); code only [0xb4, 0xb4+CodeSize); from EntryPoint
[0x100, …) and [0x100, 0x100+CodeSize); [20, end); [0, CodeSize); up to the
Rtos and SoftRegisters boundaries; whole and code ranges byte-reversed) ×
input transforms (raw bytes, dword-byte-swapped) × digest comparison forms
(raw, dword-swapped, fully reversed). **Zero matches, on any image, under any
combination.** A plain additive checksum or CRC32 is independently excluded:
the field is 160 bits wide and all 20 bytes are high-entropy and image-specific
(a 32-bit check would leave 16 bytes unexplained).

Because no candidate reproduces the stored digest even on *unpatched* images,
the step-2 fix-up procedure (recompute → rewrite → re-dance) cannot be
produced, and the mandatory self-verification fails for every candidate — that
exhaustive failure across 13 images is itself the evidence for the verdict:

## 4. Verdict: a keyed/authenticated digest verified by ROM — the patch road is closed

Digest[5] is an **authenticated digest computed by AMD at build time and
verified by the SMU's mask-ROM bootloader against a hardware-resident key**.
This is consistent with the register-level evidence already in the project
findings (2026-11-08 probe round 2: SMU_FIRMWARE = 0x0003001e — protected=1,
hard key=1 — identical on both cards). It is not an RSA-style signature (none
is present), but it is equally unforgeable from this bench: without the fused
key, no corrected digest can be computed for a modified image, so **no fix-up
exists and a patched SMC image cannot be made to pass**. Patch track P2 (the
0x40→0x30 re-route) is therefore closed at the SMC's front door, regardless of
whether the re-route would have worked. Any future patch road would first
require extracting the SMU's fused key from silicon — out of scope for this
bench. Remaining roads are the ones that never modify SMC code (start-sequence
report: B1 GRBM soft-reset pulse before the load, B2 partial-mask display-only
bind, Apple-TOC replay).
