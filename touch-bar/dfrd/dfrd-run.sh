#!/usr/bin/env bash
# dfrd-run.sh — start the T1 Touch Bar userspace stack (renderer + touch daemon)
# and tear it down cleanly on exit.
#
# PRECONDITIONS (this script checks them; see DFRD-RUNBOOK.md to set them up):
#   - appletbdrm.ko + apple_dfr_cfgsel.ko loaded, device holding config 2
#   - 99-touchbar-dfr.rules installed BEFORE the modules were loaded
#   - run as root (DRM master + /dev/hidraw + /dev/uinput)
#
# Usage: sudo ./dfrd-run.sh [layout] [extra dfr-render flags...]
#   e.g. sudo ./dfrd-run.sh fn
#        sudo ./dfrd-run.sh fn --flip-short      # if labels are mirrored
# Layout swap while running:  sudo kill -USR1 -- -<pgid-of-this-script>
#   (both daemons cycle layouts on SIGUSR1; this script puts them in one
#    process group so one signal reaches both)

set -u
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LAYOUT="${1:-fn}"
shift 2>/dev/null || true

if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: must run as root (sudo $0 $LAYOUT)" >&2
    exit 1
fi

# --- precondition checks -------------------------------------------------
cfg="$(cat /sys/bus/usb/devices/1-3/bConfigurationValue 2>/dev/null || echo '?')"
if [ "$cfg" != "2" ]; then
    echo "ERROR: iBridge is in config $cfg, not 2 — load the display stack first:" >&2
    echo "  cd ~/Code/xeeban/macbook-t1-linux/touch-bar/kernel/t1-touchbar-display" >&2
    echo "  sudo modprobe -r apple_touchbar apple_ibridge" >&2
    echo "  sudo insmod ./appletbdrm.ko && sudo insmod ./apple_dfr_cfgsel.ko" >&2
    exit 1
fi

card=""
for c in /sys/class/drm/card[0-9]*; do
    drv="$(basename "$(readlink -f "$c/device/driver" 2>/dev/null)" 2>/dev/null)"
    if [ "$drv" = "appletbdrm" ]; then card="/dev/dri/$(basename "$c")"; break; fi
done
if [ -z "$card" ]; then
    echo "ERROR: no appletbdrm DRM card found (modules loaded but probe failed? check dmesg)" >&2
    exit 1
fi
seat="$(udevadm info "$card" 2>/dev/null | sed -n 's/^E: ID_SEAT=//p')"
if [ "$seat" != "seat-touchbar" ]; then
    echo "WARNING: $card ID_SEAT='${seat:-seat0(default)}' — the seat rule did not apply." >&2
    echo "  mutter may hold DRM master and the renderer will fail. Fix:" >&2
    echo "  sudo install -m0644 $DIR/99-touchbar-dfr.rules /etc/udev/rules.d/" >&2
    echo "  sudo udevadm control --reload   # then RELOAD the kernel modules" >&2
    echo "  (continuing anyway — works if you're on a bare VT)" >&2
fi

# --- launch --------------------------------------------------------------
RPID="" TPID=""
cleanup() {
    trap - EXIT INT TERM
    [ -n "$TPID" ] && kill "$TPID" 2>/dev/null
    [ -n "$RPID" ] && kill "$RPID" 2>/dev/null
    wait 2>/dev/null
    echo "dfrd: stopped."
}
trap cleanup EXIT INT TERM

echo "dfrd: starting renderer ($card, layout '$LAYOUT')..."
"$DIR/dfr-render" -l "$LAYOUT" "$@" &
RPID=$!
sleep 1
if ! kill -0 "$RPID" 2>/dev/null; then
    echo "ERROR: dfr-render exited at startup (DRM master denied? see output above)" >&2
    RPID=""
    exit 1
fi

echo "dfrd: starting touch daemon (layout '$LAYOUT')..."
"$DIR/dfr-touchd" -l "$LAYOUT" &
TPID=$!
sleep 1
if ! kill -0 "$TPID" 2>/dev/null; then
    echo "ERROR: dfr-touchd exited at startup (no digitizer hidraw? see output above)" >&2
    TPID=""
    exit 1
fi

echo "dfrd: up. renderer pid $RPID, touchd pid $TPID. Ctrl-C to stop."
echo "dfrd: cycle layouts with: sudo kill -USR1 $RPID $TPID"
wait -n
echo "dfrd: a daemon exited — tearing down." >&2
