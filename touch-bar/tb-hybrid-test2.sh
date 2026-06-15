#!/bin/bash
# tb-hybrid-test2.sh — v2 of the config-1 bounce test, now with a FULL USB
# RE-ENUMERATION into config 1 (not just a SET_CONFIGURATION).
#
# WHY: v1 (echo 1 > bConfigurationValue + modprobe stack) left the panel DARK
# post-S3-suspend, yet the same modprobe stack was LIT post-S4-hibernate
# (investigation 2026-06-14). The difference: S4 resume RE-ENUMERATES the USB
# device (fresh descriptors, cold-boot-like), S3 resume just wakes it. So the
# relight likely needs the device to be re-enumerated in config 1, which we
# force here by unbinding apple_dfr_cfgsel (so it can't grab config 2) and
# toggling `authorized` 0->1.
#
# Run as root from a dark config-2 state. Watch the bar at A and B; TAP A KEY
# at each checkpoint (input drives apple_touchbar to full brightness). Reboot
# fully recovers.
set -u
DEV=/sys/bus/usb/devices/1-3
CFG=$DEV/bConfigurationValue
CFGSEL=/sys/bus/usb/drivers/apple_dfr_cfgsel
say(){ echo; echo ">>> $*"; }
subhids(){ ls /sys/bus/hid/drivers/apple-touchbar 2>/dev/null | grep -c 0003; }
reenum(){ echo 0 > $DEV/authorized; sleep 1; echo 1 > $DEV/authorized; sleep 2; }

say "0. start: config=$(cat $CFG) (expect 2, dark); stop dfrd"
systemctl stop dfrd; sleep 1

say "1. unbind apple_dfr_cfgsel so it won't force config 2 on re-enum"
echo -n 1-3 > $CFGSEL/unbind 2>/dev/null || echo "   (cfgsel already unbound?)"
sleep 1

say "2. RE-ENUMERATE the iBridge (authorized 0->1) -> should come up config 1"
reenum
echo "   config now = $(cat $CFG 2>/dev/null)"
if [ "$(cat $CFG 2>/dev/null)" != "1" ]; then
    echo "   forcing config 1 explicitly"; echo 1 > $CFG 2>/dev/null; sleep 1
fi

say "3. load stock stack on the freshly-enumerated config-1 device"
modprobe --ignore-install apple_ibridge  ; sleep 2
modprobe --ignore-install apple_touchbar ; sleep 2
echo "   config=$(cat $CFG)  apple-touchbar sub-HIDs=$(subhids)"
say "CHECKPOINT A >>> WATCH + TAP A KEY: is the bar LIT now? (pausing 5s)"
sleep 5

say "4. back to config 2: unload stack, rebind cfgsel, re-enumerate"
modprobe -r apple_touchbar 2>/dev/null
modprobe -r apple_ibridge  2>/dev/null
sleep 1
echo -n 1-3 > $CFGSEL/bind 2>/dev/null || echo "   (cfgsel bind failed?)"
sleep 1
reenum
echo "   config now = $(cat $CFG 2>/dev/null) (expect 2)"
if [ "$(cat $CFG 2>/dev/null)" != "2" ]; then
    echo "   forcing config 2 explicitly"; echo 2 > $CFG 2>/dev/null; sleep 2
fi
systemctl start dfrd; sleep 3

say "CHECKPOINT B >>> WATCH + TAP: did the custom config-2 bar come up LIT?"
echo "   config=$(cat $CFG)  dfrd=$(systemctl is-active dfrd)"
echo
echo "Report A (lit/dark) and B (lit/dark). If still messy: reboot recovers."
