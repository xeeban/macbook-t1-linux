# T1 Touch Bar — persistent install (survives reboot, zero manual steps)

This packages the proven custom Touch Bar stack so it comes up automatically on
every boot and after hibernate resume. Everything here is **packaging only** —
the daemons (`dfr-render` / `dfr-touchd` / `dfr-fnd`) and kernel modules are
unchanged from the working session-mode stack.

- Hardware: MacBookPro13,2 (T1), Arch, kernel 7.0.10-arch1-1, GNOME/Wayland,
  user `nnishigaya` (uid 1000). iBridge USB device: `/sys/bus/usb/devices/1-3`
  (05ac:8600).
- Session-mode bring-up + the live hardware tests live in
  `../DFRD-RUNBOOK.md` and `../../kernel/RUNBOOK.md`. Do those FIRST; this file
  assumes the stack already works when loaded by hand.

## What gets installed

| Artifact | Destination | Purpose |
|---|---|---|
| `t1-touchbar-display` DKMS (`appletbdrm` + `apple_dfr_cfgsel`) | `/usr/src/t1-touchbar-display-1.0`, `updates/dkms` | config-2 selector + DRM driver, autoloaded by `usb:v05ACp8600*` modalias; overrides in-tree `appletbdrm` |
| apple-ibridge patch | rebuilt `apple-ib-drv/r307.4afd309` DKMS | stops apple_ibridge forcing config 1 once per boot |
| `99-touchbar-dfr.rules` | `/etc/udev/rules.d/` | frees the card from mutter (seat) + `SYMLINK=/dev/dri/touchbar` + `SYSTEMD_WANTS=dfrd.service` |
| `dfr-render`, `dfr-touchd`, `dfr-fnd`, `dfrd-run.sh` | `/usr/local/bin/` | the userspace stack (run together by `dfrd-run.sh`) |
| `dfrd.service` | `/etc/systemd/system/` | one unit, bound to the DRM card, `Restart=always`, layout `media` |
| `51-touchbar-relight-hibernate.sh` (config-aware) | `/usr/lib/systemd/system-sleep/` | no-ops the apple_ibridge reload in config 2; nudges dfrd instead |

## How start-up / binding works (the design)

**One unit, `dfrd.service`, running `dfrd-run.sh media`.** Chosen over the two
draft per-daemon units because `dfrd-run.sh` already orchestrates all three
daemons (including `dfr-fnd`, which the drafts omit) in a single process group
with clean teardown and correct signal wiring. One unit = one lifecycle = no
inter-daemon ordering races.

**The card drives the unit, via udev → systemd:**
- The udev rule `SYMLINK+="dri/touchbar"` gives the appletbdrm card a stable
  name regardless of its card number, and `TAG+="systemd"` makes systemd
  materialize it as `dev-dri-touchbar.device`.
- `ENV{SYSTEMD_WANTS}+="dfrd.service"` on the same rule line **pulls in
  dfrd.service every time the card is added/changed** — at boot (DKMS modalias
  autoload) and after any re-enumeration (hibernate resume, manual module
  reload). This is the primary start path; `enable` + `WantedBy=multi-user`
  is a redundant fallback.
- `BindsTo=dev-dri-touchbar.device` + `After=` in the unit means: the daemons
  **stop the instant the card vanishes** (suspend teardown, rmmod) instead of
  spinning on a dead card, and start only once it exists.
- `Restart=always` + `RestartSec=2` + `StartLimitIntervalSec=0` covers crashes
  and the resume re-probe window without systemd ever giving up.
- `ExecStartPre=/usr/bin/sleep 1` absorbs the lag between the AV interface
  (DRM card) and the HID interfaces (digitizer hidraw) probing, so
  `dfr-touchd` doesn't lose the race on a cold start.

**The seat rule makes the boot race irrelevant.** GDM/mutter starts roughly
when the modules autoload. Whichever wins, mutter never claims a card whose
`ID_SEAT != seat0`, so DRM master stays free for `dfr-render`. dfrd starts even
before any user login (render + touch work at the GDM screen); the
settings-launch path in `dfr-touchd` simply no-ops until a session exists.

**Hibernate.** On resume the iBridge re-enumerates → `apple_dfr_cfgsel`
re-selects config 2 → `appletbdrm` re-probes → card re-add → `SYSTEMD_WANTS`
restarts dfrd. The config-aware `51-` hook detects config 2 and does NOT run the
old apple_ibridge reload (which, with the patch, would just decline in config 2
and only risks racing the re-probe); it instead issues a cheap
`systemctl restart dfrd.service` as a belt-and-suspenders repaint. In config 1
(stack uninstalled) the same hook falls back to the original stock-bar reload.

---

## Install — exact ordered commands

```sh
# 0. Build the binaries fresh (no root).
cd ~/Code/xeeban/macbook-t1-linux/touch-bar/dfrd
make

# 1. Run the installer (root). Idempotent; re-runnable.
sudo ~/Code/xeeban/macbook-t1-linux/touch-bar/dfrd/install/install.sh

# 2. Reboot to activate.
sudo reboot
```

That is the whole install. The script: DKMS-installs the display modules and
verifies `appletbdrm` overrides the in-tree one; applies the apple-ibridge
patch (idempotently) and rebuilds that DKMS; installs the persistent udev rule;
copies the four userspace files to `/usr/local/bin`; installs + enables
`dfrd.service`; swaps in the config-aware hibernate hook (backing up the
original). It does **not** load anything live — it stages for the next boot.

---

## Verify after reboot

```sh
# 1. Device is in display config 2 (and stays).
cat /sys/bus/usb/devices/1-3/bConfigurationValue        # expect: 2

# 2. DKMS override is in effect (NOT the in-tree appletbdrm).
modinfo -F filename appletbdrm                          # expect: .../updates/dkms/appletbdrm.ko...
dkms status t1-touchbar-display                          # installed

# 3. The card exists, is symlinked, and escaped the desktop seat.
ls -l /dev/dri/touchbar                                  # symlink -> ../dri/cardN
udevadm info /dev/dri/touchbar | grep ID_SEAT           # expect: ID_SEAT=seat-touchbar
for c in /sys/class/drm/card[0-9]*; do \
  echo "$c -> $(basename $(readlink -f $c/device/driver))"; done   # one -> appletbdrm

# 4. The service is up.
systemctl status dfrd.service                            # active (running)
systemctl status dev-dri-touchbar.device                 # active (plugged)

# 5. The bar itself.
#    - LOOK: 'media' control strip is drawn (brightness/volume/etc.).
#    - HOLD Fn: row switches to F1–F12 (momentary; release returns to media).
#    - TAP a key in a focused text field: real key event lands.
#    - HOLD Ctrl+Fn, tap the battery button: gnome-control-center opens the
#      power panel owned by your user (only works once logged in):
ps -o user,cmd -C gnome-control-center                   # user = nnishigaya

# 6. Regressions (should be unaffected):
v4l2-ctl --list-devices 2>/dev/null | head              # FaceTime camera still present
cat /proc/bus/input/devices | grep -A2 'Apple SPI'      # keyboard/touchpad fine
```

### If something's off after reboot
- **config != 2 / flips to 1:** apple-ibridge patch didn't take. Check
  `grep -n usb_driver_set_configuration /usr/src/apple-ib-drv-r307.4afd309/apple-ibridge.c`
  (should be gone) and `dkms status apple-ib-drv`; re-run `install.sh`.
- **`appletbdrm` resolves to the in-tree `.ko.zst`, not updates/dkms:**
  `sudo depmod -a` then re-check; ensure `dkms status t1-touchbar-display`
  says installed.
- **`/dev/dri/touchbar` missing:** udev rule not applied at card-add. Confirm
  `/etc/udev/rules.d/99-touchbar-dfr.rules` has the `SYMLINK`/`SYSTEMD_WANTS`
  line; `udevadm test $(udevadm info -q path -n /dev/dri/cardN) 2>&1 | tail`.
- **mutter grabbed the card (renderer EACCES / dfrd flapping):** check
  `udevadm info /dev/dri/touchbar | grep ID_SEAT`. If not `seat-touchbar`, the
  rule lost the boot race for THIS card — log out/in once (mutter re-enumerates
  and now honors the seat); worst case `sudo systemctl restart dfrd.service`
  after relogin.
- **dfrd active but bar dark:** `journalctl -u dfrd.service -b` — look for the
  `dfr-render` startup line (`mode set; layout 'media'`) vs a DRM-master denial.

---

## Uninstall — back to the stock firmware Touch Bar

```sh
sudo ~/Code/xeeban/macbook-t1-linux/touch-bar/dfrd/install/uninstall.sh
sudo reboot
```

Reverses everything: stops/removes `dfrd.service`, deletes the binaries and
udev rule, `dkms remove`s the display modules + `depmod -a`, reverses the
apple-ibridge patch and rebuilds its DKMS (so it forces config 1 again), and
restores the original hibernate hook from the `.pre-dfrd` backup. After reboot
the stock ESC/F-key/media bar returns via apple_ibridge + apple_touchbar.

---

## Ranked: what needs live reboot verification (top risk first)

1. **apple-ibridge patch survives a real boot and the device holds config 2.**
   HIGHEST. The current working session only avoids the config-1 knockback
   because apple_ibridge is unloaded right now. On a fresh boot apple_ibridge
   loads early; the patched probe must return `-ENODEV` in config 2 instead of
   forcing config 1. If the DKMS rebuild didn't take (wrong tree, depmod stale),
   the bar reverts to stock ~once per boot. **Check #1 first:**
   `bConfigurationValue == 2` and it stays.

2. **DKMS `appletbdrm` actually overrides the in-tree module at boot.** HIGH.
   `modinfo` today still points at the in-tree `.ko.zst`; the override only
   exists after `dkms install` + depmod. If `updates/dkms` doesn't win the
   depmod search at boot, the in-tree appletbdrm (no T1 id) loads and the card
   never appears. Verify `modinfo -F filename appletbdrm` post-reboot.

3. **The udev → systemd handoff fires at boot.** HIGH. `SYMLINK=/dev/dri/touchbar`
   + `TAG+="systemd"` + `SYSTEMD_WANTS` is the entire auto-start mechanism and
   has never run at boot (session mode started dfrd by hand). Risk: rule
   doesn't match at card-add, or `dev-dri-touchbar.device` never materializes,
   so dfrd never starts. Verify `systemctl status dev-dri-touchbar.device` and
   `dfrd.service` are both active.

4. **Seat rule wins the boot race vs mutter for the appletbdrm card.** MEDIUM-HIGH.
   Proven in session mode on a hotplug (manual reload), but boot is a tighter
   race — GDM and module autoload start together. If mutter opens the card
   before the rule tags the seat, it holds DRM master and dfr-render gets
   EACCES. Fallback is a single logout/login. Verify `ID_SEAT=seat-touchbar`.

5. **dfr-touchd vs hidraw-node timing on cold start.** MEDIUM. The HID
   interfaces probe a beat after the AV interface; `ExecStartPre sleep 1` +
   `Restart=always` should cover it, but if the digitizer node lags >1s,
   dfr-touchd may exit once and restart. Cosmetic (touch works after the
   restart) but verify no restart loop in `journalctl -u dfrd.service -b`.

6. **Hibernate resume re-probe → dfrd restart.** MEDIUM. The card-disappears /
   reappears path (`BindsTo` stop + `SYSTEMD_WANTS` re-add) is untested across
   a real S4 cycle with this packaging. Risk: the card re-enumerates as a
   different cardN but the SYMLINK should follow it; or it doesn't re-enumerate
   and the config-aware hook's `systemctl restart dfrd` repaints onto the
   surviving card. Test: hibernate, resume, confirm the bar relights and
   `systemctl status dfrd` is active without manual help.

7. **Boot before login (GDM screen) render+touch.** LOW-MEDIUM. dfrd starts at
   `multi-user`, before any user session. Render + touch should work; the
   settings-launch no-ops until login. Verify the bar is lit at the GDM
   password screen and the settings-launch starts working after you log in.

8. **DKMS auto-rebuild on the next kernel update.** LOW (deferred). Both DKMS
   modules `AUTOINSTALL=yes`, so a kernel bump should rebuild them — but the
   apple-ibridge patch lives in `/usr/src` and persists, while a kernel update
   that also bumps the in-tree appletbdrm could change the override calculus.
   Re-verify checks #1–#2 after the first post-install kernel update.
