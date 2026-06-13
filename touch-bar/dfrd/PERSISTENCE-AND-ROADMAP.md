# dfrd — persistence plan + app-aware roadmap

Status: session-mode stack built and compiling (2026-06-12). This file covers
how to make it survive reboots, and the forward plan for per-app layouts.

## 1. Making it persistent

Order matters — each step depends on the previous one being solid:

### 1a. Kernel modules via DKMS (prerequisite, already planned)
Follow `../kernel/RUNBOOK.md` **Phase 2** (patch `apple-ib-drv` so
`apple_ibridge` stops forcing config 1) **then Phase 3** (DKMS install of
`t1-touchbar-display`). After that the appletbdrm card exists from boot via
the `usb:v05ACp8600*` modalias autoload — no insmod sessions.

### 1b. udev rule (already required for session mode)
`/etc/udev/rules.d/99-touchbar-dfr.rules` — must be present before the card
appears at boot; with DKMS autoload this is automatic (rules are installed
before root pivot... rule lives in /etc, applied when the uevent fires).

### 1c. Binaries + systemd units
```sh
sudo install -m0755 dfr-render dfr-touchd /usr/local/bin/
sudo install -m0644 systemd/dfr-render.service systemd/dfr-touchd.service /etc/systemd/system/
```

The draft units bind to `dev-dri-touchbar.device`, which does NOT exist yet.
Create it by EXTENDING the udev rule's DRM-card line with:

```
SYMLINK+="dri/touchbar", TAG+="systemd", ENV{SYSTEMD_WANTS}+="dfr-render.service dfr-touchd.service"
```

- `SYMLINK` gives a stable `/dev/dri/touchbar` name (card number varies);
  systemd then exposes it as `dev-dri-touchbar.device`.
- `SYSTEMD_WANTS` starts both services whenever the card (re)appears —
  including after the post-hibernate re-probe, which kills two birds: no
  separate resume hook needed for the renderer. (The existing 51- hibernate
  hook reloads the apple_ibridge stack; after the Phase-2 patch apple-ibridge
  declines in config 2 and appletbdrm re-probes → card re-add → units restart.)
- `BindsTo=` in the units stops the daemons when the card vanishes
  (suspend teardown, module unload) instead of leaving them spinning.
- No `[Install]`/`enable` needed once SYSTEMD_WANTS drives them; the
  `WantedBy=multi-user.target` lines are a belt-and-suspenders fallback and
  can be dropped.

Open question to validate live: whether dfr-touchd starting in parallel with
the card-add races hidraw node creation (the HID interfaces probe slightly
after the AV interface). `Restart=on-failure` + `RestartSec=2` papers over it;
if it flaps, add `ExecStartPre=/usr/bin/sleep 1` or a tiny udev-settle wait.

### 1d. Boot-order trap to keep in mind
GDM/mutter starts ~simultaneously with module autoload at boot. The seat rule
is what makes that race irrelevant: whichever side wins, mutter never claims a
card whose ID_SEAT isn't seat0. Verify once after the first persistent boot:
`udevadm info /dev/dri/touchbar | grep ID_SEAT` → `seat-touchbar`.

## 2. App-aware layer (per-app layouts) — forward plan

Goal: Obsidian/editor/terminal focused → different Touch Bar layout.

Architecture (deliberately dumb v1):

1. **Focus source**: GNOME Wayland exposes no external focus API by design.
   Install the **focused-window-dbus** GNOME Shell extension (flexagoon,
   extensions.gnome.org ID **5592**) → exports the focused window's
   `wm_class` on D-Bus: `org.gnome.shell.extensions.FocusedWindow.Get`.
2. **`dfr-appwatchd`** (small user-session daemon, python or C + sd-bus):
   subscribes/polls the extension, maps `wm_class` → layout name via a config
   table (e.g. `obsidian: media`, `kitty: fn`, default: `fn`).
3. **Control channel**: today both daemons cycle layouts on SIGUSR1 — fine
   for two layouts, wrong for targeting a specific one. v2: replace SIGUSR1
   with a tiny control socket (`/run/dfrd.sock`, line protocol
   `layout <name>\n`) served by BOTH daemons, or better:
4. **Merge** dfr-render + dfr-touchd into one `dfrd` process (they already
   share dfr-layout.h; one process kills the layout-desync risk entirely,
   single socket, and enables drawn touch feedback: highlight the pressed
   button — the renderer already keeps the fb mapped, just repaint the zone
   on touch-down and DirtyFB).
   The two-binary split exists because it made the first hardware bring-up
   independently testable; merging is the natural v2.

Also on the v2 list:
- per-key colors/icons in `dfr-layout.h` (data already structured for it)
- key auto-repeat (uinput EV_REP) for volume/brightness hold
- multi-touch: the 52-byte report likely carries more than X — capture
  `dfr-touchd -v -n` dumps with two fingers and stare at bytes 4..51
- long-axis touch calibration: right edge reads ~0.997 not 1.000; if the last
  key feels dead at the very edge, stretch the map: `nx = (x-0.5)/(0.997-0.5)`.

## 3. Layout-sync contract (current, signal-based)

Both daemons compile in the SAME `dfr-layout.h` and start with the same `-l`.
SIGUSR1 advances each to its next layout in the same fixed order. To swap:
`sudo kill -USR1 <render-pid> <touchd-pid>` (dfrd-run.sh prints both pids; it
runs them in one process group, so `kill -USR1 -- -<pgid>` also works). If one
signal is lost the zones and labels desync — acceptable for manual use, the
reason v2 wants the socket.
