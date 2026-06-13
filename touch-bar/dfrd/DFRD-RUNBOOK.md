# DFRD runbook — custom T1 Touch Bar, userspace stack

Built 2026-06-12 on top of the validated kernel stack (`../kernel/RUNBOOK.md`).
Everything below is **copy-paste, in order**. Steps marked 🔬 are live
hardware tests — outcomes unknown until run.

What you get at the end: a drawn Esc+F1–F12 row on the Touch Bar, taps
injecting real key events, all torn down cleanly with Ctrl-C.

```
component map
  kernel (already proven):  apple_dfr_cfgsel.ko + appletbdrm.ko  -> /dev/dri/cardN "appletbdrm"
  this dir:                 99-touchbar-dfr.rules  -> frees the card from mutter (seat0)
                            dfr-render             -> KMS client, draws the function row
                            dfr-touchd             -> hidraw float32-X -> uinput keys
                            dfrd-run.sh            -> starts/stops both
```

## 0. Build (no root — already done, binaries in this dir)

```sh
cd ~/Code/xeeban/macbook-t1-linux/touch-bar/dfrd
make            # cc -O2 -Wall -Wextra ... (compiles clean)
```

## 1. Install the seat udev rule (BEFORE loading modules)

```sh
cd ~/Code/xeeban/macbook-t1-linux/touch-bar/dfrd
sudo install -m0644 99-touchbar-dfr.rules /etc/udev/rules.d/
sudo udevadm control --reload
```

Why: GNOME/mutter takes DRM master on every seat0 card the moment it appears.
The rule re-points the iBridge card's `ID_SEAT` to a dummy seat so mutter
ignores it (it filters hotplugged cards by seat). The rule must be live
**before** the card is created — that's why this is step 1.

## 2. Load the kernel display stack

```sh
cd ~/Code/xeeban/macbook-t1-linux/touch-bar/kernel/t1-touchbar-display
sudo modprobe -r apple_touchbar apple_ibridge
sudo insmod ./appletbdrm.ko
sudo insmod ./apple_dfr_cfgsel.ko
sleep 5
```

## 3. Verify the card exists AND escaped the desktop  🔬 TEST A

```sh
cat /sys/bus/usb/devices/1-3/bConfigurationValue     # expect: 2 (and stays 2)
for c in /sys/class/drm/card[0-9]*; do echo "$c -> $(basename $(readlink -f $c/device/driver))"; done
# expect one card -> appletbdrm (likely card0 or card2; Intel i915/card1 is the desktop — untouched)

udevadm info /dev/dri/cardN | grep -E 'ID_SEAT|TAGS'   # <- use the appletbdrm card number
# expect: E: ID_SEAT=seat-touchbar  and TAGS containing :seat-touchbar:
#         and NOT :master-of-seat:
```

- `ID_SEAT=seat-touchbar` missing → rule didn't match: run
  `udevadm test /sys/class/drm/cardN 2>&1 | tail -20` and paste the output.
- ID_SEAT correct but the renderer in step 4 still gets `EACCES` on SetCrtc →
  mutter grabbed it anyway (it was faster than the rule, or mutter ignores
  seat for this path): first retry `sudo rmmod apple_dfr_cfgsel appletbdrm`
  then redo step 2 (fresh card with rule definitely active); if it STILL
  fails, log out/in of GNOME once; last resort `sudo chvt 3` and run from the
  bare VT (this always works — mutter is suspended there).

## 4. First pixels: renderer alone  🔬 TEST B

```sh
cd ~/Code/xeeban/macbook-t1-linux/touch-bar/dfrd
sudo ./dfr-render -l fn
```

Expected output: card path, `mode 60x2170`, `mode set; layout 'fn' (13 keys)`.
**Look at the bar** and check, in order:

1. 13 labeled buttons visible? (ESC slightly wider + warm-tinted, F1–F12)
2. **Is ESC on the physical LEFT?** If the row runs right-to-left:
   Ctrl-C, rerun with `--flip-long`. (Touch is independently proven
   left-to-right, so fix the DRAWING, never the touch map.)
3. **Do the labels read correctly?** Mirrored/upside-down glyphs: Ctrl-C,
   rerun adding `--flip-short`.
4. Note the flags that made it correct → they become the defaults (edit
   `g_flip_long`/`g_flip_short` init in dfr-render.c, rebuild, and tell the
   units/PERSISTENCE notes).

Leave it running for step 5 (open a second terminal).

## 5. Locate the digitizer + dry-run touch  🔬 TEST C

```sh
# informational — see the config-2 HID nodes the daemon will scan:
ls -l /sys/bus/usb/devices/1-3:2.*/0003:*/hidraw/ 2>/dev/null
grep -H . /sys/bus/usb/devices/1-3:2.*/bInterfaceNumber

cd ~/Code/xeeban/macbook-t1-linux/touch-bar/dfrd
sudo ./dfr-touchd -n -v        # dry run: prints, injects nothing
```

Expected: `digitizer: /dev/hidrawN (...)` picking the **interface 2** node.
Now tap the bar left to right:

- `first report: 52 bytes`, `float X found at report offset 0`
- `DOWN x=0.51.. -> zone 0 [ESC]` at the far left, `zone 12 [F12]` far right
- zone boundaries should match the drawn buttons (they share the layout math)

Failure modes:
- *no config-2 hidraw node*: hid-generic didn't bind the HID interfaces —
  paste `ls /sys/bus/usb/devices/1-3:2.*/` + `dmesg | tail -30`.
- *daemon picks interface 6 / reports don't parse*: paste a few `-v` report
  dumps; the float may live on the other interface or at another offset
  (the daemon already tries offsets 0 and 1).
- *reports keep streaming after finger up, keys stick*: release is detected
  by 150 ms of silence — if the firmware idles differently, we tune
  `RELEASE_MS` or key off the report contents (paste `-v` output of a
  tap-and-hold then release).

## 6. Full stack with key injection  🔬 TEST D

Stop the step-4/5 processes (Ctrl-C), then:

```sh
cd ~/Code/xeeban/macbook-t1-linux/touch-bar/dfrd
sudo ./dfrd-run.sh fn                  # add --flip-* flags found in step 4
```

Focus any text field / terminal, tap F-keys and ESC, confirm real keystrokes
land (e.g. tap inside `sudo libinput debug-events` or check a focused editor;
ESC should leave insert mode in vim). Layout swap test:

```sh
sudo kill -USR1 <render-pid> <touchd-pid>     # pids printed by dfrd-run.sh
# bar redraws as the media strip; taps now send volume/brightness/media keys
```

Ctrl-C on dfrd-run.sh tears both down (bar goes dark — normal: nothing is
driving the card; the kernel stack stays loaded and step 6 can rerun
immediately).

## 7. Teardown / recovery to stock

```sh
# stop daemons first (Ctrl-C on dfrd-run.sh), then:
sudo rmmod apple_dfr_cfgsel appletbdrm
echo 0 | sudo tee /sys/bus/usb/devices/1-3/authorized
echo 1 | sudo tee /sys/bus/usb/devices/1-3/authorized
sudo modprobe -a apple_ibridge apple_touchbar          # -a! (separate args)
sudo /usr/local/sbin/touchbar-relight-reload           # if the bar stays dark
# worst case: reboot (acceptable on this machine)
```

The udev rule can stay installed — it only matches the 05ac:8600 DRM card,
which doesn't exist in stock config 1.

## 8. Persistence (after 🔬 A–D all pass)

See `PERSISTENCE-AND-ROADMAP.md`: DKMS (kernel RUNBOOK Phases 2–3), then
binaries to /usr/local/bin, systemd units + `SYMLINK+="dri/touchbar"` +
`SYSTEMD_WANTS` in the udev rule.

---

# STEP 2 — Nerd Font rendering + ctrl/alt/meta layers (built 2026-06-12, NOT hardware-tested)

On top of the proven Step-1 stack, this revision adds:

- **FreeType text**: labels render anti-aliased from **JetBrainsMono Nerd Font**
  (fontconfig lookup, `DFR_FONT=/path.ttf` env override). UTF-8 labels may mix
  ASCII and Nerd Font icons; missing glyphs draw as U+FFFD.
- **Layout model**: `struct dfr_key` gained `action` (`DFR_ACT_KEY` /
  `DFR_ACT_CMD`), `cmd`, and `indicator` (`DFR_IND_BATTERY/WIFI/BT/KBDLIGHT`).
  `fn` and `media` are unchanged.
- **New layers** (dfr-fnd already routes these by name — nothing to wire):
  - `alt`  (Opt+Fn): F13–F24 (`KEY_F13..KEY_F24`) — GNOME-assignable.
  - `meta` (Cmd+Fn): media transport with icon glyphs
    (prev/play-pause/next/stop/mute/vol-/vol+).
  - `ctrl` (Ctrl+Fn): SYSTEM row — live kbd-backlight / battery / wifi / bt
    indicators; **tap opens the matching gnome-control-center panel** in the
    user's session.
- **Dynamic refresh**: with `ctrl` active, dfr-render re-reads the values every
  2.5 s (battery + kbd backlight + bt rfkill straight from /sys; wifi via
  `timeout 1 nmcli -t`, throttled) and redraws only on change.
- **Command launch as the user**: dfr-touchd (root) double-forks, drops to the
  desktop user (env `DFR_USER`/`DFR_UID` override; else owner of
  `/run/user/<uid>`; else uid 1000), sets `HOME`, `XDG_RUNTIME_DIR`,
  `DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/<uid>/bus`,
  `WAYLAND_DISPLAY=wayland-0`, `DISPLAY=:0`, and runs `/bin/sh -c "<cmd>"`.
  One launch per tap (down-edge latch + 300 ms debounce).

## Nerd Font glyph map (verified present in JetBrainsMonoNerdFont-Regular.ttf)

| Use | Codepoint | Name |
|---|---|---|
| prev / play-pause / next | U+F04AE / U+F040E / U+F04AD | nf-md-skip_previous / play_pause / skip_next |
| stop / mute / vol- / vol+ | U+F04DB / U+F075F / U+F075E / U+F075D | nf-md-stop / volume_mute / volume_minus / volume_plus |
| battery charging / full / 10–90 / empty | U+F0084 / U+F0079 / U+F007A–F0082 / U+F008E | nf-md-battery_* |
| wifi connected / on-disconnected / off | U+F05A9 / U+F092E / U+F05AA | nf-md-wifi / wifi_strength_outline / wifi_off |
| bt on / off | U+F00AF / U+F00B2 | nf-md-bluetooth / bluetooth_off |
| kbd backlight | U+F030C | nf-md-keyboard (F08DC "keyboard_brightness" renders as a file glyph in this font — avoided) |

## 9. Build + offline self-check (no root)

```sh
cd ~/Code/xeeban/macbook-t1-linux/touch-bar/dfrd
make                                     # render now links freetype2+fontconfig
for l in fn media ctrl alt meta; do
    ./dfr-render -l $l --preview /tmp/dfrd-$l.ppm
    magick /tmp/dfrd-$l.ppm /tmp/dfrd-$l.png
done
# eyeball the PNGs: icons crisp? F13-F24 present? ctrl row shows live
# battery/kbd values (preview reads /sys + nmcli unprivileged)?
```

## 10. New layers on hardware  🔬 TEST E

```sh
cd ~/Code/xeeban/macbook-t1-linux/touch-bar/dfrd
sudo ./dfrd-run.sh                       # default media; dfr-fnd drives layers
```

- Hold **Opt+Fn** → F13–F24 row. Tap a few; check with `sudo libinput
  debug-events | grep -E 'F1[3-9]|F2[0-4]'`, then bind one in GNOME Settings →
  Keyboard → Custom Shortcuts.
- Hold **Cmd+Fn** → icon media row; with music playing, tap play/pause + vol.
- Hold **Ctrl+Fn** → SYSTEM row. Check each indicator against reality
  (battery %, charge bolt when plugged, kbd backlight % after
  `brightnessctl -d 'spi::kbd_backlight' set 50%`, wifi/bt toggles in
  quick-settings flip the icons within ~2.5 s).

## 11. Command launch into the GNOME session  🔬 TEST F  ← TOP RISK

```sh
# dry-run first: prints "CMD: gnome-control-center ..." without launching
sudo ./dfr-touchd -n -l ctrl

# then live (full stack running): Ctrl+Fn hold, tap the battery button
# -> gnome-control-center should open ON the power panel, owned by your user
ps -o user,cmd -C gnome-control-center      # expect nnishigaya, not root
```

If nothing opens:
- `sudo -u nnishigaya env XDG_RUNTIME_DIR=/run/user/1000 \
   DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus \
   WAYLAND_DISPLAY=wayland-0 DISPLAY=:0 gnome-control-center wifi`
  — if THIS fails too, the env recipe is wrong for this session: check
  `ls /run/user/1000/` for the real `wayland-*` socket name and
  `loginctl show-session $(loginctl | awk '/nnishigaya/{print $1; exit}')`.
- if it works manually but not from the bar, suspect the double-fork env
  (run `sudo ./dfr-touchd -v -l ctrl` and watch for the CMD line).
- override the target user any time: `sudo DFR_USER=nnishigaya ./dfrd-run.sh`.

## STEP-2 open hardware questions, ranked by risk

1. **Launch-as-user into Wayland GNOME (TEST F).** The env quintet
   (HOME/XDG_RUNTIME_DIR/DBUS bus/WAYLAND_DISPLAY/DISPLAY) is the standard
   recipe but `WAYLAND_DISPLAY=wayland-0` is an assumption — GNOME on this
   box may use another socket name, and gnome-control-center may also want
   `XDG_CURRENT_DESKTOP=GNOME` (add to launch_cmd env if panels open weird).
   Verified manually-runnable fallback above isolates env vs daemon issues.
2. **Nerd glyph legibility at 60 px bar height on the physical panel.**
   Previews look crisp at 30 px type, but the panel's subpixel layout +
   the RGB888 conversion could soften thin strokes (wifi arcs, bt rune).
   Knobs: `FONT_PX_MAX` in dfr-render.c, or point `DFR_FONT` at
   `JetBrainsMonoNerdFont-Bold.ttf`.
3. **Indicator correctness on battery/radio edges**: Discharging vs Charging
   vs Full strings from BAT0, wifi radio-on-but-disconnected state parsing
   (`nmcli -t -f TYPE,STATE device status`), rfkill index stability for bt.
   Watch the ctrl row across a plug/unplug + airplane-mode cycle.
4. **One-launch-per-tap debounce (TEST F).** Down-edge latch + 300 ms guard
   should make taps singular, but a finger resting across the 150 ms release
   window could re-trigger; if double-launches appear, raise
   LAUNCH_DEBOUNCE_MS or debounce per-zone.
5. **Redraw cost / flicker on indicator refresh.** Full-bar redraw +
   DirtyFB every change (≤1 per 2.5 s) should be invisible, but if the
   panel blanks/tears, switch to per-button dirty rects.
6. **nmcli latency as root in the render loop.** `timeout 1` caps the stall
   (worst case: bar redraw delayed, touch unaffected — touchd never shells
   out for indicators). If it annoys, move wifi polling to rfkill sysfs.

---

## Open hardware-test questions, ranked by risk

1. **Does the seat rule actually free the card from mutter?** (TEST A/B)
   Highest risk: the mechanism (ID_SEAT filtering at hotplug) is how mutter is
   documented/coded to behave, but it's unverified on GNOME 50.2.
   Fallbacks staged: fresh module reload → relogin → bare VT (`chvt 3`,
   always works).
2. **hidraw delivery of the non-standard reports.** (TEST C) hid-generic
   should pass 52-byte raw reports through even though the descriptor is
   weird; if the descriptor declares a different report size, reads may
   differ from the wire (daemon handles offsets 0/1 and any length ≥ 4).
3. **Orientation of the drawn image** (TEST B): row 0 = physical left is
   proven via the USB spike, but the kernel scanout path could differ
   (panel-orientation property is a hint to compositors, not applied by the
   driver for dumb-buffer scanout)… and the short-axis (glyph) direction was
   never observable with solid color bands. `--flip-long` / `--flip-short`
   cover all 4 combinations.
4. **Release detection timing** (TEST C/D): 150 ms silence threshold copied
   from the dfr-switch behavior (timeout-as-release). May need tuning, or an
   explicit release report may exist (would make it crisper).
5. **Stray evdev device from hid-generic** (TEST C): if libinput starts
   seeing garbage pointer/key events from the digitizer's auto-created input
   node, enable the commented LIBINPUT_IGNORE_DEVICE rule (with its config-1
   caveat) — check `sudo libinput list-devices | grep -A3 -i bridge`.
6. **XRGB8888 path on real hardware** (TEST B): we render XR24 and rely on
   appletbdrm's conversion to the panel's RGB888; only RG24 was exercised by
   the USB spike. If colors/geometry look insane, that's the first suspect
   (cheap test: does `modetest -M appletbdrm -s <conn>:60x2170` look right?).
7. **Touchd vs hidraw node timing at module load** (persistence phase only).
