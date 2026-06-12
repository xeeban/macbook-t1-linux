#!/usr/bin/env bash
# dfr-debug.sh — figure out WHY `echo 2 > bConfigurationValue` doesn't stick.
# RUN AS ROOT. Always restores config 1. Captures kernel (dmesg) reaction to each
# attempt and tries three escalating methods.
set -u
DEV=/sys/bus/usb/devices/1-3
[ "$(id -u)" -eq 0 ] || { echo "run as root"; exit 1; }

show() { echo "   config=$(cat $DEV/bConfigurationValue 2>/dev/null)"; }
bound() {
	echo "   interface drivers:"
	for i in "$DEV":*; do [ -e "$i" ] || continue
		echo "     $(basename "$i") cls=$(cat "$i/bInterfaceClass" 2>/dev/null) -> $(basename "$(readlink "$i/driver" 2>/dev/null)" 2>/dev/null)"
	done
}
dmesg_since() { dmesg | tail -n "${1:-15}"; }

restore() {
	echo "== RESTORE -> config 1"
	echo 1 > "$DEV/bConfigurationValue" 2>/dev/null
	sleep 1; show
	[ "$(cat $DEV/bConfigurationValue)" = 1 ] || echo "   !! not 1 — if bar dark: sudo /usr/local/sbin/touchbar-relight-reload"
}
trap restore EXIT INT TERM

echo "######## BASELINE ########"
echo "device-level driver: $(basename "$(readlink $DEV/driver 2>/dev/null)" 2>/dev/null)"
echo "bNumConfigurations=$(cat $DEV/bNumConfigurations) bDeviceClass=$(cat $DEV/bDeviceClass)"
show; bound

echo; echo "######## ATTEMPT 1: plain 'echo 2', watch dmesg ########"
dmesg -C 2>/dev/null
echo 2 > "$DEV/bConfigurationValue"; echo "   write rc=$?"
sleep 1; show
echo "   --- dmesg ---"; dmesg_since 20

echo; echo "######## ATTEMPT 2: unconfigure (0) then 2 ########"
dmesg -C 2>/dev/null
echo 0 > "$DEV/bConfigurationValue"; echo "   set-0 rc=$?"; sleep 1; show
echo 2 > "$DEV/bConfigurationValue"; echo "   set-2 rc=$?"; sleep 1; show
echo "   --- dmesg ---"; dmesg_since 25

if [ "$(cat $DEV/bConfigurationValue)" != 2 ]; then
echo; echo "######## ATTEMPT 3: unbind all interface drivers, then 2 ########"
dmesg -C 2>/dev/null
for i in "$DEV":*; do [ -e "$i" ] || continue
	drv="$(basename "$(readlink "$i/driver" 2>/dev/null)" 2>/dev/null)"
	[ -n "$drv" ] || continue
	echo "   unbind $(basename "$i") from $drv"
	echo "$(basename "$i")" > "/sys/bus/usb/drivers/$drv/unbind" 2>/dev/null || echo "     unbind failed"
done
sleep 1; bound
echo 2 > "$DEV/bConfigurationValue"; echo "   set-2 rc=$?"; sleep 1; show
echo "   --- dmesg ---"; dmesg_since 25
fi

echo; echo "######## RESULT ########"
FINAL="$(cat $DEV/bConfigurationValue)"
echo "final config before restore: $FINAL"
if [ "$FINAL" = 2 ]; then
	echo ">> REACHED CONFIG 2. Interfaces now:"; bound
fi
