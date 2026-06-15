#!/bin/bash
# tb-bright-test.sh — the bounce relights the panel (proven), but config-2 comes
# up DIM. Brightness is config-1-only (the config-2 disp report 0x22 is ignored).
# This tests: (1) does input brighten the panel to FULL while in config 1 with
# apple_touchbar live and dimming disabled, and (2) does that full brightness
# SURVIVE the switch back to config 2?  Run as root from any state. Reboot recovers.
set -u
DEV=/sys/bus/usb/devices/1-3
CFG=$DEV/bConfigurationValue
say(){ echo; echo ">>> $*"; }
subhids(){ ls /sys/bus/hid/drivers/apple-touchbar 2>/dev/null | grep -c 0003; }

say "0. stop dfrd; -> config 1"
systemctl stop dfrd; sleep 1
echo 1 > $CFG; sleep 1

say "1. load stock stack with dimming DISABLED (idle_timeout=-1 dim_timeout=-1)"
modprobe --ignore-install apple_ibridge ; sleep 2
modprobe --ignore-install apple_touchbar idle_timeout=-1 dim_timeout=-1 ; sleep 2
echo "   config=$(cat $CFG)  apple-touchbar sub-HIDs=$(subhids)"

say "CHECKPOINT A >>> TAP KEYS / SWIPE THE BAR REPEATEDLY for the next 10s."
echo "    Watch: does it go to FULL brightness (vs the dim we saw)? (pausing 10s)"
sleep 10

say "2. switch back -> config 2 (custom bar), start dfrd"
modprobe -r apple_touchbar 2>/dev/null
modprobe -r apple_ibridge  2>/dev/null
sleep 1
echo 2 > $CFG; sleep 2
systemctl start dfrd; sleep 3

say "CHECKPOINT B >>> Is the custom config-2 bar now FULL bright (did it carry over)?"
echo "   config=$(cat $CFG)  dfrd=$(systemctl is-active dfrd)"
echo
echo "Report: A brightness (dim/full, and did tapping change it?) and B (dim/full)."
