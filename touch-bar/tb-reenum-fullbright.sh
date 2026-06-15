#!/bin/bash
# tb-reenum-fullbright.sh — the decisive "native config-1" test.
#
# Established so far (post-hibernate, no reboot): switching to config-1 via a
# bare SET_CONFIGURATION lets apple_touchbar toggle the panel off<->dim, but the
# T1 firmware's ambient-light->brightness loop NEVER engages, so full brightness
# is unreachable. The STOCK bar reaches FULL because it is ENUMERATED in config-1
# from boot. This test reproduces that as closely as possible WITHOUT rebooting:
# a full USB re-enumeration into config-1 (unbind apple_dfr_cfgsel so it can't
# force config-2, then authorized 0->1), with verification gates so we get clean
# data even if apple_ibridge is slow to bind. Then full reload + wait + ALS nudge.
#
# Run as root from a post-hibernate config-2 state. WATCH THE BAR. Report whether
# brightness EVER reaches full (and at which step), A (config-1), B (config-2).
# Reboot fully recovers.
set -u
DEV=/sys/bus/usb/devices/1-3
CFG=$DEV/bConfigurationValue
CFGSEL=/sys/bus/usb/drivers/apple_dfr_cfgsel
say(){ echo; echo ">>> $*"; }
subhids(){ ls /sys/bus/hid/drivers/apple-touchbar 2>/dev/null | grep -c 0003; }
vhids(){ ls /sys/bus/hid/devices 2>/dev/null | grep -c '1D6B:0301'; }       # apple_ibridge virtual HIDs
ifaces(){ ls -d ${DEV}:*/ 2>/dev/null | wc -l; }

say "0. stop dfrd"
systemctl stop dfrd; sleep 1

say "1. unbind apple_dfr_cfgsel (so re-enum comes up in the device-default config 1)"
echo -n 1-3 > $CFGSEL/unbind 2>/dev/null || echo "   (cfgsel already unbound)"
sleep 1

say "2. FULL USB re-enumeration: authorized 0 -> 1"
echo 0 > $DEV/authorized; sleep 2
echo 1 > $DEV/authorized; sleep 5
echo "   config=$(cat $CFG 2>/dev/null)  interfaces=$(ifaces)"
if [ "$(cat $CFG 2>/dev/null)" != "1" ]; then
    echo "   not config 1 — forcing"; echo 1 > $CFG 2>/dev/null; sleep 3
    echo "   config=$(cat $CFG 2>/dev/null)  interfaces=$(ifaces)"
fi

say "3. load apple_ibridge (verify it creates virtual HIDs before proceeding)"
modprobe --ignore-install apple_ibridge; sleep 3
for try in 1 2 3 4; do
    [ "$(vhids)" -gt 0 ] && break
    echo "   waiting for apple_ibridge virtual HIDs (try $try)..."; sleep 2
done
echo "   virtual HIDs (1D6B:0301) = $(vhids)"
if [ "$(vhids)" -eq 0 ]; then
    echo "   !! apple_ibridge did NOT create virtual HIDs — re-enum/bind failed."
    echo "      (Same failure mode as v2. This test is inconclusive; aborting to config 2.)"
    modprobe -r apple_ibridge 2>/dev/null
    echo -n 1-3 > $CFGSEL/bind 2>/dev/null
    sleep 1; echo 0 > $DEV/authorized; sleep 2; echo 1 > $DEV/authorized; sleep 4
    [ "$(cat $CFG 2>/dev/null)" = "2" ] || { echo 2 > $CFG; sleep 2; }
    systemctl start dfrd; exit 1
fi

say "4. load apple_touchbar"
modprobe --ignore-install apple_touchbar; sleep 3
echo "   config=$(cat $CFG)  apple-touchbar sub-HIDs=$(subhids)"

say "5. WAIT ~18s NATIVELY in config 1 — WATCH the bar. Does it reach FULL?"
echo "    Cover/uncover the ambient-light sensor (near the camera) and tap the bar."
for s in 18 15 12 9 6 3; do echo "   ...${s}s left"; sleep 3; done

say "CHECKPOINT A >>> in NATIVE config 1: bar FULL / DIM / OFF?  (pausing 6s)"
echo "   config=$(cat $CFG)  sub-HIDs=$(subhids)"
sleep 6

say "6. back to config 2: unload, rebind cfgsel, re-enumerate, start dfrd"
modprobe -r apple_touchbar 2>/dev/null
modprobe -r apple_ibridge  2>/dev/null
sleep 1
echo -n 1-3 > $CFGSEL/bind 2>/dev/null || echo "   (cfgsel bind failed?)"
sleep 1
echo 0 > $DEV/authorized; sleep 2; echo 1 > $DEV/authorized; sleep 5
[ "$(cat $CFG 2>/dev/null)" = "2" ] || { echo "   forcing config 2"; echo 2 > $CFG; sleep 2; }
systemctl start dfrd; sleep 3

say "CHECKPOINT B >>> in CONFIG 2 (custom bar): FULL / DIM / OFF?"
echo "   config=$(cat $CFG)  dfrd=$(systemctl is-active dfrd)"
echo
echo "Report: did brightness EVER reach full (and at which step), A (config-1),"
echo "B (config-2). If apple_ibridge failed to bind, say so."
