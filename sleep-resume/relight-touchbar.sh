#!/bin/bash
# Manually relight the Touch Bar when it comes back DARK after a hibernate resume.
#
# Why this exists: the system-sleep hook no longer touches the Touch Bar on
# hibernate (its modprobe -r can deadlock appletb_remove in D-state and force a
# hard reboot if it races the freeze). Skipping it keeps hibernate safe but leaves
# the bar dark on resume. This script does the proven relight sequence ON DEMAND,
# AFTER the system is fully resumed -- so it never races a freeze and cannot cause
# the hard-reboot wedge. Run it whenever the bar is dark.
#
# Sequence (mirrors the old hook's working 'post' block):
#   unload touchbar+ibridge -> USB re-enumerate the iBridge (05ac:8600) -> reload.
# NOTE: the unload (modprobe -r apple_touchbar) is the step that *can* hang in
# rare cases. Run manually it's a contained event (the system stays up); if it
# ever wedges, a normal `reboot` clears it -- you do NOT need a hard power-off.
set -u

log() { echo "relight-touchbar: $*"; }

if [ "$(id -u)" -ne 0 ]; then
    echo "run as root:  sudo $0" >&2
    exit 1
fi

find_ibridge() {
    local d
    for d in /sys/bus/usb/devices/*/; do
        [ -f "$d/idVendor" ] || continue
        if [ "$(cat "$d/idVendor" 2>/dev/null)" = "05ac" ] \
           && [ "$(cat "$d/idProduct" 2>/dev/null)" = "8600" ]; then
            printf '%s' "$d"; return 0
        fi
    done
    return 1
}

log "unloading apple_touchbar + apple_ibridge"
modprobe -r apple_touchbar 2>/dev/null || log "warn: could not remove apple_touchbar"
modprobe -r apple_ibridge  2>/dev/null || log "warn: could not remove apple_ibridge"

if dev="$(find_ibridge)"; then
    log "USB reset ${dev} (deauthorize -> reauthorize)"
    echo 0 > "${dev}authorized" 2>/dev/null || log "warn: deauthorize failed"
    sleep 2
    echo 1 > "${dev}authorized" 2>/dev/null || log "warn: reauthorize failed"
    sleep 3
else
    log "warn: iBridge USB device (05ac:8600) not found; skipping USB reset"
fi

log "reloading apple_ibridge then apple_touchbar"
modprobe apple_ibridge  2>/dev/null || log "warn: could not load apple_ibridge"
sleep 1
modprobe apple_touchbar 2>/dev/null || log "warn: could not load apple_touchbar"

echo
log "done — check the Touch Bar now."
lsmod | grep -iE 'apple_touchbar|apple_ibridge' || log "WARN: modules not loaded"
