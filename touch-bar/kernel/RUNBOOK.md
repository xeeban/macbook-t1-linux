# T1 Touch Bar display mode — kernel runbook

Root fix: select the iBridge display configuration (config 2) **at enumeration,
inside the kernel** (`apple_dfr_cfgsel.ko`, modeled on r8152's cfgselector), and
bind the in-kernel pixel driver (`appletbdrm.ko` + T1 id) to the AV interface.
The userspace revert was caused by `apple_ibridge`'s probe forcing config 1
(`apple-ibridge.c:541` → `usb_driver_set_configuration(udev, 1)`), plus the
firmware self-resetting on live 1→2 switches; both paths are closed here.

All commands need root unless noted. Device path: `/sys/bus/usb/devices/1-3`.

## ✅ STATUS (2026-06-12): Phase 1 VALIDATED on real T1 hardware

The kernel path works: `apple_dfr_cfgsel` selects config 2 at enumeration and it **holds**, `appletbdrm` binds the AV interface, GINF succeeds, `/dev/dri/card0` appears, `[drm] Initialized appletbdrm 1.0.0`. Reproducible across a reboot (session-only insmod; not yet DKMS-persistent).

Two T1 adaptations were needed in `appletbdrm.c` to get there (now in the source):
- **RGB888** plane format + `drm_fb_xrgb8888_to_rgb888` (T1 panel byte order is R,G,B; T2 is BGR).
- **GINF read drains transient post-switch messages** + `usb_clear_halt` on both pipes in probe (T1 emits a readiness signal and a short ~52-byte status before the 65-byte GINF reply; without draining, probe failed `-EBADMSG`/`-74`).
- **16-byte frame-ack tolerance** in `flush_damage` (T1 acks a frame with a 16-byte header, not T2's 40-byte UDCL).

**Not yet validated:** an actual rendered frame on the panel — `modetest` is blocked because GNOME/mutter holds DRM master on `card0`. NEXT SESSION, in order:
1. **udev rule to release `card0` from the desktop seat** (so mutter doesn't grab master) — required before any renderer (modetest/tiny-dfr) can drive it from within a GNOME session. (Quick alt to just *see* it: `sudo chvt 3` to a bare VT, then modetest — no Touch Bar F-keys needed for `chvt`.)
2. **`tiny-dfr`** pointed at `card0` → real drawn function row.
3. **T1 touch shim**: the digitizer is a non-standard float32-X HID on interrupt EP `0x83` (range [0.5,1.0]); needs translating to input events (the userspace `dfr-switch.c buttons` stage prototypes this).

**Gotcha:** to restore the stock bar, load BOTH modules with `sudo modprobe -a apple_ibridge apple_touchbar` (separate args!) — `modprobe apple_ibridge apple_touchbar` treats the second as a param and silently doesn't load `apple_touchbar`, leaving the bar dead.

## Phase 1 — session-only smoke test (no DKMS, fast iteration)

```sh
cd ~/Code/xeeban/macbook-t1-linux/touch-bar/kernel/t1-touchbar-display
make                                   # no root needed; already verified to build

sudo modprobe -r apple_touchbar apple_ibridge   # remove the config-1 enforcer for this session
sudo insmod ./appletbdrm.ko            # display driver FIRST, so the AV interface binds instantly
sudo insmod ./apple_dfr_cfgsel.ko      # registration auto-reprobes 1-3 -> SET_CONFIGURATION(2)
```

Verify (give it ~5 s; one firmware self-reset/re-enumeration is OK and expected
on the first live switch — it re-enumerates straight into config 2):

```sh
cat /sys/bus/usb/devices/1-3/bConfigurationValue        # expect: 2, and it STAYS 2
sudo dmesg | grep -iE 'apple_dfr|appletbdrm|usb 1-3' | tail -30
basename $(readlink /sys/bus/usb/devices/1-3/driver)    # expect: apple_dfr_cfgsel
basename $(readlink /sys/bus/usb/devices/1-3:2.3/driver) # expect: appletbdrm (note :2.3 = config2, intf 3)
ls /dev/dri/                                            # a new cardN appears
grep -l . /sys/class/drm/card*/device/modalias 2>/dev/null | xargs grep -i 8600  # find the touchbar card
```

Push pixels (from a TTY or ssh; no compositor needed — the card is non-desktop):

```sh
modetest -M appletbdrm                       # note connector id + mode name (expect ~2008x60 or 2170x60)
modetest -M appletbdrm -s <CONN_ID>:<MODE>   # SMPTE color bars ON THE TOUCH BAR; Ctrl-C stops
```

Success criteria: bConfigurationValue holds at 2, dmesg shows appletbdrm GINF
succeeded (no "Failed to get display information"), color bars visible.

### Failure modes
- **Config flips back to 1**: something still calls set_configuration — check
  `lsmod | grep apple_ib` (must be empty in phase 1) and dmesg.
- **GINF fails / "unexpected bits per pixel"**: T1 protocol variance — capture
  full dmesg; we adapt the vendored appletbdrm.c (this is the main hardware unknown).
- **Reset loop** (device re-enumerates repeatedly): firmware rejects
  enumeration-time config 2 → falsifies hypothesis. Recover below, capture dmesg.

### Recover to stock (this session)
```sh
sudo rmmod apple_dfr_cfgsel appletbdrm
echo 0 | sudo tee /sys/bus/usb/devices/1-3/authorized
echo 1 | sudo tee /sys/bus/usb/devices/1-3/authorized   # forces clean reprobe -> generic driver -> config 1
sudo modprobe apple_ibridge apple_touchbar
sudo /usr/local/sbin/touchbar-relight-reload            # if the bar is dark
# worst case: reboot (acceptable on this machine)
```

## Phase 2 — patch apple-ib-drv so both stacks coexist

Must be done BEFORE making phase 3 persistent, or apple_ibridge will knock the
device back to config 1 once at every boot.

```sh
cd ~/Code/xeeban/macbook-t1-linux/touch-bar/kernel
sudo patch -d /usr/src/apple-ib-drv-r307.4afd309 -p1 < patches/apple-ibridge-no-config1-revert.patch
sudo dkms build apple-ib-drv/r307.4afd309 --force
sudo dkms install apple-ib-drv/r307.4afd309 --force
```

(After this, in config 2 the apple-ibridge-hid probe returns -ENODEV and
hid-generic takes the HID interfaces; in config 1 — e.g. with cfgsel removed —
apple_ibridge behaves exactly as before.)

## Phase 3 — persistent install (DKMS) + reboot test

```sh
cd ~/Code/xeeban/macbook-t1-linux/touch-bar/kernel
sudo cp -r t1-touchbar-display /usr/src/t1-touchbar-display-1.0
sudo dkms add t1-touchbar-display/1.0
sudo dkms install t1-touchbar-display/1.0

modinfo -F filename appletbdrm        # MUST print .../updates/dkms/appletbdrm.ko... (override of in-tree)
modinfo -F filename apple_dfr_cfgsel  # .../updates/dkms/...
sudo reboot
```

After reboot (both modules autoload via their `usb:v05ACp8600*` modaliases —
no modules-load.d needed):

```sh
cat /sys/bus/usb/devices/1-3/bConfigurationValue   # 2
ls /dev/dri/ ; modetest -M appletbdrm -s <CONN_ID>:<MODE>
# regression checks:
v4l2-ctl --list-devices 2>/dev/null | head          # camera still present (uvcvideo binds in config 2 too)
cat /proc/bus/input/devices | grep -A2 'Apple SPI'  # keyboard/touchpad unaffected (they are on applespi, not iBridge)
```

Then re-test hibernate (the 51- post-resume hook still reloads apple_ibridge;
it should now just decline in config 2 — confirm the bar relights via appletbdrm
re-probe instead).

## Phase 4 — full uninstall / recovery

```sh
sudo dkms remove t1-touchbar-display/1.0 --all
sudo rm -rf /usr/src/t1-touchbar-display-1.0
sudo patch -R -d /usr/src/apple-ib-drv-r307.4afd309 -p1 \
    < ~/Code/xeeban/macbook-t1-linux/touch-bar/kernel/patches/apple-ibridge-no-config1-revert.patch
sudo dkms build apple-ib-drv/r307.4afd309 --force
sudo dkms install apple-ib-drv/r307.4afd309 --force
sudo reboot                                        # back to stock simple-mode Touch Bar
```

## Knobs
- `apple_dfr_cfgsel.display_config=0` (module param) disables forcing without
  unloading; `=N` forces a specific bConfigurationValue; `-1` (default)
  auto-detects the config containing an Audio/Video class interface.

## Known tradeoffs / open hardware questions
1. In display mode there is no firmware ESC/F-key strip — the bar shows only
   what DRM clients draw. (Physical keyboard is SPI and unaffected.)
2. T1 GINF reply (resolution/bpp/pixel-format) not yet observed live — appletbdrm
   validates and will refuse cleanly if it differs.
3. Touch input in display mode arrives via the config-2 HID interfaces under
   hid-generic — mapping is future work.
4. cdc_ncm may create a network interface for the iBridge CDC function in
   config 2 — harmless; blacklist later if noisy.
