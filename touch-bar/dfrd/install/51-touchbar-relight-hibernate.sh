#!/bin/bash
# Post-hibernate Touch Bar relight — CONFIG-AWARE variant for the custom
# (appletbdrm / display-mode) stack. Installed to
# /usr/lib/systemd/system-sleep/51-touchbar-relight-hibernate.sh by
# dfrd/install/install.sh, replacing the stock-bar-only original.
#
# TWO REGIMES, decided at resume time by reading bConfigurationValue of 1-3:
#
#   config 2  (custom display stack active):
#       Post-hibernate the panel comes back DARK: pixels repaint fine but the
#       backlight is off, because panel POWER is a config-1 apple_touchbar
#       firmware function with NO config-2 equivalent (proven 2026-06-15 — the
#       config-2 bulk protocol and every iface-6 vendor HID report are inert for
#       panel power). So a dfrd restart alone leaves it dark. Instead schedule
#       the "config-1 bounce" (detached, time-bounded): briefly switch to config
#       1, load apple_ibridge+apple_touchbar to power the panel, switch back to
#       config 2. That relight SURVIVES the switch back -> bar returns DIM but
#       fully functional, no reboot. (Brightness is ALS/bridgeOS-internal and
#       reboot-only; we accept dim. S3 suspend is NOT recoverable, hence the
#       hibernate-only guard below.)
#
#   config 1  (stock firmware bar — i.e. the custom stack is uninstalled or
#             disabled): fall back to the ORIGINAL full apple_ibridge-stack
#             reload, the only thing that relights the stock post-hibernate bar.
#
# systemd-sleep args: $1 = pre|post, $2 = suspend|hibernate|...
set -u

case "$2" in
    hibernate) ;;
    *) exit 0 ;;        # s2idle family owned by 50-apple-ibridge-touchbar.sh
esac
[ "$1" = "post" ] || exit 0   # relight only after resume

cfg="$(cat /sys/bus/usb/devices/1-3/bConfigurationValue 2>/dev/null || echo '?')"

if [ "$cfg" = "2" ]; then
    # Schedule the config-1 bounce +5s detached (let the resume settle first:
    # card re-enumerate, dfrd come up), time-bounded so it can never wedge resume.
    # The bounce stops/starts dfrd itself. Relights the bar DIM (see header).
    echo "touchbar-relight: config 2 (display stack) — scheduling +5s detached config-1 bounce relight"
    if systemd-run --on-active=5s --collect --unit=touchbar-config1-bounce \
            -p RuntimeMaxSec=120 -p TimeoutStopSec=20 \
            --description='post-hibernate Touch Bar relight (config-1 bounce)' \
            /usr/local/sbin/touchbar-config1-bounce; then
        echo "touchbar-relight: bounce scheduled OK (touchbar-config1-bounce -> +5s)"
    else
        echo "touchbar-relight: warn: systemd-run scheduling failed; falling back to dfrd restart"
        systemctl restart dfrd.service >/dev/null 2>&1 || true
    fi
    exit 0
fi

# --- config 1 / stock-bar fallback (original behavior) ----------------------
echo "touchbar-relight: config $cfg (stock bar) — scheduling +5s detached apple_ibridge reload"
if systemd-run --on-active=5s --collect --unit=touchbar-relight \
        -p RuntimeMaxSec=90 -p TimeoutStopSec=20 \
        --description='post-hibernate Touch Bar relight (apple_ibridge stack reload)' \
        /usr/local/sbin/touchbar-relight-reload; then
    echo "touchbar-relight: scheduled OK (touchbar-relight.timer -> +5s)"
else
    echo "touchbar-relight: warn: systemd-run scheduling failed"
fi
exit 0
