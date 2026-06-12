#!/usr/bin/env bash
# dfr-spike.sh — Phase-2 spike runner (RUN AS ROOT).
#
# Switches the T1 iBridge (05ac:8600) to USB config 2 (the macOS "display mode"
# config that exposes the pixel-streaming interface), runs ./dfr-spike for the
# requested stage, then ALWAYS restores config 1 so the Touch Bar returns to its
# normal firmware "simple mode" (your Esc key).
#
# Switching config tears down apple_ibridge — the exact path the #7 OOB fix made
# safe — so this also real-world-tests that fix. The keyboard is on SPI (applespi),
# NOT the iBridge, so it is unaffected. The webcam (uvcvideo) briefly re-enumerates.
#
# Usage:  sudo ./dfr-spike.sh probe        # safest: just switch config + confirm interface 3 claimable
#         sudo ./dfr-spike.sh info         # send GINF, print geometry  (proves the protocol, no pixels)
#         sudo ./dfr-spike.sh clear        # blank the panel
#         sudo ./dfr-spike.sh frame        # paint whole bar magenta
#         sudo ./dfr-spike.sh frame 00FF00 # paint whole bar green (RRGGBB)
set -u

DEV=/sys/bus/usb/devices/1-3
BIN="$(dirname "$(readlink -f "$0")")/dfr-spike"
STAGE="${1:-probe}"
COLOR="${2:-}"

[ "$(id -u)" -eq 0 ] || { echo "must run as root (sudo)"; exit 1; }
[ -e "$DEV/bConfigurationValue" ] || { echo "iBridge not at $DEV — is it 1-3?"; exit 1; }
[ -x "$BIN" ] || { echo "binary $BIN not built. Run: gcc -O2 -Wall -o dfr-spike dfr-spike.c -lusb-1.0"; exit 1; }

ORIG="$(cat "$DEV/bConfigurationValue")"
echo "== saved current config: $ORIG"

restore() {
	echo "== restoring config $ORIG"
	echo "$ORIG" > "$DEV/bConfigurationValue" 2>/dev/null || echo "  (restore write failed — see below)"
	sleep 1
	local now; now="$(cat "$DEV/bConfigurationValue" 2>/dev/null)"
	echo "== config now: $now"
	if [ "$now" != "1" ]; then
		echo "!! NOT back in config 1. If the Touch Bar is dark, recover with:"
		echo "     echo 1 | sudo tee $DEV/bConfigurationValue"
		echo "     sudo /usr/local/sbin/touchbar-relight-reload   # if still dark"
	fi
}
trap restore EXIT INT TERM

echo "== switching to config 2"
if ! echo 2 > "$DEV/bConfigurationValue" 2>/dev/null; then
	echo "!! kernel refused config 2 (device may NAK config change). Aborting; will restore."
	exit 1
fi
sleep 1
NOWCFG="$(cat "$DEV/bConfigurationValue")"
echo "== config now: $NOWCFG"
[ "$NOWCFG" = "2" ] || { echo "!! did not reach config 2 (got $NOWCFG). Aborting."; exit 1; }

if [ "$STAGE" = "probe" ]; then
	echo "== config-2 interfaces now present:"
	for i in "$DEV":*; do
		[ -e "$i" ] || continue
		echo "   $(basename "$i"): class=$(cat "$i/bInterfaceClass" 2>/dev/null) " \
		     "sub=$(cat "$i/bInterfaceSubClass" 2>/dev/null) " \
		     "driver=$(basename "$(readlink "$i/driver" 2>/dev/null)" 2>/dev/null)"
	done
fi

echo "== running: dfr-spike $STAGE $COLOR"
"$BIN" "$STAGE" $COLOR
RC=$?
echo "== dfr-spike exit: $RC"
exit $RC
