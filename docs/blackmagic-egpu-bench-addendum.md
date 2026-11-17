# Addendum — Blackmagic eGPU bench (for the Nov 5, 2026 build day)

**Why this exists:** On Nov 4 the author and Wednesday settled a sequence for build day — bench Pete's Blackmagic eGPU on the macOS side while the Wednesday backup copies — but that step never made it into the 7-phase Kubuntu checklist (`kubuntu-dualboot-walkthrough-checklist.md`). This addendum is that missing step. The trade is settled (Charizard ex, the author's call, Nov 4) and the box is already at the house, photographed beside the iMac on the evening of Nov 4 — a day early. It does NOT need to wait for tomorrow.

**The box:** Blackmagic eGPU, Radeon Pro 580, 8 GB GDDR5, ~5.5 TFLOPS. It had been sitting unused in Pete's closet a long while, so first-power behaviour is genuinely unknown until it is benched.

## Gating facts — read before scheduling the day

- **The Amazon Thunderbolt cable gates ONLY this bench, not the install.** The Kubuntu install, the backup, and the restore do not need the eGPU or its cable. If the cable has not landed when Hunter goes home, build day proceeds exactly as checklisted and the bench slips to whenever the cable arrives. Do not let a cable delay the install.
- **The backup no longer runs on the 8 TB drive.** the author's Nov 4 call: Wednesday backs up to the dedicated 1 TB WD portable (believed exFAT, USB-C cable on hand), kept separate from his other files. The bench-while-it-copies slot is Phase 1 of the checklist, on that drive.
- **The HP Omen is not a fallback host.** Settled Nov 4: the RX 560 SKU of the 2017 Omen 15 shipped without a Thunderbolt controller (TB3 was GTX 1060 SKUs only), so the box has nothing to talk to there. The iMac is the only host.

## Bench protocol — macOS side, while Phase 1 copies

1. iMac booted into macOS Sequoia (native on this 2019 27″ — no OpenCore anywhere in this).
2. Connect the Blackmagic box: power first, then Thunderbolt to the iMac. Use the Amazon cable when it lands; length/brand recorded below.
3. **Recognition check:** Apple menu → About This Mac / System Information → Graphics/Displays and → Thunderbolt. Record: does "Blackmagic eGPU" / Radeon Pro 580 appear, and is it listed as connected at full Thunderbolt speed?
4. **Prefer-eGPU check:** note whether macOS offers the eGPU for apps (Get Info → "Prefer External GPU" on a test app), and whether an external display path exists if he wants one. A headless compute bench is fine — recognition plus a load is the pass bar.
5. **Load check (only if LM Studio is present on the macOS side; if it isn't, skip rather than installing anything mid-build-day):** load one small model with GPU offload and confirm it runs and the eGPU shows activity. If LM Studio is not on macOS, the load test defers to the Kubuntu smoke test in Phase 6 — recognition on macOS still proves the hardware wakes up clean, which was the point of benching it here first.
6. **Record the result here before Phase 3 starts:**
   - Date/time:
   - Cable used:
   - Recognised by macOS? (Y/N, name as shown)
   - Load test run? Result:
   - Any fan/noise/heat oddities after sitting unused:

## Kubuntu expectations — set them before Phase 6

- The Blackmagic box was designed as a Mac accessory. Under Kubuntu on this iMac, Thunderbolt eGPU support rides on the Linux amdgpu driver plus Thunderbolt device authorisation (bolt) — it may work, it may need manual authorisation, and it is NOT part of the 1.5 pass bar.
- **Order of operations stands:** Phase 6 restores Wednesday on the internal Radeon Pro 570X first and the full smoke test passes on internal GPU alone. Only after 1.5 is verified working does the eGPU get attached under Kubuntu as a separate experiment, so a Kubuntu-eGPU failure can never be mistaken for a 1.5 failure.
- If it does enumerate under Kubuntu, the prize is real: 8 GB of extra VRAM beside the internal 4 GB, on the machine the Wexford tandem just passed 100% on.

*Companion: `kubuntu-dualboot-walkthrough-checklist.md` (the 7 phases this slots into, at Phase 1).*
