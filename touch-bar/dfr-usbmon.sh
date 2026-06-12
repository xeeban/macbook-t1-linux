#!/usr/bin/env bash
# dfr-usbmon.sh — capture the USB wire while dfr-switch tries to enter config 2,
# so we can see whether the device STALLs SET_CONFIGURATION(2) or ACKs-then-resets.
# RUN AS ROOT.  Bus 1 (the iBridge is 1-3).
set -u
HERE="$(dirname "$(readlink -f "$0")")"
[ "$(id -u)" -eq 0 ] || { echo "run as root"; exit 1; }
modprobe usbmon 2>/dev/null
MON=/sys/kernel/debug/usb/usbmon/1u
[ -r "$MON" ] || { echo "usbmon bus-1 node not readable ($MON). Is debugfs mounted?"; exit 1; }
OUT=/tmp/dfr-usbmon.txt

dmesg -C 2>/dev/null
echo "== capturing $MON ..."
timeout 15 cat "$MON" > "$OUT" &
CAT=$!
sleep 0.5

"$HERE/dfr-switch" info
sleep 0.5
kill "$CAT" 2>/dev/null; wait "$CAT" 2>/dev/null

echo
echo "############ SET_CONFIGURATION transactions (setup byte pattern '00 09') ############"
# usbmon setup fields: 's bmRequestType bRequest wValue wIndex wLength'
grep -nE ' s 00 09 ' "$OUT" || echo "  (none seen — SET_CONFIGURATION may not have been issued on bus 1)"
echo
echo "############ all control transfers (Co/Ci) with their completion status ############"
grep -E ':1:0(0[0-9]|[0-9][0-9]):0' "$OUT" | grep -E ' [CS][io]:' | head -60
echo
echo "############ lines mentioning errors/stall (-32 EPIPE, status != 0) ############"
grep -nE '\-32|\-71|\-110|stall|EPIPE' "$OUT" | head -20
echo
echo "full capture: $OUT  ($(wc -l <"$OUT") lines)"
echo "(if you want, send me the whole file or the first ~120 lines)"
