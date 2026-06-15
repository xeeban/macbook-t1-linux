#!/bin/bash
# tb-fullbright-test.sh — does a FAITHFUL stock-style config-1 relight reach
# FULL brightness post-hibernate, and does full brightness INHERIT into config-2?
#
# WHY: the STOCK config-1 bar comes back at FULL brightness after hibernate via
# touchbar-relight-reload (modprobe -r both -> reload). Our shipped config-1
# bounce only reached DIM — but it did a quick SET_CONFIGURATION(1) and did NOT
# (a) do the full unload->reload cycle, nor (b) WAIT in config 1 for the T1
# firmware's ambient-light->brightness loop to spin up. This test adds exactly
# those two things to see if brightness climbs to full while in config 1, and
# whether that full state survives the switch back to config 2 (panel POWER did).
#
# Run as root from a post-hibernate dim/dark config-2 state. WATCH THE BAR.
# Tell me: at what countdown second (if any) brightness changes, A (config-1
# full/dim) and B (config-2 full/dim). Reboot fully recovers.
set -u
DEV=/sys/bus/usb/devices/1-3
CFG=$DEV/bConfigurationValue
say(){ echo; echo ">>> $*"; }
subhids(){ ls /sys/bus/hid/drivers/apple-touchbar 2>/dev/null | grep -c 0003; }

say "0. stop dfrd; -> config 1 (bare SET_CONFIGURATION, same as shipped bounce)"
systemctl stop dfrd; sleep 1
echo 1 > $CFG; sleep 1
echo "   config=$(cat $CFG)"

say "1. FULL stock-style reload (mirror touchbar-relight-reload), --ignore-install"
modprobe -r apple_touchbar 2>/dev/null
modprobe -r apple_ibridge  2>/dev/null
sleep 1
modprobe --ignore-install apple_ibridge  ; sleep 3
# NO idle/dim disable here on purpose: we WANT the firmware's normal ALS loop /
# input-driven full-brightness behavior, exactly like the stock bar.
modprobe --ignore-install apple_touchbar ; sleep 3
echo "   config=$(cat $CFG)  apple-touchbar sub-HIDs=$(subhids)"

say "2. WAIT ~15s in config 1 — WATCH the bar. Does brightness climb to FULL?"
echo "    (tap the bar / cover & uncover the ambient-light sensor near the camera"
echo "     to nudge the ALS loop. Note the countdown second when it changes.)"
for s in 15 12 9 6 3; do echo "   ...${s}s left"; sleep 3; done

say "CHECKPOINT A >>> in CONFIG 1: bar FULL or DIM?  (pausing 5s)"
echo "   config=$(cat $CFG)  sub-HIDs=$(subhids)"
sleep 5

say "3. unload stack -> back to config 2, start dfrd"
modprobe -r apple_touchbar 2>/dev/null
modprobe -r apple_ibridge  2>/dev/null
sleep 1
echo 2 > $CFG; sleep 2
systemctl start dfrd; sleep 3

say "CHECKPOINT B >>> in CONFIG 2 (custom bar): did FULL brightness inherit?"
echo "   config=$(cat $CFG)  dfrd=$(systemctl is-active dfrd)"
echo
echo "Report: when (if) brightness changed during the 15s wait, A (config-1"
echo "full/dim), B (config-2 full/dim)."
