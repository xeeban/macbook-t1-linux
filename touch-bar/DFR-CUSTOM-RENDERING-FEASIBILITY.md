# Custom Touch Bar rendering on T1 (Linux) — feasibility report

> **Question:** Can we get macOS-style custom Touch Bar content on the T1 (MacBookPro13,2) under Linux — arbitrary buttons/labels, and per-app layers (e.g. an Obsidian layout) — rather than just the firmware's three predefined layouts?
>
> **Short answer:** **PROVEN — done.** On 2026-06-12 we drove a custom BGR888 frame to the T1 Touch Bar from Linux and **the whole bar lit magenta.** The userspace spike (`dfr-switch.c`) enters USB "display mode" (config 2), speaks the `appletbdrm` protocol (GINF → REDY → frame), and renders arbitrary pixels. Per the prior-art survey this is a **first** for a T1 (`05ac:8600`) on Linux. **Outcome:** this scoping doc planned a `tiny-dfr` route — but `tiny-dfr` is T2/Apple-Silicon-only, so what shipped is a kernel DRM driver (`kernel/`) **plus a bespoke userspace stack (`dfrd/`)** that renders a layered, colour-Nerd-Font-icon, interactive bar and installs persistently. See [`dfrd/README.md`](./dfrd/README.md). The reverse-engineering below is preserved as the historical scoping record.
>
> *Status: core feasibility PROVEN end-to-end (pixels on screen). Productization (kernel module) in progress. Date: 2026-06-12.*
>
> **The unlock, in one line:** a raw `SET_CONFIGURATION(2)` is reverted to config 1 in ~45 ms because **`apple_ibridge.c:541` force-selects config 1 whenever it probes in a non-default config**; disabling `drivers_autoprobe` (so `usbhid`→`apple_ibridge` never binds) lets config 2 hold, and then GINF/REDY/frame just work.
>
> **Proven protocol details (T1, 2026-06-12):**
> - **Display**: GINF reports **2170×60, 24 bpp**, `bytes_per_row=6510`, pixel_format `0x52474241`. Frame = appletbdrm `fb_request` to bulk OUT `0x02`.
> - **Pixel byte order is R,G,B** on the wire (NOT BGR — empirically: a `{0,0,255}` pixel renders blue). Framebuffer is **W rows × H px (portrait, panel rotated 90°)** — long axis = `index / height`.
> - **Frame-completion ack is 16 bytes**, not T2's 40-byte `UDCL` — the kernel `appletbdrm` port must tolerate the short ack.
> - **Touch** (display mode): the firmware streams a HID report on interrupt EP **`0x83` (interface 2)**; first 4 bytes = **little-endian float32 = X position, range ≈ [0.5, 1.0]** across the bar (left edge 0.500, right edge 0.997). Map: `nx = (x − 0.5) × 2`. Brightness/volume do NOT fire in config 2 — touch is fully host-controlled.
> - **End-to-end demo working** (`dfr-switch buttons`): 4 drawn zones + tap → `uinput` keystroke, axes aligned.

---

## 1. The verdict in one paragraph

The T1 Touch Bar is a **2170×60 MIPI-DSI panel** that can run in two completely different modes. In **"simple mode"** the T1 bridge chip renders three built-in layouts itself (Esc / F1–F12 / control-strip) and the host just sends a HID "pick a layout" command — this is all the current Linux driver (`apple-touchbar`) does. In **"display mode"** the host composes pixels and **streams BGR24 frames over a USB bulk endpoint** to the panel — this is what macOS uses for every custom NSTouchBar UI. Display mode lives in a **different USB configuration** that Linux never selects. We confirmed by descriptor dump that **this machine's iBridge exposes that display configuration** (config 2, with a class-0x10 Audio/Video interface carrying a bulk OUT endpoint). So custom rendering is not blocked by the hardware — it is blocked by the absence of a Linux driver that (a) switches the device into display mode and (b) speaks the pixel protocol. Both are known quantities.

---

## 2. Evidence: the hardware exposes the pixel path (confirmed on this machine)

`apple-touchbar.c`'s own header says it plainly:

> *"MacOS supports a fancy mode … this driver supports the simple mode that consists of 3 predefined layouts."*

The current driver's entire vocabulary is four HID mode commands — `MODE_ESC` (0), `MODE_FN` (1, F1–F12), `MODE_SPCL` (2, control strip), `MODE_OFF` (3) — plus the `fnmode` policy knob. **No pixels.**

But the iBridge USB device (`05ac:8600`) advertises **three USB configurations** (`bNumConfigurations=3`, `bDeviceClass=ef` multi-function). Parsing the raw descriptors on this machine:

| Config | What it contains | Used by |
|---|---|---|
| **1** | Video (camera) + 2× HID (touch bar simple-mode + keyboard) | **Linux today** |
| **2** | Video + HID + **class 0x10 (Audio/Video) iface w/ bulk EP 0x02 OUT + 0x85 IN** + CDC comm/data (bulk) + vendor-specific 0xff (f9/11, bulk EP 0x05 OUT/0x88 IN) | **macOS display mode** |
| **3** | Video + 2× HID + CDC | (alt / management) |

The smoking gun is **Config 2, Interface 3: USB class `0x10` (Audio/Video Devices) with a bulk OUT endpoint `0x02`.** That is the framebuffer sink — the endpoint macOS streams pixel frames to. The CDC pair (intf 4/5) and the vendor-specific interface (intf 7) are the control/handshake channels. Crucially, **the camera and keyboard interfaces still exist in config 2**, so switching configurations does not cost us the webcam or the keyboard.

**This means the custom-rendering capability is physically present and currently dormant on the machine.** Linux simply never asks for config 2.

---

## 3. Prior art: this is a port, not a blind RE

- **The protocol is reverse-engineered.** Ben (Bingxing) Wang's *"A deep dive into Apple Touch Bar"* documents: two USB configurations (HID vs display), **host-composed pixels in BGR24**, **frames > ~54 KB** with a semi-static header carrying a **frame ID** and a **rectangular dirty-region** (x/y/w/h), the **2170×60** panel, MIPI-DSI in **command mode** (not video mode). The RE notes treat **T1 and T2 as the same pixel protocol**.
- **A working reference driver exists:** **`DFRDisplayKm`** (Windows kernel driver) implements exactly this — config switch + frame streaming. It is a porting reference (license needs checking before any code is copied; a clean-room reimplementation from the protocol description is the safer path).
- **T2 has a mainline Linux analogue:** `appletbdrm` (Linux 6.15) drives the T2 Touch Bar (`05ac:8302`) as a DRM display, and `tiny-dfr` (Asahi) renders buttons to it with cairo + libinput. **Neither supports T1** (`appletbdrm` only registers `8302`; its own patch says "Testing on T1 Macs would be appreciated"). But `appletbdrm` + `tiny-dfr` are the architectural template: a tiny DRM panel driver underneath, a userspace button-renderer on top.
- **Negative confirmation:** no existing project renders custom content to a **T1** Touch Bar on Linux. We would be first (consistent with the relight result — see [THE-RELIGHT-HUNT.md](./THE-RELIGHT-HUNT.md)).

---

## 4. What "per-app layers like Obsidian" actually requires

Two independent pieces, of very different cost:

### 4a. The renderer (the hard 90%)
A driver that exposes the Touch Bar as a **DRM display** (mirror the `appletbdrm` design for the T1 transport), so that `tiny-dfr` — which already does buttons, SVG icons, and multitouch hit-testing — can drive it **for free**. Sub-parts:

1. **Mode/config switch** — get the iBridge into config 2 (or whichever the display interface lives in) without breaking camera/keyboard, and coexist with `apple_ibridge`/`apple_touchbar`. This is the riskiest integration point: the existing demux driver owns the device in config 1.
2. **Panel bring-up + framebuffer** — bind the class-0x10 interface, do the init handshake (likely over the CDC/vendor channel), stream BGR24 dirty-rect frames to bulk EP `0x02`, handle backlight/dim/off.
3. **Touch input** — recover multitouch in display mode (digitizer over HID, or coordinates over the vendor channel) and feed it to userspace as a libinput touch device so `tiny-dfr` can map taps to buttons.
4. **DRM glue** — present 2170×60 BGR24 as a DRM device so `tiny-dfr` attaches unmodified (ideal), or write a small bespoke renderer.

### 4b. Per-app switching (the easy 10%)
Watch the focused app and tell the renderer which layout to show. On **Wayland/GNOME 50.2** this needs the **`focused-window-dbus`** GNOME Shell extension (flexagoon, ext ID 5592) — it exposes `wm_class`/`app_id` of the focused window on D-Bus (`org.gnome.shell.extensions.FocusedWindow`). Wayland has no other external focus signal by design. `tiny-dfr` already supports layered/configurable buttons; wiring app→layout is a small daemon or a `tiny-dfr` config feature.

**Note:** 4b is *also* the entire deliverable of the cheaper "Option B" (per-app switching among the **firmware presets**, no custom pixels). If the renderer (4a) proves too costly, 4b alone still gives "Obsidian → F-keys, else → media strip" — just without custom labels.

---

## 5. Phased plan (de-risk before committing)

**Phase 0 — Protocol confirmation (low risk, do first).**
- Finish pulling the exact wire protocol (frame header bytes, init sequence, touch path) from the imbushuo writeup + `DFRDisplayKm` source. *(in progress — research agent running.)*
- USB-capture macOS or the Windows driver talking display mode if a reference frame dump is needed for the bytes the blog doesn't give.

**Phase 1 — Read-only probe (low risk).**
- From userspace (libusb), switch a *spare* test into config 2 and enumerate; confirm the class-0x10 interface claims and the endpoints behave as described. **Do not** ship this on the daily-driver without the teardown-safety lessons from #7 — config switching while `apple_ibridge` is bound is exactly the class of operation that GPF'd before the OOB fix.

**Phase 2 — One frame (medium risk).**
- Userspace libusb spike: do the init handshake, push a single solid-color BGR24 frame to bulk OUT, see the bar light with our pixels. This is the go/no-go milestone — if we can paint one frame, the rest is engineering.

**Phase 3 — DRM panel driver (high effort).**
- Promote the spike to a kernel DRM tiny driver (or a stable userspace daemon), modeled on `appletbdrm`. Coexist cleanly with `apple_ibridge` (config ownership, suspend/resume, the hibernate relight path).

**Phase 4 — `tiny-dfr` + per-app (low effort once Phase 3 lands).**
- Attach `tiny-dfr`; add the `focused-window-dbus` extension + an app→layout map. Obsidian layer ships here.

---

## 6. Risks & unknowns (honest list)

- **Config-switch coexistence.** `apple_ibridge` owns the device in config 1. Switching to config 2 underneath it, or teaching it to manage config 2, is the central integration risk. The #7 OOB/teardown saga shows this device punishes sloppy bind/unbind with GPFs and D-state deadlocks. Whatever we build must respect that.
- **T1-specific protocol deltas.** The RE sources lean T2/"both"; T1 may differ in the handshake, the init, or which channel carries touch. Phase 2 flushes this out.
- **Suspend/hibernate.** We just spent a whole saga getting the bar to relight after hibernate (#7). A display-mode driver must re-establish display mode on resume, or we regress the hard-won relight.
- **Touch in display mode.** Whether multitouch returns as a clean HID digitizer (great — libinput just works) or as raw vendor-channel data (more work) is unconfirmed.
- **`DFRDisplayKm` license** — reference for understanding, but clean-room from the protocol description unless its license permits derivation.
- **Effort.** Realistically **weeks of focused work**, kernel USB/DRM territory, with a genuine chance Phase 2 reveals a T1 wrinkle that adds more. Not a weekend project. The payoff: first-ever custom T1 Touch Bar on Linux, and an Obsidian (and editor, and terminal) layer.

---

## 6b. Phase-2 attempt log (2026-06-12) — the display-mode *entry* is the real blocker

We built and ran a userspace libusb spike (`dfr-spike.c` + `dfr-spike.sh`) that faithfully replays the upstream `appletbdrm` protocol (struct sizes verified byte-exact: 32/65/12/80/48/40). It assumes the device is in config 2 and was driven by switching config via sysfs (`echo 2 > .../bConfigurationValue`). **Result: the switch did not stick and it wedged the Touch Bar firmware.**

What actually happened:
- `echo 2 > bConfigurationValue` returned success but the device **stayed in config 1** (kernel-side interface bind half-failed and rolled back), **while the bridge firmware had already dropped simple-mode rendering** → the panel went **dark with nothing driving it**.
- Neither the `apple_ibridge` stack reload (the #7 relight), a full USB driver unbind/bind re-enumeration, a config `0→1` cycle, **nor a hibernate (S4) lid-close** brought it back. **Only a full reboot** re-initialised the bridge and restored the Esc key.

Why the "proper trigger" idea collapsed: reading the mainline T2 stack shows **`hid-appletb-kbd` only does HID mode switching** (ESC/FN/SPCL/OFF), **not** a USB config switch. On **T2** the Touch Bar is a *separate* USB device (`8302`) that simply **defaults** to exposing the Audio/Video display interface, so `appletbdrm` binds with no switch. On **T1** the display interface is gated behind **config 2 of the composite iBridge**, and **no open-source code knows how to enter it** — that transition lives only in macOS's proprietary bridge driver. Entering display mode tears down the firmware's simple-mode rendering, so any half-completed or unbacked transition leaves the bar dark until a bridge reset (reboot).

**Refined blocker:** the gate is not the pixel protocol (known) — it's the **firmware-blessed sequence that moves the T1 iBridge into display mode** (and the command to return it to simple mode). Brute-forcing `SET_CONFIGURATION` is **not** that sequence.

### Second attempt (atomic libusb switch, `dfr-switch.c`) — same wall, sharper data

To rule out "the kernel rolled back" we did the switch entirely in one libusb process: detach all kernel drivers → `libusb_set_configuration(2)` → claim the AV interface → GINF, with no unattended gap. Result:

```
detached intf 0,2,3
libusb_set_configuration(2) -> 0 (ok)
config now = 1          <-- device ignored it
```

`set_configuration` returned **success**, but `get_configuration` read back **1**, and dmesg showed the iBridge **re-enumerating** (uvcvideo re-found, every virtual HID rebinding). So the device **ACKs `SET_CONFIGURATION(2)`, then USB-resets itself back to config 1** — confirmed now via *both* sysfs and libusb. The bar went dark and the `apple_ibridge` relight-reload did **not** recover it (reported 2 sub-HIDs bound, panel still black) → reboot.

**Interpretation (strong hypothesis):** On Windows the display config is selected by `DFRUsbCcgp.inf` — the USB **Generic Parent (UsbCcgp)** composite driver — which owns the whole device and picks the configuration **at enumeration**, before function drivers attach. On Linux the iBridge comes up at config 1 and `uvcvideo`/`usbhid` claim it immediately; a *late* switch makes the firmware reset. The entry trigger is therefore most likely **"come up in / select config 2 at enumeration time,"** not a runtime switch — or a vendor request the parent issues first. Pinning this down is the current task (reading DFRDisplayKm/DFRUsbCcgp); a macOS/Windows usbmon capture is the fallback that settles it definitively.

**To unblock safely requires a macOS USB capture** (usbmon/PacketLogger of the internal `05ac:8600` bus during Touch Bar display use): the exact control/HID sequence macOS issues to enter display mode, what claims the AV interface and how fast, and the return-to-simple-mode command. Until that handshake is known, further live experimentation on the only T1 just costs reboots for little information.

## 7. Recommendation

Phase 2's first attempt hit the wall in §6b: the pixel protocol is not the blocker, **display-mode entry is**. The *method* was wrong, though — sysfs `echo 2` switches config and then leaves the firmware in display mode with nothing driving it (and the kernel rolled the switch back). 

**This is a dedicated experimentation machine, not a daily driver — reboots are an acceptable cost.** So the plan is to drive the transition **atomically from one libusb program**: detach the kernel drivers, issue `SET_CONFIGURATION(2)` directly, immediately claim the Audio/Video interface, and run GINF→REDY→frame before the firmware times out — then restore config 1 (or reboot). The libusb `set_configuration` return code + dmesg will also reveal *why* the kernel-side switch rolled back. See `dfr-switch.c`.

A **macOS usbmon/PacketLogger capture** of the `05ac:8600` bus during display use remains the highest-information unblock if the atomic libusb switch also fails — it gives the exact firmware-blessed entry/exit handshake.

**Option B** (per-app switching among firmware presets via `focused-window-dbus` + a live `fnmode` write) stays in reserve as the zero-risk fallback that still delivers "Obsidian gets its own layer," minus custom labels.

---

*Companion docs: [README.md](./README.md) · [THE-RELIGHT-HUNT.md](./THE-RELIGHT-HUNT.md) · driver source `apple-touchbar.c`. Protocol appendix to follow from the in-flight research dig.*
