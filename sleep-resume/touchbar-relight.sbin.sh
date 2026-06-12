#!/bin/bash
# Installed to /usr/local/sbin/touchbar-relight (see test-05-deploy-relight-hook.sh).
# Invoked ~15s after hibernate resume by a transient systemd unit (51-touchbar-relight-hibernate.sh).
#
# ===========================================================================================
# ROOT CAUSE (deep driver analysis, 2026-06-11, vs /usr/src/apple-ib-drv-r307.4afd309 + live):
#   apple_touchbar uses a SINGLETON appletb_dev allocated once at module load, freed only at
#   module UNLOAD. Across hibernate it keeps DEAD cached transport (re-enumerated usb_iface,
#   reset disp_iface endpoint). appletb_reset_resume only flips flags + re-queues the worker;
#   it does NO hid_hw_open / NO USB re-enumerate. set_tb_disp rides a fire-and-forget/VOID
#   ll_request, so a failed SET_REPORT is SILENT and the driver believes it lit -> bar stays DARK.
#   The ONLY relight is a FRESH appletb_probe over a RE-ENUMERATED iBridge (05ac:8600) endpoint.
#   The probe light-up (set pnd_tb_mode/disp=UPD) runs ONLY if appletb_test_and_mark_active() sees
#   !active -- and the singleton keeps active=TRUE across hibernate. active is cleared ONLY by
#   appletb_remove on a live iface, which a USB DISCONNECT triggers. So the winning order is:
#     (A) DRAIN tb_work  ->  (B) USB disconnect (clears active, power-cycles endpoint)  ->
#     (C) cold re-probe runs the light-up over fresh transport.
#
# DEADLOCK SAFETY: every teardown cascades to appletb_remove -> cancel_delayed_work_sync, which
#   hangs (D-state) iff tb_work is wedged mid usb_control_msg/hid_hw_request on a half-dead endpoint.
#   We DRAIN first: writing idle_timeout=-2 forces ONE worker pass that returns WITHOUT re-arming
#   (idle_timeout=-2 -> min_timeout=-1; a failed set on dead transport leaves need_reschedule=false),
#   so the cascaded sync-wait returns fast. A clean whole-device deauthorize also fast-fails in-flight
#   URBs with -ENODEV. We NEVER modprobe -r apple_touchbar and NEVER sysfs-unbind a 0301 directly.
#
# HARD RULES: on ANY failure -> LEAVE THE BAR DARK and exit non-zero. NEVER auto-escalate to a
#   heavier teardown (that re-opens the deadlock). Physical lit-state must be eyeballed by a human
#   (set_tb_disp is fire-and-forget; no sysfs readback proves photons).
# ===========================================================================================
set -u
log() { echo "touchbar-relight: $*"; }

TB=/sys/bus/hid/drivers/apple-touchbar
DRAIN_SETTLE=8     # > worker worst-case-ish; deauth also fast-fails residual transfers
REAUTH_SETTLE=6    # let udev rebind usbhid -> apple-ibridge-hid -> apple-touchbar (fresh probe)

# Resolve the single LIVE mode_iface sub-HID (the only 1D6B:0301 exposing idle_timeout).
# Stale/orphan generations from prior cycles have no idle_timeout attr -> skipped.
live_node() {
    local h
    for h in "$TB"/0003:1D6B:0301.*; do
        [ -e "$h/idle_timeout" ] && { printf '%s' "$h"; return 0; }
    done
    return 1
}

node="$(live_node)" || { log "FAIL: no live mode_iface (idle_timeout) sub-HID; leaving dark"; exit 1; }
log "live mode_iface: $node (idle_timeout=$(cat "$node/idle_timeout" 2>/dev/null))"

# (A) DRAIN tb_work: force one OFF pass that returns without re-arming, so the upcoming
#     disconnect-driven appletb_remove -> cancel_delayed_work_sync returns fast.
log "draining tb_work (idle_timeout=-2), settle ${DRAIN_SETTLE}s"
printf -- -2 > "$node/idle_timeout" 2>/dev/null || log "warn: write idle_timeout=-2 failed"
sleep "$DRAIN_SETTLE"

# Find the iBridge USB device (05ac:8600).
D=""
for d in /sys/bus/usb/devices/*/; do
    [ -f "$d/idVendor" ] || continue
    if [ "$(cat "$d/idVendor" 2>/dev/null)" = "05ac" ] \
       && [ "$(cat "$d/idProduct" 2>/dev/null)" = "8600" ]; then
        D="$d"; break
    fi
done
[ -n "$D" ] || { log "FAIL: iBridge 05ac:8600 not found; leaving dark"; exit 1; }
log "iBridge USB: $D (authorized=$(cat "${D}authorized" 2>/dev/null))"

# (B) ONE clean USB re-enumerate. deauthorize drives the disconnect -> appletb_remove on the live
#     iface clears active and fast-fails URBs; reauthorize cold-enumerates -> fresh demux + probe.
log "deauthorize (disconnect clears active; fast-fails in-flight URBs)"
echo 0 > "${D}authorized" 2>/dev/null || log "warn: deauthorize write failed"
log "self proc-state after deauth: $(awk '{print $3}' /proc/self/stat 2>/dev/null) (R=ok, D=stalled on line-1301 set_tb_mode)"
sleep 2
log "reauthorize (cold re-enumerate -> fresh appletb_probe light-up), settle ${REAUTH_SETTLE}s"
echo 1 > "${D}authorized" 2>/dev/null || log "warn: reauthorize write failed"
sleep "$REAUTH_SETTLE"

# SUCCESS CHECK: a fresh live mode_iface must exist, and this cycle must show no -19 / no stale-worker WARN.
newnode="$(live_node)" || { log "FAIL: no live mode_iface after reauth; leaving dark, NOT escalating"; exit 1; }
if journalctl -k -b --since "$((DRAIN_SETTLE + REAUTH_SETTLE + 10)) sec ago" 2>/dev/null \
     | grep -qiE 'hw open failed \(-19\)|hid_hw_request'; then
    log "FAIL: 'hw open failed (-19)' or stale-worker hid_hw_request WARN this cycle; leaving dark, NOT escalating"
    exit 1
fi

# (C) RESTORE the never-blank state on the fresh live node.
printf -- -1 > "$newnode/idle_timeout" 2>/dev/null || log "warn: restore idle_timeout=-1 failed"
log "sequence complete (live node: $newnode). NOTE: physical lit-state must be confirmed by a human."
exit 0
