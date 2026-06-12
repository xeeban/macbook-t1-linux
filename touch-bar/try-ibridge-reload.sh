#!/bin/bash
# FINAL exhaustive relight test (run post-hibernate, bar dark): full reload of the apple_ibridge
# stack — unload apple_touchbar + apple_ibridge, reload both. This destroys and RE-CREATES the
# virtual HIDs and runs a totally fresh apple_touchbar probe (the most complete driver re-init
# short of a reboot). Now safe to run because the appleib_add_device heap-OOB is fixed (the gate
# proved appletb_remove teardown is clean). Logs its own proc-state at each step so a D-state stall
# is visible. SAVE WORK FIRST. If a `modprobe -r` hangs >30s (self=D), it's the old deadlock —
# reboot (SysRq R-E-I-S-U-B); don't stack commands.
set -u
log(){ echo "reload: $*"; }
selfstate(){ awk '{print $3}' /proc/self/stat 2>/dev/null; }
[ "$(id -u)" -eq 0 ] || { echo "run as root: sudo $0"; exit 1; }

SINCE="$(date '+%Y-%m-%d %H:%M:%S')"
echo; log ">>> WATCH THE TOUCH BAR through the reload (~6s) <<<"; echo

log "unload apple_touchbar (self=$(selfstate))"
modprobe -r apple_touchbar 2>&1 | sed 's/^/  /'
log "  after -r apple_touchbar: self=$(selfstate)  (R=ok, D=stalled)"

log "unload apple_ibridge (self=$(selfstate))"
modprobe -r apple_ibridge 2>&1 | sed 's/^/  /'
log "  after -r apple_ibridge: self=$(selfstate)"
sleep 1

log "load apple_ibridge"
modprobe apple_ibridge 2>&1 | sed 's/^/  /'
sleep 2
log "load apple_touchbar"
modprobe apple_touchbar 2>&1 | sed 's/^/  /'
sleep 3

echo
echo "=== crash markers (want NONE) ==="
journalctl -k -b --since "$SINCE" 2>/dev/null | grep -E 'list_del|general protection|protection fault|Oops|BUG:|use-after-free' || echo "CLEAN — no crash markers."
echo "=== D-state (want none) ==="; ps -eo state,pid,comm | awk '$1=="D"{print}'; echo "(nothing above = good)"
echo "=== fresh re-probe + any display/hw error ==="
journalctl -k -b --since "$SINCE" 2>/dev/null | grep -iE 'apple-touchbar|tb:|Touchbar|hw open failed|Failed to set touch bar|1D6B:0301' | tail -14
echo "=== bound: $(ls /sys/bus/hid/drivers/apple-touchbar/ 2>/dev/null | grep -cE '0003:1D6B:0301') sub-HID(s); 'Unknown collection' this run: $(journalctl -k -b --since "$SINCE" 2>/dev/null | grep -c 'Unknown collection') (want 0) ==="
echo
echo ">>> Tell Claude: did the bar relight after the full reload? <<<"
