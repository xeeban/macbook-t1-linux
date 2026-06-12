#!/bin/bash
# THE GATE (MORNING-PLAN.md Step 3): live-endpoint iBridge power-cycle, NO hibernate.
# This is the EXACT operation that GPF'd the kernel before the appleib_add_device OOB fix.
# On a fully-awake machine it proves the teardown is now safe AND that a USB re-enumerate
# relights the Touch Bar (the cold-boot mechanism). Run as root. SAVE OPEN WORK FIRST.
#
# PASS = bar goes dark then RELIGHTS, no crash markers, both writes return promptly, no D-state.
set -u
log() { echo "gate: $*"; }
selfstate() { awk '{print $3}' /proc/self/stat 2>/dev/null; }

[ "$(id -u)" -eq 0 ] || { echo "run as root: sudo $0"; exit 1; }

# Locate the iBridge USB device (05ac:8600) by VID/PID, not a hardcoded port.
D=""
for d in /sys/bus/usb/devices/[0-9]*; do
    [ -f "$d/idVendor" ] || continue
    if [ "$(cat "$d/idVendor" 2>/dev/null)" = "05ac" ] \
       && [ "$(cat "$d/idProduct" 2>/dev/null)" = "8600" ]; then
        D="$d"; break
    fi
done
[ -n "$D" ] || { log "iBridge 05ac:8600 not found"; exit 1; }
log "iBridge: $D (authorized=$(cat "$D/authorized" 2>/dev/null))"

SINCE="$(date '+%Y-%m-%d %H:%M:%S')"
echo
log ">>> WATCH THE TOUCH BAR: it should go DARK now, then RELIGHT a few seconds later <<<"
echo

log "deauthorize (the old crash path: appleib teardown -> hid_destroy_device)"
echo 0 > "$D/authorized" 2>/dev/null
log "  self proc-state after deauthorize: $(selfstate)  (R=ok, D=stalled)"
sleep 3
log "reauthorize (re-enumerate -> fresh probe -> cold-boot light-up)"
echo 1 > "$D/authorized" 2>/dev/null
log "  self proc-state after reauthorize: $(selfstate)"
sleep 4

echo
echo "=== crash markers since the cycle (want NONE) ==="
if journalctl -k -b --since "$SINCE" 2>/dev/null | grep -E 'list_del|general protection|protection fault|Oops|BUG:|use-after-free|WARNING'; then
    echo ">>> FAIL: crash markers above. Revert (revert-ibridge-teardown-patch.sh) + reboot; do NOT hibernate-test."
else
    echo "CLEAN — no crash markers."
fi

echo
echo "=== D-state (uninterruptible) procs — want NONE ==="
ds="$(ps -eo state,pid,comm | awk '$1=="D"{print}')"
if [ -n "$ds" ]; then echo "$ds"; echo ">>> FAIL: D-state task(s) present."; else echo "none — good."; fi

echo
echo "=== iBridge + touchbar state now ==="
log "iBridge authorized=$(cat "$D/authorized" 2>/dev/null)"
echo "  apple-touchbar bound: $(ls /sys/bus/hid/drivers/apple-touchbar/ 2>/dev/null | grep -cE '0003:1D6B:0301') sub-HID(s)"

echo
echo "PASS = bar RELIT + 'CLEAN' + no D-state + authorized=1 + 2 sub-HIDs bound."
echo ">>> Tell Claude: did the bar relight, and paste the CLEAN/FAIL lines. <<<"
