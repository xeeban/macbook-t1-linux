#!/bin/bash
# Installed to /usr/local/sbin/touchbar-relight-reload.
# Post-hibernate Touch Bar relight: FULL RELOAD of the apple_ibridge stack. This destroys and
# re-creates the virtual HIDs and runs a fresh apple_touchbar probe = the cold-boot light-up path,
# which is the ONLY thing that relights the post-hibernate Touch Bar (a USB re-enumerate / bus reset
# reuse the stale apple_ibridge instance and stay dark; a successful set_tb_disp alone stays dark).
# SAFE only because the appleib_add_device heap-OOB is fixed (the reload's teardown no longer GPFs/
# D-state-deadlocks) -- verified 2026-06-12. Invoked ~5s post-resume by a DETACHED, time-bounded
# transient unit (see 51-touchbar-relight-hibernate.sh) so it can never block or wedge the resume path.
set -u
log() { echo "tb-relight-reload: $*"; }
ss() { awk '{print $3}' /proc/self/stat 2>/dev/null; }

log "reload start (self=$(ss))"
modprobe -r apple_touchbar 2>/dev/null || log "warn: -r apple_touchbar (self=$(ss))"
modprobe -r apple_ibridge  2>/dev/null || log "warn: -r apple_ibridge (self=$(ss))"
sleep 1
modprobe apple_ibridge 2>/dev/null || log "warn: load apple_ibridge"
sleep 2
modprobe apple_touchbar 2>/dev/null || log "warn: load apple_touchbar"
sleep 1
log "reload done; apple-touchbar bound: $(ls /sys/bus/hid/drivers/apple-touchbar/ 2>/dev/null | grep -cE '0003:1D6B:0301') sub-HID(s)"
exit 0
