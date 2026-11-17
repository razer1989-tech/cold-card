# Blackmagic eGPU — Door 2: the SMC conversation (investigation sheet, prepared 2026-11-08 evening)

State this starts from (all measured 2026-11-08, full record in `blackmagic-egpu-linux-findings.md`):
BAR 0/2 assign for the first time ever with the EFI shim + `pci=realloc,hpmemsize=512M amdgpu.rebar=0`. amdgpu then dies in the SMU firmware load: the driver spends ~10.42 s (boot probe) or ~78 s in five retries (manual probe) sending messages to the card's SMU, and the SMC response register reads a flat 0 every time — "last message was failed ret is 0" ×5, then "SMU load firmware failed", probe -22. Exonerated by direct experiment: Display Core (`dc=0`, now BANNED — it also wedges the healthy internal card's console), DPM, PCIe ASPM, stone-cold enclosure power, warm Kubuntu reboots, FLR (refused, ENOTTY), and a genuine secondary bus reset via setpci. The MacBook reset-on-init quirk (LKML, Oct 2026) is therefore ruled out as the fix — a real reset changed nothing.

The author's call, 2026-11-08 18:38 local: "door 2" — the deep route, comparing how macOS's driver talks to this SMC against how Linux does. He noted he has nothing lined up tomorrow (Mon Nov 9).

## Experiment A — FIRST, before any reverse-engineering: the macOS warm handoff (never tested)

Every "warm boot" on record was Kubuntu→Kubuntu. No boot on record has ever let macOS's driver bring the SMU up first and then handed the card to Linux without a power cycle. If macOS loads/starts the SMC and that state survives a warm restart, Linux's probe inherits a living SMU instead of a silent one. ~20 minutes, zero writes, fully reversible.

1. Shut down Kubuntu. Leave the Blackmagic plugged in and powered the entire experiment — its power state IS the variable. Do NOT unplug it at any point.
   Verify: enclosure stays lit/connected through every step below.
2. Boot macOS the plain way: Option key → the macOS / internal entry directly. No EFI Boot shim card, no GRUB edits.
   Verify: macOS desktop on the main panel.
3. Confirm macOS's driver has the eGPU fully up, not just enumerated: System Information → Graphics/Displays shows the Radeon Pro 580, and the second monitor driven over the enclosure's HDMI is lit (this was verified working under macOS on 2026-11-05).
   Verify: second monitor showing a desktop.
4. Give the SMU real work for 2–3 minutes so macOS completes its full power/clock bring-up (e.g. drag a window to the second monitor, play a short video there).
   Verify: video plays on the eGPU monitor without stutter/tearing.
5. Restart — Apple menu → Restart (NOT Shut Down). As it comes back, hold Option → EFI Boot (the shim card) → GRUB `e` → append the proven trio, read it back in the editor before Ctrl+X (anchors: `hpmemsize=512M` with its equals sign, `amdgpu` with one p):
   ```
   pci=realloc,hpmemsize=512M amdgpu.rebar=0
   ```
   Verify: the line in the editor matches exactly. No `dc=`, no `dpm=` — both banned/exonerated.
6. In Kubuntu, first the standing read-back, then the verdict — with the SMU grep UNFILTERED by BDF (the BDF filter is what hid the SMU lines all night on Nov 8):
   ```
   cat /proc/cmdline
   lspci -s 44:00.0 -k
   sudo dmesg | grep -iE "smu|smc" | tail -20
   sudo dmesg | grep -E "0000:44:00" | grep -vE "input:" | tail -12
   ```
   Verify: `/proc/cmdline` shows the trio exactly; then read the SMU lines.

How to read the outcome:
- `Kernel driver in use: amdgpu` and no "SMU load firmware failed" → the handoff worked: macOS-started SMC state survives a warm restart, and the recipe gains a macOS-first step (or proves the fault is only in Linux's cold-start upload).
- Identical failure (SMU lines at +~10.4 s, probe -22) → decisive the other way: either the shim/GRUB chain or the restart resets the SMC, or macOS's state does not persist — the fault is confirmed inside Linux's upload conversation itself, and Tier 1 below is the next step with no more boot-state theories left.
- A different failure (different timing, different lines) → new data; bank the full unfiltered slice before touching anything else.
- If the warm restart hangs (the iMac's pre-existing warm-reboot quirk, which predates the eGPU): power-hold, boot stock Kubuntu the plain way — tonight's sessions proved repeatedly that nothing from a one-time GRUB edit follows you home.

## Experiment B — the cheap residual (separate run, one variable)

Different Thunderbolt cable, and/or the iMac's other TB port, then a normal shim + trio boot with the same checks as step 6. Rated a long shot (enumeration has been flawless for hours), ~5 minutes, and it closes the last physical-layer question so Tier 1's results can't be second-guessed later.

## Tier 1 — if A and B fail: instrument Linux's side of the conversation

Goal: name the FIRST PPSMC message that fails and what the response register actually does, using the working internal Ellesmere at 01:00.0 as the control card — it answers its SMU on the same driver, same kernel, same boot.

1. Boot the shim + trio + dynamic debug:
   ```
   pci=realloc,hpmemsize=512M amdgpu.rebar=0 dyndbg="module amdgpu +p"
   ```
   (Quotes around the value only — on Nov 8 the quotes wrapped the whole assignment and the cmdline still accepted it; confirm from the output that debug lines actually appear.)
   Verify: `cat /proc/cmdline`, then check the dmesg capture below is visibly denser than a stock probe.
2. Prefer a MANUAL probe for the capture — it takes the ~78 s five-retry path, which is a far wider window to observe than the 10.4 s boot probe:
   ```
   echo 0000:44:00.0 | sudo tee /sys/bus/pci/drivers/amdgpu/bind
   sudo dmesg | tail -300 > ~/egpu-door2-manual-probe.txt
   ```
   (If bind returns "Invalid argument" because an auto-probe raced it, capture that probe instead — same command, second line only.)
   Verify: `~/egpu-door2-manual-probe.txt` exists and contains "Found UVD firmware" and the SMU failure lines.
3. Control capture, same boot: pull the internal card's full successful SMU sequence from the same dmesg (`grep` around 0000:01:00.0's UVD line, plus the unprefixed `amdgpu:` SMU/SMC lines that precede its successful init) into the same file's notes — the side-by-side of "messages the twin answers" vs "first message the eGPU never answers" is the comparison Door 2 exists to produce.
   Verify: the file holds both sequences, labelled by card.

## Tier 2 — macOS-side artifacts (capture what macOS will actually give up)

Honest limit: macOS exposes no public MMIO trace of its AMD driver, so the macOS half of the comparison is artifacts, not a wire capture: (a) both cards' VBIOS dumps from Linux (`/sys/bus/pci/devices/0000:44:00.0/rom` and the twin's, while unbound) for a byte-level diff of the SMC firmware tables; (b) macOS System Information's firmware/driver versions for the Blackmagic while it is live under macOS (collect during Experiment A, step 3). Either could show a version/strap mismatch; neither is promised to.

## Decision point after A + B + Tier 1

If the response register still reads flat 0 on the first message in every state, the remaining routes are code-level (a patched amdgpu build tracing/altering the smu7 message path for this subsystem ID) — a project-scale step that is the author's call to open, with the Tier 1 capture as its evidence base. Bank every result, positive or negative, as a dated addendum to `blackmagic-egpu-linux-findings.md` — tonight's map is already, per the Nov 8 research, beyond anything published for this machine.
