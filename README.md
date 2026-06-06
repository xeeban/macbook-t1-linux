# A T1 MacBook Pro on Linux — the journey

Turning a **2016 13" MacBook Pro with the Touch Bar (MacBookPro13,2, the "T1" generation)** into a clean, distraction-free Arch Linux "writer's deck" — and fixing, one at a time, the hardware quirks that make this specific model a trap.

**The point of this repo:** if you're putting Arch (or any modern Linux) on this model, these writeups should save you the days of frustration each fix took to find. Each one leads with what actually works, names the **dead ends so you don't repeat them**, and explains the real root cause — usually found by reading kernel/driver source, not forum threads. Where a coding agent can do the work for you, there's an `AGENT_SPEC.md` you can point it at.

## The machine

- **MacBookPro13,2** — 2016 13" MacBook Pro, **T1** Touch Bar, no physical function row.
- **Arch Linux**, GNOME on Wayland, kernel `7.0.10-arch1-1`.
- Sleep: `s2idle` (correct for Intel Macs — S3 "deep" resume is broken in Apple's firmware).

## The journey

| # | Quirk | Status | Root cause (short) | Writeup |
|---|---|---|---|---|
| 1 | **Touch Bar dark** — no esc, F-keys, brightness | ✅ Fixed | The out-of-tree `apple-ib-drv` shipped `appleib_ll_parse()` gutted to a no-op, so the bar's virtual HID sub-devices were rejected with `-ENODEV` and never bound. | [`touch-bar/`](./touch-bar/) |
| 2 | **Never resumes from sleep** — suspend wedges, hard power-down required | ✅ Fixed | The **Apple S3X NVMe controller** (`106b:2003`) carries no `SIMPLE_SUSPEND` quirk, so `nvme_suspend()` keeps it powered across `s2idle`; the controller can't recover and falls off the bus on wake (`I/O timeout → Identify -4 → reset -5` → root fs read-only). Fix: `intel_idle.max_cstate=1` + `nvme_core.default_ps_max_latency_us=0` + a `d3cold_allowed=0` udev rule, plus two `systemd-sleep` hooks. | [`sleep-resume/`](./sleep-resume/) |
| 3 | **Wi-Fi won't associate** — BCM43602 | ✅ Fixed¹ | `wpa_supplicant` 2.11 ⊗ `brcmfmac` firmware-offloaded handshake (`FWSUP`/`FWAUTH`) bug; fixed with `feature_disable=0x82000` so the supplicant does the handshake host-side. | [`wifi/`](./wifi/) |
| 4 | **No sound** — speakers + headphone jack silent | ✅ Fixed | The **Cirrus CS8409** codec drives external Maxim amps over I²C/TDM via undocumented vendor-node writes; the in-tree `cs8409` module loads but never programs the amps, so a working-looking pipeline emits nothing. Fix: install **davidjo's `snd_hda_macbookpro`** patched CS8409 driver via DKMS. | [`audio/`](./audio/) |
| 5 | **No Bluetooth** — no `bluetoothctl`, nothing connects | ✅ Fixed | A false alarm: the UART Broadcom controller (`btbcm`/`hci_uart`) enumerates fine with its real Apple BD_ADDR — the radio was never broken. Arch just doesn't install **`bluez-utils`** (no `bluetoothctl`) or enable **`bluetooth.service`**. Two commands, no driver/firmware work. | [`bluetooth/`](./bluetooth/) |

¹ Fixed for standard APs (DHCP + internet, validated). A **mesh** router that enforces 802.11r/band-steering still rejects association (`status 16`) — an AP-side limitation, not the card; details in the [Wi-Fi writeup](./wifi/#known-limitation).

> **Where it stands:** all five fixed and the deck is in daily use — including real, unattended idle-suspend/resume cycles (validated 2026-06-05). Touch Bar lit, Wi-Fi connected, sleep/wake reliable, sound working, **Bluetooth paired (A2DP audio confirmed)**.

## A red herring worth calling out

The resume failure (#2) *looked* like the Touch Bar driver — `apple_ibridge` has a real use-after-free in its suspend callback that Oopses on wake. We patched it. **The machine still wouldn't resume.** Unloading the Touch Bar stack before suspend and watching it *still* wedge is what cleared the false trail and pointed at the NVMe controller. If you take one thing from this repo: **prove the suspect before you sentence it** — one cheap test saves days.

## The throughline

This model trips Linux at several different layers, and each needed a different kind of fix:

- An **out-of-tree HID driver** shipped subtly broken (Touch Bar).
- A **kernel power-management default** that's wrong for this controller (NVMe resume).
- A **userspace ⊗ firmware** handshake interaction (Wi-Fi).
- An **in-tree codec driver** that loads but doesn't do the model-specific amp bring-up (audio).
- A pair of **distro defaults** that leave a perfectly good radio inert (Bluetooth — missing `bluez-utils`, disabled service).

The common lesson isn't "it's always the Apple drivers" — it's that **the answer was in the source (kernel, driver, or the actual crash), not in the pile of community workarounds for the wrong problem.** T1 ≠ T2; "just use mainline," "switch to deep sleep," and "it must be muted / a PipeWire problem" are all dead ends on this hardware. And the inverse trap is just as real — **Bluetooth looked like another driver saga and was actually two missing defaults**, so diagnose each radio on its own evidence rather than assuming the pattern repeats.

## Why bother

A nine-year-old laptop, written off as e-waste by its OS's end-of-life, is now a fast, silent, single-purpose writing machine with a battery that lasts. Old hardware is worth saving.

---

*By [Nori Nishigaya](https://github.com/xeeban) · Victoria, BC · [Emergent Insights](https://emergentinsights.substack.com/)*
