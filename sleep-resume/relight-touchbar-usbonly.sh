#!/bin/bash
# EXPERIMENT: relight the Touch Bar using ONLY a USB re-enumeration of the iBridge
# (05ac:8600) -- NO modprobe -r, NO module reload. The apple_touchbar/apple_ibridge
# drivers stay loaded; deauthorize->reauthorize forces the USB device to disconnect
# and reconnect, and the kernel auto-rebinds the loaded drivers on reconnect.
#
# Why try this: the only step that can deadlock (appletb_remove via `modprobe -r`,
# unkillable D-state) is GONE here. If this lights the bar, it's safe to wire as an
# automatic post-hibernate hook with no hard-reboot risk at all.
#
# Caveat: the USB reconnect still drives the HID disconnect/reprobe path internally,
# so it's not provably deadlock-free -- but it avoids the explicit module unload that
# actually wedged on 2026-06-11. If the bar stays dark, fall back to ./relight-touchbar.sh
set -u

log() { echo "relight-usbonly: $*"; }

if [ "$(id -u)" -ne 0 ]; then
    echo "run as root:  sudo $0" >&2
    exit 1
fi

log "modules before:"; lsmod | grep -iE 'apple_touchbar|apple_ibridge' || log "(touchbar/ibridge not loaded — usb-only likely won't help; use ./relight-touchbar.sh)"

dev=""
for d in /sys/bus/usb/devices/*/; do
    [ -f "$d/idVendor" ] || continue
    if [ "$(cat "$d/idVendor" 2>/dev/null)" = "05ac" ] \
       && [ "$(cat "$d/idProduct" 2>/dev/null)" = "8600" ]; then
        dev="$d"; break
    fi
done

if [ -z "$dev" ]; then
    log "iBridge USB device (05ac:8600) not found — cannot USB-reset"; exit 1
fi

log "USB reset ${dev} (deauthorize -> reauthorize), no modprobe"
echo 0 > "${dev}authorized" 2>/dev/null || log "warn: deauthorize failed"
sleep 2
echo 1 > "${dev}authorized" 2>/dev/null || log "warn: reauthorize failed"
sleep 3

echo
log "done — LOOK AT THE TOUCH BAR. modules after:"
lsmod | grep -iE 'apple_touchbar|apple_ibridge' || log "(modules not loaded)"
log "If the bar is LIT: this is the safe path, tell Claude to wire it as a post-hook."
log "If still DARK: run ./relight-touchbar.sh (the full reload) instead."
