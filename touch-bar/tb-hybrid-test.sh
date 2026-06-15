#!/bin/bash
# Decisive test: does a config-1 apple_touchbar disp-power relight SURVIVE the
# switch back to config-2 (appletbdrm custom bar)? Run as root. Watch the bar at
# the two ">>> WATCH" checkpoints. Reboot fully recovers afterwards.
set -u
DEV=/sys/bus/usb/devices/1-3/bConfigurationValue
say(){ echo; echo ">>> $*"; }
subhids(){ ls /sys/bus/hid/drivers/apple-touchbar 2>/dev/null | grep -c 0003; }

say "0. start: config=$(cat $DEV) (expect 2, dark); stopping dfrd"
systemctl stop dfrd
sleep 1

say "1. switch -> config 1"
echo 1 > $DEV
sleep 1

say "2. load stock stack (this powers the panel)"
modprobe --ignore-install apple_ibridge  ; sleep 2
modprobe --ignore-install apple_touchbar ; sleep 2
echo "   config=$(cat $DEV)  apple-touchbar sub-HIDs=$(subhids)"
say "CHECKPOINT A >>> WATCH THE BAR: is it LIT now (stock F-keys)?  (pausing 4s)"
sleep 4

say "3. switch back -> config 2 (custom appletbdrm bar), keep panel powered"
modprobe -r apple_touchbar 2>/dev/null
modprobe -r apple_ibridge  2>/dev/null
sleep 1
echo 2 > $DEV
sleep 2
echo "   config=$(cat $DEV)"
systemctl start dfrd
sleep 3

say "CHECKPOINT B >>> WATCH THE BAR: did it STAY lit in custom config-2 mode?"
echo "   config=$(cat $DEV)  dfrd=$(systemctl is-active dfrd)"
echo
echo "Report: bar state at CHECKPOINT A (lit/dark) and CHECKPOINT B (lit/dark)."
