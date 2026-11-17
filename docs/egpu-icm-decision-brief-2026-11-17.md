# eGPU — ICM-claim decision brief (Tue Nov 17, 2026)

The author paused this decision on the night of Nov 16 with "in the morning."
This is that morning. Today is a light day — coffee with Rex is the only
agenda item — so there is room to decide without a session attached.

## What last night proved (closed, on evidence)

- The EFI road is closed. The shim's placement holds rock-stable inside EFI
  (BAR0 0x4000000000, COMMAND 0x0007 at the +10 s recheck), disconnecting
  every EFI Thunderbolt driver changes nothing (the tunnel is held by the
  controller's own ICM firmware, not the drivers), and the card is absent
  from the kernel's first PCI scan — it appears only at ~29.5 s, virgin,
  after the enclosure power-cycles it at the handover.
- Every earlier road is also closed on evidence: SMC firmware (Linux's and
  Apple's own 0x1f0e00 stall identically), CGTT clock table (cold card
  already holds Apple's values), warm macOS handoff (card resets across
  the reboot), the one-byte SMC patch, B1 soft-reset, Apple-TOC replay.
- Display-only B2 was rejected by the author on Nov 16 — not re-pitched here.

## The one road left

An ICM-claim shim: an EFI-stage program that speaks the Thunderbolt
controller's NHI/ICM mailbox protocol and claims the controller before the
handover, so the tunnel never drops and the card arrives in the kernel's
first PCI scan still warm — the way the internal twin does.

- Prize: a warm card in the first scan is the one state never yet tested
  against the SMC load wall. If the wall falls, full AI compute under
  Linux — the goal the author set ("full function or nothing").
- Cost: the deepest build of the project. NHI/ICM claiming from EFI is
  new territory — no existing shim code does this part; it is a research
  build, not an evening's patch.
- Risk to the prize: unguaranteed. Even with the tunnel held, the kernel's
  own Thunderbolt stack might reset the controller on first contact, and
  the SMC wall might live in the cold-card state regardless of how the
  card arrives. Tonight's evidence cannot rule either out.

## The alternatives

1. Build the ICM-claim shim (above).
2. Park the hunt where it stands — the map is fully written down, the
   eGPU still works fully under macOS, and WednesdayLM 1.5 core runs
   without it. Nothing decays by waiting.
3. A different dig on the cold-card side — keep attacking the SMC load
   wall in the state every frontier run actually starts from.

## Regardless of the decision — one housekeeping command

The updated egpu-boot-setup.sh (with the SDHCI blacklist that makes the
built-in SD reader inert under Linux, so its interrupt-timeout loop can
never bury a login again) was handed over on Nov 16 night. If it hasn't
been re-run on the Kubuntu desktop yet, one run —
`bash ~/egpu/egpu-boot-setup.sh` — closes that loop permanently.
macOS keeps using the reader as normal.
