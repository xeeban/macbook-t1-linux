#!/bin/bash
# Post-hibernate Touch Bar relight — CONFIG-AWARE variant for the custom
# (appletbdrm / display-mode) stack. Installed to
# /usr/lib/systemd/system-sleep/51-touchbar-relight-hibernate.sh by
# dfrd/install/install.sh, replacing the stock-bar-only original.
#
# TWO REGIMES, decided at resume time by reading bConfigurationValue of 1-3:
#
#   config 2  (custom display stack active):
#       Do NOTHING here. The relight is handled by the kernel + systemd:
#       on resume the iBridge re-enumerates, apple_dfr_cfgsel re-selects
#       config 2, appletbdrm re-probes -> the DRM card is re-added -> the
#       udev rule's ENV{SYSTEMD_WANTS}=dfrd.service restarts the renderer,
#       which repaints the bar. Reloading apple_ibridge here is pointless
#       (patched, it just declines in config 2) and risks racing that
#       re-probe, so we skip it.
#       (Belt-and-suspenders: also kick dfrd.service in case the card did
#       NOT re-enumerate — a no-op restart if it's already up.)
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
    echo "touchbar-relight: config 2 (display stack) — kernel re-probe + dfrd handle relight; nudging dfrd.service"
    # Restart is a cheap no-op if the card re-add already pulled dfrd back up;
    # if the card did NOT re-enumerate, this repaints onto the surviving card.
    systemctl restart dfrd.service >/dev/null 2>&1 \
        || echo "touchbar-relight: warn: could not restart dfrd.service (may not be installed)"
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
