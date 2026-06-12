#!/bin/bash
# Post-hibernate Touch Bar relight (2026-06-12, the WORKING approach).
# On hibernate RESUME, schedule a full apple_ibridge-stack module reload ~5s later, DETACHED in a
# time-bounded transient unit. The reload (see /usr/local/sbin/touchbar-relight-reload) re-creates the
# virtual HIDs + fresh apple_touchbar probe = the cold-boot light-up, which relights the post-hibernate
# bar. Safe now that the appleib_add_device heap-OOB is fixed (the reload teardown no longer GPFs).
#
# WHY DETACHED + BOUNDED: the reload itself does `modprobe -r`; if that ever stalls, a transient unit
# with RuntimeMaxSec contains it instead of hanging the systemd-sleep resume path. We do NOTHING in the
# `pre` phase and nothing during the freeze, so there is no freeze-time deadlock surface.
#
# systemd-sleep args: $1 = pre|post, $2 = suspend|hibernate|...
set -u

case "$2" in
    hibernate) ;;
    *) exit 0 ;;        # s2idle family owned by 50-apple-ibridge-touchbar.sh; we only touch hibernate
esac

[ "$1" = "post" ] || exit 0   # nothing on pre; relight only after resume

echo "touchbar-relight: scheduling +5s detached module-reload relight"
if systemd-run --on-active=5s --collect --unit=touchbar-relight \
        -p RuntimeMaxSec=90 -p TimeoutStopSec=20 \
        --description='post-hibernate Touch Bar relight (apple_ibridge stack reload)' \
        /usr/local/sbin/touchbar-relight-reload; then
    echo "touchbar-relight: scheduled OK (touchbar-relight.timer -> +5s)"
else
    echo "touchbar-relight: warn: systemd-run scheduling failed"
fi
exit 0
