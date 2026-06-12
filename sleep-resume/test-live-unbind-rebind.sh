#!/bin/bash
# LIVE-endpoint Touch Bar unbind/rebind test -- NO hibernate, run on a LIT bar.
#
# Validates the deep-dive's "tear down while LIVE" hypothesis safely:
#   (a) does unbinding apple_touchbar on a LIVE endpoint return WITHOUT D-state?  (expected: yes)
#   (b) does rebinding re-run appletb_probe and RELIGHT the bar?                   (expected: yes,
#       because the unbind ran appletb_remove which clears `active`, so the rebind probe's
#       appletb_test_and_mark_active() fires the init light-up over the still-live transport)
#
# If (a) fails, a step will hang in D-state -- the per-step "self=D" log shows exactly where, and
# recovery is a reboot (SysRq R-E-I-S-U-B). If (a) and (b) pass, the pre-hibernate approach is
# validated: do this same quiesce+unbind in the sleep `pre` hook (endpoint still live), and resume
# cold-re-probes a lit bar. Keyboard (applespi) is unaffected; only the Touch Bar blinks out briefly.
set -u
log(){ echo "live-test: $*"; }
selfstate(){ awk '{print $3}' /proc/self/stat 2>/dev/null; }   # R = running/ok, D = uninterruptible stall

TBDRV=/sys/bus/hid/drivers/apple-touchbar

ids=""
for l in "$TBDRV"/0003:1D6B:0301.*; do [ -e "$l" ] && ids="$ids $(basename "$l")"; done
ids="${ids# }"
[ -n "$ids" ] || { log "no apple-touchbar-bound 1D6B:0301 HIDs found; abort"; exit 1; }
log "touchbar HIDs bound to apple-touchbar: $ids"

# (1) QUIESCE tb_work (fast on a live endpoint: worker commands succeed -> work completes -> idle)
for h in "$TBDRV"/0003:1D6B:0301.*/idle_timeout; do [ -e "$h" ] && printf -- -2 > "$h" 2>/dev/null; done
log "quiesced (idle_timeout=-2); self=$(selfstate); settle 3s"
sleep 3

# (2) UNBIND each id -> appletb_remove -> cancel_delayed_work_sync (returns fast iff endpoint live)
for id in $ids; do
    log "UNBIND $id (self before=$(selfstate)) ..."
    echo "$id" > "$TBDRV/unbind" 2>/dev/null
    log "  unbound $id (self after=$(selfstate))   <- reaching here = NO deadlock on this id"
done
log "all unbinds returned cleanly; bar should now be DARK. settle 2s"
sleep 2

# (3) REBIND each id -> appletb_probe -> (active was cleared) -> init light-up over live transport
for id in $ids; do
    log "BIND $id ..."
    echo "$id" > "$TBDRV/bind" 2>/dev/null
    log "  bound $id (self=$(selfstate))"
done
sleep 2

# restore never-blank state
for h in "$TBDRV"/0003:1D6B:0301.*/idle_timeout; do [ -e "$h" ] && printf -- -1 > "$h" 2>/dev/null; done

log "DONE. >>> WATCH THE BAR: did it RELIGHT? <<<"
log "if lit + every step logged 'self after=R' -> live teardown is SAFE and the pre-hibernate plan is GO."
