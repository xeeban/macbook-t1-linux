#!/bin/bash
# Revert the set_tb_disp direct-USB patch: restore the backed-up apple-touchbar.c
# and rebuild/install the stock module. Run as root, then reboot.
set -eu
SRC=/usr/src/apple-ib-drv-r307.4afd309/apple-touchbar.c
VER=apple-ib-drv/r307.4afd309
BAK="$SRC.bak-predispfix"

[ "$(id -u)" -eq 0 ] || { echo "run as root: sudo $0"; exit 1; }
[ -f "$BAK" ] || { echo "no backup found ($BAK); nothing to revert"; exit 1; }

cp -v "$BAK" "$SRC"
dkms build "$VER" --force
dkms install "$VER" --force
echo "Reverted to stock apple-touchbar.c. Reboot to load it."
