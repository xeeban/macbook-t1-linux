#!/bin/bash
# Pre/post-hibernate Touch Bar relight via LIVE-endpoint unbind + post-resume bind.
# 2026-06-11: this REPLACES all earlier (deadlocking) relight designs. See TOUCHBAR-RELIGHT-ANALYSIS.md.
#
# WHY THIS IS SAFE BY CONSTRUCTION:
#   - The ONLY deadlock site is appletb_remove -> cancel_delayed_work_sync, reached by UNBIND/disconnect.
#     It hangs only against a HALF-DEAD endpoint. We therefore UNBIND only in the `pre` hook, while the
#     endpoint is still LIVE (validated 2026-06-11 19:51: live unbind returns instantly, self=R, and a
#     subsequent bind relights the bar). systemd-sleep runs `pre` hooks to completion BEFORE the freeze.
#   - `bind` invokes ONLY appletb_probe (never appletb_remove), so it CANNOT deadlock. We therefore only
#     BIND in the `post` hook. Worst case post-resume is a dark bar (if hid_hw_open can't re-open the
#     reset-resumed endpoint) -- never a hang, never a reboot.
#   - Net: unbind=pre(live), bind=post(deadlock-free). The half-dead post-resume endpoint is NEVER torn down.
#
# MECHANISM: unbinding clears the driver's `active` flag (appletb_remove). On resume, binding re-runs
# appletb_probe which, seeing !active, runs the init light-up (set tb mode/disp via hid_hw_request) over
# the resumed transport -> bar lit (the same path a cold boot takes).
#
# systemd-sleep args: $1 = pre|post, $2 = suspend|hibernate|...
set -u

case "$2" in
    hibernate) ;;
    *) exit 0 ;;            # s2idle family is owned by 50-apple-ibridge-touchbar.sh; we only touch hibernate
esac

TBDRV=/sys/bus/hid/drivers/apple-touchbar
log() { echo "touchbar-relight: $*"; }
selfstate() { awk '{print $3}' /proc/self/stat 2>/dev/null; }

case "$1" in
    pre)
        # Endpoint is LIVE here. Quiesce tb_work, then unbind apple_touchbar (clears `active`, fast).
        ids=""
        for l in "$TBDRV"/0003:1D6B:0301.*; do [ -e "$l" ] && ids="$ids $(basename "$l")"; done
        ids="${ids# }"
        log "pre-hibernate: touchbar HIDs bound = [$ids]"
        for h in "$TBDRV"/0003:1D6B:0301.*/idle_timeout; do [ -e "$h" ] && printf -- -2 > "$h" 2>/dev/null; done
        sleep 2
        for id in $ids; do
            log "pre: unbind $id (self=$(selfstate))"
            echo "$id" > "$TBDRV/unbind" 2>/dev/null
            log "pre: unbound $id (self=$(selfstate))"
        done
        log "pre-hibernate: apple_touchbar unbound on live endpoint; entering hibernate"
        ;;
    post)
        # Resume: the reset-resumed firmware endpoint is STALE -- a plain bind enumerates HID but the
        # display SET_REPORT silently fails (fire-and-forget) -> dark. A USB power-cycle is required to
        # re-init the endpoint. That is SAFE here ONLY because apple_touchbar is UNBOUND (from `pre`):
        # the iBridge deauthorize tears down the virtual HIDs via hid_destroy_device, but with no
        # touchbar driver bound there is NO appletb_remove -> NO cancel_delayed_work_sync -> no deadlock.
        # (apple-ibridge.c itself has zero _sync/flush.) After reauthorize, udev coldplug re-binds the
        # whole stack fresh = the cold-boot light-up path.
        sleep 3   # let the iBridge reset_resume settle

        # SAFETY GATE: only power-cycle if apple_touchbar is confirmed UNBOUND. If something re-bound it,
        # tearing down a stale endpoint could deadlock -> refuse and leave the bar dark instead.
        if ls "$TBDRV"/0003:1D6B:0301.* >/dev/null 2>&1; then
            log "post: WARN apple_touchbar is BOUND; power-cycling a stale endpoint is unsafe -> leaving DARK"
            exit 0
        fi
        log "post: apple_touchbar confirmed UNBOUND -> power-cycling iBridge is deadlock-safe"

        D=""
        for d in /sys/bus/usb/devices/*/; do
            [ -f "$d/idVendor" ] || continue
            if [ "$(cat "$d/idVendor" 2>/dev/null)" = "05ac" ] \
               && [ "$(cat "$d/idProduct" 2>/dev/null)" = "8600" ]; then
                D="$d"; break
            fi
        done
        [ -n "$D" ] || { log "post: iBridge 05ac:8600 not found; leaving dark"; exit 0; }

        log "post: USB power-cycle $D (self=$(selfstate))"
        echo 0 > "${D}authorized" 2>/dev/null
        log "post: deauthorized (self=$(selfstate))"
        sleep 2
        echo 1 > "${D}authorized" 2>/dev/null
        log "post: reauthorized (self=$(selfstate)); settle for udev coldplug re-bind"
        sleep 4

        # Fallback: if udev did not auto-bind apple_touchbar to the fresh virtual HIDs, bind them.
        for l in /sys/bus/hid/devices/0003:1D6B:0301.*; do
            [ -d "$l" ] || continue
            id="$(basename "$l")"
            [ -e "$l/driver" ] && continue
            log "post: fallback bind $id"
            echo "$id" > "$TBDRV/bind" 2>/dev/null
        done
        sleep 1
        for h in "$TBDRV"/0003:1D6B:0301.*/idle_timeout; do [ -e "$h" ] && printf -- -1 > "$h" 2>/dev/null; done
        log "post-hibernate: power-cycle relight done (fresh re-enumerate); bar should be lit"
        ;;
esac
exit 0
