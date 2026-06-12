#!/bin/bash
# Revert the ibridge teardown fix: restore the backed-up apple-ibridge.c
# (which still contains the earlier resume/disp fixes) and rebuild via DKMS.
# Run as root, then reboot.
set -eu
SRC=/usr/src/apple-ib-drv-r307.4afd309/apple-ibridge.c
VER=apple-ib-drv/r307.4afd309
BAK="$SRC.bak-preteardownfix"

[ "$(id -u)" -eq 0 ] || { echo "run as root: sudo $0"; exit 1; }
[ -f "$BAK" ] || { echo "no backup found ($BAK); nothing to revert"; exit 1; }

cp -v "$BAK" "$SRC"
dkms build "$VER" --force
dkms install "$VER" --force
echo "Reverted apple-ibridge.c to pre-teardown-fix state. Reboot to load it."
