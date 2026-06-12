#!/bin/bash
# Escalation test: issue a real USB BUS RESET (USBDEVFS_RESET ioctl = what usb_reset_device()
# does in-kernel) on the iBridge, instead of the logical `authorized` 0->1 toggle. A bus reset
# is a harder re-init than re-enumeration; testing whether it un-latches the post-hibernate
# Touch Bar display. SAFE now (the appleib teardown OOB is fixed). Run as root. WATCH THE BAR.
set -u
log() { echo "usb-reset: $*"; }
[ "$(id -u)" -eq 0 ] || { echo "run as root: sudo $0"; exit 1; }

# locate iBridge 05ac:8600
D=""
for d in /sys/bus/usb/devices/[0-9]*; do
    [ -f "$d/idVendor" ] || continue
    if [ "$(cat "$d/idVendor" 2>/dev/null)" = "05ac" ] \
       && [ "$(cat "$d/idProduct" 2>/dev/null)" = "8600" ]; then
        D="$d"; break
    fi
done
[ -n "$D" ] || { log "iBridge 05ac:8600 not found"; exit 1; }
BUS=$(cat "$D/busnum"); DEV=$(cat "$D/devnum")
NODE=$(printf '/dev/bus/usb/%03d/%03d' "$BUS" "$DEV")
log "iBridge $D -> $NODE (authorized=$(cat "$D/authorized" 2>/dev/null))"

SINCE="$(date '+%Y-%m-%d %H:%M:%S')"
echo
log ">>> WATCH THE TOUCH BAR through the reset (a few seconds) <<<"
echo

python3 - "$NODE" <<'PY'
import fcntl, os, sys
USBDEVFS_RESET = 0x5514  # _IO('U', 20)
node = sys.argv[1]
fd = os.open(node, os.O_WRONLY)
try:
    fcntl.ioctl(fd, USBDEVFS_RESET, 0)
    print("usb-reset: USBDEVFS_RESET issued on", node)
finally:
    os.close(fd)
PY
rc=$?
[ $rc -eq 0 ] || log "warn: ioctl returned $rc"
sleep 5

echo
echo "=== crash markers since reset (want NONE) ==="
journalctl -k -b --since "$SINCE" 2>/dev/null | grep -E 'list_del|general protection|protection fault|Oops|BUG:|use-after-free' || echo "CLEAN — no crash markers."
echo
echo "=== touchbar re-probe + any display error after reset ==="
journalctl -k -b --since "$SINCE" 2>/dev/null | grep -iE 'apple-touchbar|tb:|Touchbar|hw open failed|usb .*reset|1D6B:0301' | tail -12
echo
echo "=== D-state (want none) ==="; ps -eo state,pid,comm | awk '$1=="D"{print}'; echo "(nothing above = good)"
echo "=== state now ==="; log "authorized=$(cat "$D/authorized" 2>/dev/null); apple-touchbar bound: $(ls /sys/bus/hid/drivers/apple-touchbar/ 2>/dev/null | grep -cE '0003:1D6B:0301') sub-HID(s)"
echo
echo ">>> Tell Claude: did the bar relight after the USB reset? <<<"
