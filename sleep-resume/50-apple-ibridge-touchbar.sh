#!/bin/bash
# 2026-06-05: system-sleep hook for the out-of-tree apple_ibridge / apple_touchbar
# DKMS stack on this MacBookPro13,2 (writers-deck).
#
# Background: apple_ibridge's USB suspend callback (appleib_hid_suspend) has a
# history of walking a stale/freed sub_hdev across the s2idle suspend/resume
# cycle (use-after-free), wedging HID input on resume and/or leaving the Touch
# Bar dark. The driver source was hardened (appleib_forward_int_op guards
# hdev_info + snapshots ->driver; sub_hdevs[] slots NULL'd on destroy) and
# runtime autosuspend is pinned off via 99-apple-ibridge-no-autosuspend.rules
# -- but the SYSTEM s2idle path still enters these drivers' suspend callbacks.
#
# Strategy:
#   pre  : unload apple_touchbar + apple_ibridge so the buggy suspend/resume
#          callbacks never run against live state.
#   post : reset the iBridge USB device and reload the stack fresh.
#
# Why the USB reset on resume: a plain modprobe -r / modprobe cycle does NOT
# power-cycle the USB device -- verified 2026-06-05, the touchbar firmware
# endpoint stays stuck and apple-touchbar logs "tb: hw open failed (-19)"
# (ENODEV), leaving the bar dark. Deauthorizing then reauthorizing the iBridge
# (05ac:8600) forces a clean re-enumeration; only after that does the reload
# bind cleanly and light the bar. This mirrors the manual recovery that worked.
#
# Scope: only apple_ibridge + apple_touchbar are touched. applespi
# (keyboard/trackpad) and applesmc are left loaded -- not implicated, and the
# internal keyboard must keep working. apple_touchbar binds to a virtual HID
# created by apple_ibridge, so unload touchbar first / reload ibridge first.
# Module params (fnmode=2 idle_timeout=-1 dim_timeout=-1) come from modprobe.d
# and are reapplied automatically by plain `modprobe`.
#
# systemd-sleep runs this as root with: $1 = pre|post, $2 = suspend|hibernate|...
# stdout/stderr are captured into the journal (systemd-suspend.service).

set -u

case "$2" in
    suspend|hibernate|suspend-then-hibernate|hybrid-sleep) ;;
    *) exit 0 ;;
esac

log() { echo "apple-ibridge-sleep-hook: $*"; }

# Locate the iBridge USB device (05ac:8600) sysfs dir by VID/PID, not bus path.
find_ibridge() {
    local d
    for d in /sys/bus/usb/devices/*/; do
        [ -f "$d/idVendor" ] || continue
        if [ "$(cat "$d/idVendor" 2>/dev/null)" = "05ac" ] \
           && [ "$(cat "$d/idProduct" 2>/dev/null)" = "8600" ]; then
            printf '%s' "$d"
            return 0
        fi
    done
    return 1
}

# Force a clean USB re-enumeration of the iBridge (clears the stuck endpoint).
usb_reset_ibridge() {
    local dev
    if ! dev="$(find_ibridge)"; then
        log "warn: iBridge USB device (05ac:8600) not found; skipping USB reset"
        return 1
    fi
    log "USB reset ${dev} (deauthorize -> reauthorize)"
    echo 0 > "${dev}authorized" 2>/dev/null || log "warn: deauthorize failed"
    sleep 2
    echo 1 > "${dev}authorized" 2>/dev/null || log "warn: reauthorize failed"
    sleep 3
}

case "$1" in
    pre)
        log "unloading apple_touchbar + apple_ibridge before $2"
        modprobe -r apple_touchbar 2>/dev/null || log "warn: could not remove apple_touchbar"
        modprobe -r apple_ibridge  2>/dev/null || log "warn: could not remove apple_ibridge"
        ;;
    post)
        log "post-$2: clearing, USB-resetting, and reloading the Touch Bar stack"
        # Clear anything udev may have auto-loaded on bus resume (possibly stuck).
        modprobe -r apple_touchbar 2>/dev/null || true
        modprobe -r apple_ibridge  2>/dev/null || true
        usb_reset_ibridge
        modprobe apple_ibridge  2>/dev/null || log "warn: could not load apple_ibridge"
        sleep 1
        modprobe apple_touchbar 2>/dev/null || log "warn: could not load apple_touchbar"
        ;;
esac

exit 0
