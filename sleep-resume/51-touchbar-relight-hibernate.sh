#!/bin/bash
# 2026-06-11: post-hibernate Touch Bar relight -- schedule a DETACHED transient unit.
#
# Companion to 50-apple-ibridge-touchbar.sh, which SKIPS hibernate (its
# `modprobe -r apple_touchbar` can deadlock appletb_remove in D-state -> hard reboot).
# Skipping it leaves the Touch Bar DARK on hibernate resume; this relights it.
#
# This hook does NOTHING heavy itself. It schedules /usr/local/sbin/touchbar-relight
# to run ~15s after resume via `systemd-run`, as an independent transient unit.
# Hard-won reasons (all failed on 2026-06-11, see touchbar-relight.sbin.sh header):
#   - inline `sleep 15` here -> systemd SIGKILLs the hook mid-reset (device left
#     deauthorized). - backgrounded `&` -> killed with the sleep service cgroup.
#   - `systemd-run -c '...$d...'` -> systemd mangles $ in ExecStart -> blank path.
# Pointing systemd-run at a real SCRIPT FILE (no $ on the command line) + a 15s timer
# avoids the early-resume race (appletb_set_tb_worker) AND all three failure modes.
#
# systemd-sleep args: $1 = pre|post, $2 = suspend|hibernate|...
set -u

case "$1/$2" in
    post/hibernate) ;;
    *) exit 0 ;;
esac

echo "touchbar-relight: scheduling +15s detached relight (transient unit)"
if systemd-run --on-active=15s --collect --unit=touchbar-relight \
        -p TimeoutStartSec=60s -p TimeoutStopSec=20s \
        --description='post-hibernate Touch Bar relight (quiesce + USB re-enumerate, no modprobe)' \
        /usr/local/sbin/touchbar-relight; then
    echo "touchbar-relight: scheduled OK (touchbar-relight.timer -> +15s)"
else
    echo "touchbar-relight: warn: systemd-run scheduling failed"
fi
exit 0
