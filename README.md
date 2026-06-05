# A T1 MacBook Pro on Linux — the journey

Turning a **2016 13" MacBook Pro with the Touch Bar (MacBookPro13,2, the "T1" generation)** into a clean, distraction-free Arch Linux "writer's deck" — and fixing, one at a time, the hardware quirks that make this specific model a trap.

Each fix below is its own deep-dive writeup: the wrong turns, the real root cause (usually found by reading kernel source, not forums), and a copy-pasteable patch. Where a coding agent can do the work for you, there's an `AGENT_SPEC.md`.

## The machine

- **MacBookPro13,2** — 2016 13" MacBook Pro, **T1** Touch Bar, no physical function row.
- **Arch Linux**, GNOME on Wayland, kernel `7.0.10-arch1-1`.
- Sleep: `s2idle` (correct for Intel Macs — S3 "deep" resume is broken in Apple's firmware).

## The journey so far

| # | Quirk | Status | Root cause (short) | Writeup |
|---|---|---|---|---|
| 1 | **Touch Bar dark** — no esc, F-keys, brightness | ✅ Fixed | The out-of-tree `apple-ib-drv` shipped `appleib_ll_parse()` gutted to a no-op, so the bar's virtual HID sub-devices were rejected with `-ENODEV` and never bound. | [`touch-bar/`](./touch-bar/) |
| 2 | **Resume wedge** — long sleep returns to a lock screen that ignores all input | ✅ Fixed¹ | Same driver: `appleib_hid_suspend()` dereferenced a **freed** sub-device (`appleib_remove_device()` never NULL'd the array slots). The use-after-free Oopsed a PM worker and wedged HID input on resume. | [`sleep-resume/`](./sleep-resume/) |
| 3 | **Wi-Fi won't associate** — BCM43602 | ✅ Fixed | `wpa_supplicant` 2.11 ⊗ `brcmfmac` firmware-offloaded 4-way handshake (FWSUP/SAE) bug; fixed with `feature_disable=0x82000`. | _writeup TBD_ |

¹ Patched, built, and disassembly-verified; final resume confirmation pending the first post-reboot suspend test — see the [sleep-resume](./sleep-resume/#validation) writeup.

## The throughline

Three different symptoms, one repeated lesson: **the out-of-tree Apple drivers are where this model goes wrong, and the answer is almost always in the kernel/driver source — not in the stack of community workarounds for the wrong problem.** T1 ≠ T2; "just use mainline" and "switch to deep sleep" are both dead ends on this hardware.

## Why bother

A nine-year-old laptop, written off as e-waste by its OS's end-of-life, is now a fast, silent, single-purpose writing machine with a battery that lasts. Old hardware is worth saving.

---

*By [Nori Nishigaya](https://github.com/xeeban) · Victoria, BC · [Emergent Insights](https://emergentinsights.substack.com/)*
