#!/usr/bin/env bash
# dfrd-ensure-config2.sh — guarantee the T1 iBridge is in display mode (config 2)
# at boot, working around apple_dfr_cfgsel's load-ordering race.
#
# At cold boot the iBridge enumerates and the generic USB driver configures it
# BEFORE apple_dfr_cfgsel autoloads. When cfgsel then re-probes the already-
# configured device it performs a *live* config switch (unconfigure -> set 2),
# which the firmware sometimes answers by dropping to an unconfigured state ->
# no appletbdrm card -> dfrd.service's device dependency never fires -> dark bar.
#
# A *fresh* enumeration with cfgsel already loaded is reliable (choose_config
# picks config 2 from the start). So: make sure the modules are loaded, then if
# the device isn't in config 2, re-enumerate it once via the authorized toggle.
#
# Installed to /usr/local/bin and run by dfrd-cfgsel.service at boot/resume.
set -u
DEV=/sys/bus/usb/devices/1-3

# make sure the selector + DRM driver are present before we re-enumerate
modprobe apple_dfr_cfgsel appletbdrm 2>/dev/null || true

[ -e "$DEV/bConfigurationValue" ] || exit 0          # iBridge not present

cfg() { cat "$DEV/bConfigurationValue" 2>/dev/null || echo ""; }

# wait briefly in case cfgsel's own (racy) attempt is mid-flight
for _ in $(seq 1 6); do [ "$(cfg)" = "2" ] && exit 0; sleep 0.5; done

echo "dfrd-ensure-config2: config='$(cfg)' (not 2) — re-enumerating 1-3" >&2
echo 0 > "$DEV/authorized" 2>/dev/null || true
sleep 1
echo 1 > "$DEV/authorized" 2>/dev/null || true

# wait up to ~12 s for config 2 to stick (cfgsel chooses it on the fresh enum)
for _ in $(seq 1 24); do [ "$(cfg)" = "2" ] && break; sleep 0.5; done

if [ "$(cfg)" = "2" ]; then
	echo "dfrd-ensure-config2: now in config 2" >&2
	exit 0
fi
echo "dfrd-ensure-config2: FAILED to reach config 2 (still '$(cfg)')" >&2
exit 1
