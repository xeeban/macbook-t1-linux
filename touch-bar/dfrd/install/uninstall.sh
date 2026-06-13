#!/usr/bin/env bash
# uninstall.sh — reverse install.sh, returning to the STOCK firmware Touch Bar.
#
# Idempotent. Run as root:
#   sudo ~/Code/xeeban/macbook-t1-linux/touch-bar/dfrd/install/uninstall.sh
#
# Reverses, in safe order:
#   1. Stop + disable + remove dfrd.service (and any superseded draft units).
#   2. Remove the userspace binaries from /usr/local/bin.
#   3. Remove the udev rule + reload.
#   4. dkms remove t1-touchbar-display + drop its /usr/src tree.
#   5. Reverse the apple-ibridge patch + rebuild/reinstall its DKMS (so
#      apple_ibridge forces config 1 again = stock simple-mode Touch Bar).
#
# Like install.sh, this does NOT live-unload modules — it stages stock for the
# NEXT boot. Reboot to land on the stock Touch Bar. (Printed at the end.)

set -uo pipefail   # not -e: we want best-effort teardown to continue past gaps

INSTALL_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DFRD_DIR="$(cd "$INSTALL_DIR/.." && pwd)"
TB_DIR="$(cd "$DFRD_DIR/.." && pwd)"
KERNEL_DIR="$TB_DIR/kernel"
IBRIDGE_PATCH="$KERNEL_DIR/patches/apple-ibridge-no-config1-revert.patch"

KMOD_NAME="t1-touchbar-display"
KMOD_VER="1.0"
KMOD_USRC="/usr/src/${KMOD_NAME}-${KMOD_VER}"

IBRIDGE_MOD="apple-ib-drv"
IBRIDGE_VER="r307.4afd309"
IBRIDGE_USRC="/usr/src/${IBRIDGE_MOD}-${IBRIDGE_VER}"

step() { echo; echo "==> $*"; }

if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: must run as root (sudo $0)" >&2
    exit 1
fi

# ----------------------------------------------------------------------------
step "1/5  Stop + remove systemd unit(s)"
for u in dfrd.service dfr-render.service dfr-touchd.service; do
    if systemctl list-unit-files "$u" >/dev/null 2>&1 && \
       [ -e "/etc/systemd/system/$u" ]; then
        echo "    stop/disable/remove $u"
        systemctl stop "$u"    >/dev/null 2>&1 || true
        systemctl disable "$u" >/dev/null 2>&1 || true
        rm -f "/etc/systemd/system/$u"
    fi
done
systemctl daemon-reload

# ----------------------------------------------------------------------------
step "2/5  Remove userspace binaries"
rm -f /usr/local/bin/dfr-render /usr/local/bin/dfr-touchd \
      /usr/local/bin/dfr-fnd    /usr/local/bin/dfrd-run.sh
echo "    removed /usr/local/bin/{dfr-render,dfr-touchd,dfr-fnd,dfrd-run.sh}"

# ----------------------------------------------------------------------------
step "3/5  Remove udev rule + module blacklist"
rm -f /etc/udev/rules.d/99-touchbar-dfr.rules
echo "    removed /etc/udev/rules.d/99-touchbar-dfr.rules"
rm -f /etc/modprobe.d/dfrd-blacklist.conf
echo "    removed /etc/modprobe.d/dfrd-blacklist.conf (apple_ibridge/apple_touchbar load again)"
udevadm control --reload

# ----------------------------------------------------------------------------
step "4/5  DKMS remove $KMOD_NAME (appletbdrm + apple_dfr_cfgsel)"
if dkms status "$KMOD_NAME/$KMOD_VER" 2>/dev/null | grep -q .; then
    dkms remove "$KMOD_NAME/$KMOD_VER" --all || true
else
    echo "    $KMOD_NAME/$KMOD_VER not in DKMS tree — skipping"
fi
rm -rf "$KMOD_USRC"
echo "    removed $KMOD_USRC"
echo "    depmod -a (so the in-tree appletbdrm resolves again)"
depmod -a || true

# ----------------------------------------------------------------------------
step "5/5  Reverse apple-ibridge patch + rebuild its DKMS"
if [ -d "$IBRIDGE_USRC" ]; then
    if patch -d "$IBRIDGE_USRC" -p1 -R --dry-run --force \
            < "$IBRIDGE_PATCH" >/dev/null 2>&1; then
        echo "    reversing apple-ibridge patch in $IBRIDGE_USRC"
        patch -d "$IBRIDGE_USRC" -p1 -R --force < "$IBRIDGE_PATCH" || true
    else
        echo "    apple-ibridge patch not applied (or already reversed) — skipping"
    fi
    echo "    dkms build/install $IBRIDGE_MOD/$IBRIDGE_VER --force (restore stock behavior)"
    dkms build   "$IBRIDGE_MOD/$IBRIDGE_VER" --force || true
    dkms install "$IBRIDGE_MOD/$IBRIDGE_VER" --force || true
else
    echo "    $IBRIDGE_USRC not present — skipping"
fi

# ----------------------------------------------------------------------------
step "6/6  Restore stock hibernate relight hook"
SLEEP_DIR="/usr/lib/systemd/system-sleep"
HOOK="51-touchbar-relight-hibernate.sh"
if [ -e "$SLEEP_DIR/$HOOK.pre-dfrd" ]; then
    mv -f "$SLEEP_DIR/$HOOK.pre-dfrd" "$SLEEP_DIR/$HOOK"
    echo "    restored original $SLEEP_DIR/$HOOK from .pre-dfrd backup"
else
    echo "    no .pre-dfrd backup found — leaving $SLEEP_DIR/$HOOK as-is"
    echo "    (the config-aware hook falls back to the stock reload in config 1,"
    echo "     so the stock bar still relights after reboot even if left in place)"
fi

echo
echo "============================================================"
echo " UNINSTALL COMPLETE — staged back to stock."
echo
echo "   sudo reboot      # to land on the stock firmware Touch Bar"
echo
echo " After reboot the stock bar should show ESC/F-keys/media via"
echo " apple_ibridge + apple_touchbar (config 1)."
echo "============================================================"
