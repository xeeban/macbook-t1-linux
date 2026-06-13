# `dfrd` — a programmable Touch Bar for the T1 (userspace stack)

This is the userspace layer that turns the T1 Touch Bar into a **macOS-style, layered, icon-driven, interactive bar** on Linux. It sits on top of the kernel display driver in [`../kernel/`](../kernel/) (which exposes the bar as a real DRM card, `/dev/dri/card*`, and routes touch via `hidraw`).

Per a survey of ~9 years of prior art, this is the **first** custom, interactive Touch Bar UI on a **T1** Mac under Linux. The protocol it speaks and the T1 quirks it works around are written up in [`../DFR-CUSTOM-RENDERING-FEASIBILITY.md`](../DFR-CUSTOM-RENDERING-FEASIBILITY.md). (We built this bespoke renderer rather than porting Asahi's `tiny-dfr`, which is T2/Apple-Silicon-only and can't drive the T1.)

## What it does

- **Renders** labelled, coloured buttons to the bar with **FreeType + a Nerd Font** (JetBrainsMono) — real wifi / battery / bluetooth / brightness / media-transport icons, not just text.
- **Reads touch** from the non-standard digitizer (a float32-X HID report on EP `0x83`, read via `/dev/hidraw` so it coexists with `hid-generic`), maps the X position to the drawn button, and injects the key via `uinput`.
- **Switches layers like macOS** by watching the keyboard's `Fn` key (+ modifiers):

  | Shown | Layer | Contents |
  |---|---|---|
  | default | `media` | brightness / media transport / mute / volume (icons) |
  | hold **Fn** | `fn` | Esc + F1–F12 |
  | **Ctrl+Fn** | `ctrl` | live **system row** — kbd-light / battery% / wifi / bluetooth icons with dynamic colour; **tap → opens that GNOME Settings panel** |
  | **Opt+Fn** | `alt` | F13–F24 (bind to anything in GNOME Settings → Keyboard) |
  | **Cmd+Fn** | `meta` | media-transport strip |

## Files

| File | Role |
|---|---|
| `dfr-render.c` → `dfr-render` | libdrm/KMS renderer. Finds the `appletbdrm` card, takes DRM master, draws the active layout (FreeType, anti-aliased, rotation handled). `--preview out.ppm` renders offline (no root/DRM) for quick iteration. Live indicators refresh every 2.5 s. |
| `dfr-touchd.c` → `dfr-touchd` | hidraw digitizer → `uinput` key injection; `DFR_ACT_CMD` buttons launch a command in the desktop user's session. |
| `dfr-fnd.c` → `dfr-fnd` | reads the Apple SPI Keyboard, momentary layer switching via `SIGRTMIN+i` to the other two daemons. |
| `dfr-layout.h` | **the layouts** — buttons, keycodes, icons (Nerd Font glyph macros), colours, indicator types. Edit here to customise; both daemons rebuild from it. |
| `dfrd-run.sh` | runs all three daemons (default layout `media`) with clean teardown. |
| `dfr-switch.c` | the original libusb **spike** that proved the protocol (display + touch) before the kernel driver existed. Kept for reference. |
| `install/` | persistence: `install.sh` / `uninstall.sh` / `dfrd.service` / udev rule / config-aware hibernate hook / `PERSISTENCE.md`. |
| `DFRD-RUNBOOK.md` | step-by-step bring-up + test commands (TESTS A–F). |

## Prerequisites

The kernel display stack must be loaded (it puts the bar in USB "display mode" and creates the DRM card) and the udev seat rule installed so GNOME/mutter doesn't hold DRM master. See [`../kernel/RUNBOOK.md`](../kernel/RUNBOOK.md). In short, once per session:

```sh
sudo install -m0644 99-touchbar-dfr.rules /etc/udev/rules.d/ && sudo udevadm control --reload
cd ../kernel/t1-touchbar-display
sudo modprobe -r apple_touchbar apple_ibridge
sudo insmod ./appletbdrm.ko && sudo insmod ./apple_dfr_cfgsel.ko
```

## Run it

```sh
make
sudo ./dfrd-run.sh          # media strip by default; hold Fn for F-keys
```

## Make it permanent (auto-start on boot, hibernate-safe)

```sh
make
sudo ./install/install.sh   # DKMS + udev + dfrd.service
sudo reboot
```

Verify + uninstall: see [`install/PERSISTENCE.md`](./install/PERSISTENCE.md).

## Customise

Everything visual/behavioural is data in `dfr-layout.h`: add or reorder buttons, change icons (Nerd Font codepoints, documented inline), set per-button colours, or point a button at a command. Rebuild with `make`. Brightness/orientation knobs (`FONT_PX_MAX`, `DFR_FONT=...`, `--flip-*`) are in `dfr-render.c`.

## Not yet done

- **App-aware auto-switch** (e.g. Obsidian gets its own layer) via the `focused-window-dbus` GNOME extension — the infrastructure (named layers, `SIGRTMIN+i` set) is in place; see `PERSISTENCE-AND-ROADMAP.md`.
- **Tap-to-latch** so the system layer stays up for one-handed tapping instead of holding Ctrl+Fn.
