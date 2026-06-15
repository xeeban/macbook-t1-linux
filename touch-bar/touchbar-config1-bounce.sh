#!/bin/bash
# touchbar-config1-bounce.sh — post-sleep relight for the CUSTOM (config-2 /
# appletbdrm) Touch Bar.
#
# WHY THIS EXISTS:
#   The T1 panel backlight/power is a config-1 `apple_touchbar` HID `disp`
#   function. The config-2 bulk protocol (appletbdrm) has NO panel-power
#   command — proven 2026-06-15: the config-2 DISP Feature report (id 2 on
#   /dev/hidraw1) is inert for panel power (writing disp=ON, and an OFF->ON
#   edge, both did nothing post-sleep). After any sleep the panel power drops
#   and nothing in config-2 turns it back on, so the custom bar stays dark
#   until reboot.
#
# WHAT THIS DOES (the "config-1 bounce"):
#   1. stop dfrd (it owns the config-2 hidraw; the config switch would yank it)
#   2. switch the iBridge to USB config 1
#   3. load apple_ibridge + apple_touchbar with --ignore-install (the dfrd
#      blacklist hard-blocks them via `install ... /bin/true`; --ignore-install
#      bypasses that) -> this powers the panel
#   4. unload them, switch back to config 2
#   5. restart dfrd -> appletbdrm re-probes, renderer repaints
#   The open question this answers in practice: does the config-1 panel power
#   SURVIVE the switch back to config-2? (CHECKPOINT B in tb-hybrid-test.sh.)
#
# SAFETY: meant to be invoked DETACHED + time-bounded from the resume hook
# (systemd-run --on-active --collect -p RuntimeMaxSec=...) exactly like
# touchbar-relight-reload, so it can never wedge the resume path. The
# apple_ibridge appleib_add_device heap-OOB is patched, so the load/unload is
# safe (see [[t1-macbook-hibernate-works]]).
#
# WIRED 2026-06-15: installed to /usr/local/sbin/touchbar-config1-bounce and
# invoked (detached, +5s, time-bounded) from the config-2 branch of
# /usr/lib/systemd/system-sleep/51-touchbar-relight-hibernate.sh on hibernate
# resume. Confirmed post-hibernate: brings the custom config-2 bar back DIM but
# fully functional, no reboot. (Full brightness is ALS/bridgeOS-internal and
# reboot-only; S3 suspend is not recoverable — hook only fires on hibernate.)
set -u

DEV=/sys/bus/usb/devices/1-3/bConfigurationValue
log() { echo "tb-config1-bounce: $*"; }
subhids() { ls /sys/bus/hid/drivers/apple-touchbar 2>/dev/null | grep -c 0003; }

start_cfg="$(cat "$DEV" 2>/dev/null || echo '?')"
log "start (config=$start_cfg)"

# 1. release the config-2 hidraw consumer
systemctl stop dfrd 2>/dev/null || log "warn: could not stop dfrd"
sleep 1

# 2. -> config 1
echo 1 > "$DEV" 2>/dev/null || { log "ERR: could not write config 1"; }
sleep 1
log "switched to config=$(cat "$DEV" 2>/dev/null)"

# 3. load stock stack (powers the panel); --ignore-install bypasses the
#    dfrd blacklist's `install ... /bin/true` hard block.
modprobe --ignore-install apple_ibridge  2>/dev/null || log "warn: load apple_ibridge"
sleep 2
# idle_timeout=-1 dim_timeout=-1: never auto-OFF/auto-DIM during the brief window
# so the panel can't drop power again before we switch back to config 2.
modprobe --ignore-install apple_touchbar idle_timeout=-1 dim_timeout=-1 2>/dev/null \
    || log "warn: load apple_touchbar"
sleep 2
log "stock stack loaded; apple-touchbar sub-HIDs=$(subhids) (panel powered; dim is expected — ALS/brightness is reboot-only)"

# 4. unload stock stack + return to config 2
modprobe -r apple_touchbar 2>/dev/null || log "warn: -r apple_touchbar"
modprobe -r apple_ibridge  2>/dev/null || log "warn: -r apple_ibridge"
sleep 1
echo 2 > "$DEV" 2>/dev/null || log "ERR: could not write config 2"
sleep 2
log "back to config=$(cat "$DEV" 2>/dev/null)"

# 5. bring the custom renderer back
systemctl start dfrd 2>/dev/null || log "warn: could not start dfrd"
sleep 2
log "done (config=$(cat "$DEV" 2>/dev/null), dfrd=$(systemctl is-active dfrd 2>/dev/null))"
exit 0
