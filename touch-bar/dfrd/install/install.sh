#!/usr/bin/env bash
# install.sh — make the T1 Touch Bar custom stack PERSIST across reboots.
#
# Idempotent. Re-runnable. Run as root from anywhere:
#   sudo ~/Code/xeeban/macbook-t1-linux/touch-bar/dfrd/install/install.sh
#
# It does NOT load anything live (no insmod / modprobe / udevadm trigger) —
# it stages everything so the NEXT boot comes up in display mode automatically.
# A reboot is required to activate (printed at the end).
#
# Steps (each echoed, each idempotent):
#   1. DKMS-install t1-touchbar-display (appletbdrm + apple_dfr_cfgsel),
#      verify appletbdrm now resolves to updates/dkms (overrides in-tree).
#   2. Patch apple-ib-drv (drop the forced config-1 revert) + rebuild its DKMS.
#   3. Install the persistent udev rule (seat + SYMLINK + SYSTEMD_WANTS).
#   4. Install the four userspace files to /usr/local/bin.
#   5. Install + enable dfrd.service.
#   6. Swap in the config-aware hibernate relight hook (no-ops in config 2).
#
# See PERSISTENCE.md for the full ordered runbook and post-reboot verification.

set -euo pipefail

# --- locate the repo (this script lives in .../touch-bar/dfrd/install/) -------
INSTALL_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DFRD_DIR="$(cd "$INSTALL_DIR/.." && pwd)"
TB_DIR="$(cd "$DFRD_DIR/.." && pwd)"           # touch-bar/
KERNEL_DIR="$TB_DIR/kernel"
KMOD_SRC="$KERNEL_DIR/t1-touchbar-display"
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

for f in "$KMOD_SRC/dkms.conf" "$IBRIDGE_PATCH" \
         "$DFRD_DIR/dfr-render" "$DFRD_DIR/dfr-touchd" "$DFRD_DIR/dfr-fnd" \
         "$DFRD_DIR/dfrd-run.sh" "$INSTALL_DIR/99-touchbar-dfr.rules" \
         "$INSTALL_DIR/dfrd.service"; do
    [ -e "$f" ] || { echo "ERROR: missing required file: $f" >&2; exit 1; }
done

# Warn (don't fail) if binaries weren't freshly built.
if [ "$DFRD_DIR/dfr-render.c" -nt "$DFRD_DIR/dfr-render" ] 2>/dev/null; then
    echo "WARNING: dfr-render.c is newer than the dfr-render binary." >&2
    echo "         Run 'make' in $DFRD_DIR before installing for the latest build." >&2
fi

# ----------------------------------------------------------------------------
step "1/5  DKMS install: $KMOD_NAME/$KMOD_VER (appletbdrm + apple_dfr_cfgsel)"

if [ ! -d "$KMOD_USRC" ]; then
    echo "    copy $KMOD_SRC -> $KMOD_USRC"
    cp -r "$KMOD_SRC" "$KMOD_USRC"
    # strip any stray build artifacts so DKMS builds clean
    make -C "$KMOD_USRC" clean >/dev/null 2>&1 || true
else
    echo "    $KMOD_USRC already present — refreshing sources in place"
    cp -rf "$KMOD_SRC/." "$KMOD_USRC/"
    make -C "$KMOD_USRC" clean >/dev/null 2>&1 || true
fi

if ! dkms status "$KMOD_NAME/$KMOD_VER" 2>/dev/null | grep -q .; then
    echo "    dkms add $KMOD_NAME/$KMOD_VER"
    dkms add "$KMOD_NAME/$KMOD_VER"
else
    echo "    dkms tree already has $KMOD_NAME/$KMOD_VER — skipping add"
fi

echo "    dkms build $KMOD_NAME/$KMOD_VER"
dkms build "$KMOD_NAME/$KMOD_VER" --force
echo "    dkms install $KMOD_NAME/$KMOD_VER"
dkms install "$KMOD_NAME/$KMOD_VER" --force

echo "    verifying appletbdrm now resolves to updates/dkms..."
abd="$(modinfo -F filename appletbdrm 2>/dev/null || true)"
echo "      modinfo appletbdrm -> ${abd:-<none>}"
case "$abd" in
    */updates/dkms/*) echo "      OK: DKMS build overrides the in-tree appletbdrm" ;;
    *) echo "      WARNING: appletbdrm does NOT point at updates/dkms yet." >&2
       echo "               depmod runs on the DKMS install; if this persists after" >&2
       echo "               reboot, run 'sudo depmod -a' and re-check." >&2 ;;
esac

# ----------------------------------------------------------------------------
step "2/5  Patch apple-ibridge (remove forced config-1 revert) + rebuild DKMS"

if [ ! -d "$IBRIDGE_USRC" ]; then
    echo "ERROR: $IBRIDGE_USRC not found — apple-ib-drv DKMS not installed?" >&2
    echo "       Expected the upstream apple-ib-drv-$IBRIDGE_VER DKMS source tree." >&2
    exit 1
fi

# Idempotency: forward-apply cleanly => NOT yet applied; reverse-apply
# cleanly => already applied. Use --dry-run to decide, then apply for real.
if patch -d "$IBRIDGE_USRC" -p1 -R --dry-run --force \
        < "$IBRIDGE_PATCH" >/dev/null 2>&1; then
    echo "    apple-ibridge patch already applied — skipping patch step"
    PATCH_NEEDED=0
elif patch -d "$IBRIDGE_USRC" -p1 --dry-run --force \
        < "$IBRIDGE_PATCH" >/dev/null 2>&1; then
    echo "    applying apple-ibridge patch to $IBRIDGE_USRC"
    patch -d "$IBRIDGE_USRC" -p1 --force < "$IBRIDGE_PATCH"
    PATCH_NEEDED=1
else
    echo "ERROR: apple-ibridge patch neither applies cleanly nor is already" >&2
    echo "       applied. The DKMS source may have changed version. Inspect:" >&2
    echo "         $IBRIDGE_USRC/apple-ibridge.c (around line 541)" >&2
    echo "         $IBRIDGE_PATCH" >&2
    exit 1
fi

# Always rebuild/reinstall so the running depmod picks up the patched module,
# even if a prior partial run left it half-done.
echo "    dkms build $IBRIDGE_MOD/$IBRIDGE_VER --force"
dkms build "$IBRIDGE_MOD/$IBRIDGE_VER" --force
echo "    dkms install $IBRIDGE_MOD/$IBRIDGE_VER --force"
dkms install "$IBRIDGE_MOD/$IBRIDGE_VER" --force
[ "${PATCH_NEEDED:-0}" = "1" ] && echo "    (patched apple-ibridge will now decline in config 2)"

# ----------------------------------------------------------------------------
step "3/5  Install persistent udev rule (seat + SYMLINK + SYSTEMD_WANTS)"

install -m0644 "$INSTALL_DIR/99-touchbar-dfr.rules" /etc/udev/rules.d/99-touchbar-dfr.rules
echo "    -> /etc/udev/rules.d/99-touchbar-dfr.rules"
# Blacklist the firmware-mode HID drivers: in display mode they claim the
# config-2 digitizer interface and suppress its /dev/hidraw node, so dfr-touchd
# can't read taps (the bar restart-loops/flickers). hid-generic must own it.
install -m0644 "$INSTALL_DIR/dfrd-blacklist.conf" /etc/modprobe.d/dfrd-blacklist.conf
echo "    -> /etc/modprobe.d/dfrd-blacklist.conf (block apple_ibridge/apple_touchbar)"
echo "    udevadm control --reload"
udevadm control --reload
# Deliberately NOT triggering live: the card is already up under the OLD rule
# this session; the new SYMLINK/SYSTEMD_WANTS take effect cleanly on the next
# boot's fresh card-add. (Re-triggering live could yank the card from a running
# desktop session.) See PERSISTENCE.md if you want to activate without reboot.

# ----------------------------------------------------------------------------
step "4/5  Install userspace binaries + runner to /usr/local/bin"

install -m0755 "$DFRD_DIR/dfr-render"  /usr/local/bin/dfr-render
install -m0755 "$DFRD_DIR/dfr-touchd"  /usr/local/bin/dfr-touchd
install -m0755 "$DFRD_DIR/dfr-fnd"     /usr/local/bin/dfr-fnd
install -m0755 "$DFRD_DIR/dfrd-run.sh" /usr/local/bin/dfrd-run.sh
install -m0755 "$INSTALL_DIR/dfrd-ensure-config2.sh" /usr/local/bin/dfrd-ensure-config2.sh
echo "    -> /usr/local/bin/{dfr-render,dfr-touchd,dfr-fnd,dfrd-run.sh,dfrd-ensure-config2.sh}"
echo "    (dfrd-run.sh resolves siblings by its own dir, so all four co-locate)"

# ----------------------------------------------------------------------------
step "5/5  Install + enable dfrd.service"

install -m0644 "$INSTALL_DIR/dfrd.service"        /etc/systemd/system/dfrd.service
install -m0644 "$INSTALL_DIR/dfrd-cfgsel.service" /etc/systemd/system/dfrd-cfgsel.service
echo "    -> /etc/systemd/system/{dfrd.service,dfrd-cfgsel.service}"
echo "    systemctl daemon-reload"
systemctl daemon-reload
echo "    systemctl enable dfrd-cfgsel.service (ensures config 2 at boot)"
systemctl enable dfrd-cfgsel.service
echo "    systemctl enable dfrd.service (belt-and-suspenders; SYSTEMD_WANTS is primary)"
systemctl enable dfrd.service

# Clean up superseded draft units if a previous attempt installed them.
for old in dfr-render.service dfr-touchd.service; do
    if [ -e "/etc/systemd/system/$old" ]; then
        echo "    removing superseded unit /etc/systemd/system/$old"
        systemctl disable "$old" >/dev/null 2>&1 || true
        rm -f "/etc/systemd/system/$old"
    fi
done
systemctl daemon-reload

# ----------------------------------------------------------------------------
step "6/6  Install config-aware hibernate relight hook"

SLEEP_DIR="/usr/lib/systemd/system-sleep"
HOOK="51-touchbar-relight-hibernate.sh"
if [ -d "$SLEEP_DIR" ]; then
    # Back up the existing (stock-bar-only) hook once, so uninstall can restore it.
    if [ -e "$SLEEP_DIR/$HOOK" ] && [ ! -e "$SLEEP_DIR/$HOOK.pre-dfrd" ]; then
        cp -p "$SLEEP_DIR/$HOOK" "$SLEEP_DIR/$HOOK.pre-dfrd"
        echo "    backed up existing hook -> $SLEEP_DIR/$HOOK.pre-dfrd"
    fi
    install -m0755 "$INSTALL_DIR/$HOOK" "$SLEEP_DIR/$HOOK"
    echo "    -> $SLEEP_DIR/$HOOK (config-aware: skips apple_ibridge reload in config 2)"
    echo "    note: depends on /usr/local/sbin/touchbar-relight-reload for the"
    echo "          config-1 (stock-bar) fallback path; that script is unchanged."
else
    echo "    WARNING: $SLEEP_DIR not present — skipping hibernate hook." >&2
fi

# ----------------------------------------------------------------------------
echo
echo "============================================================"
echo " INSTALL COMPLETE."
echo
echo " Everything is staged. REBOOT to activate the persistent stack:"
echo
echo "     sudo reboot"
echo
echo " After reboot, verify (see PERSISTENCE.md for the full list):"
echo "   cat /sys/bus/usb/devices/1-3/bConfigurationValue     # expect: 2"
echo "   ls -l /dev/dri/touchbar                              # symlink exists"
echo "   systemctl status dfrd.service                        # active (running)"
echo "   modinfo -F filename appletbdrm | grep updates/dkms   # DKMS override"
echo
echo " The Touch Bar should show the 'media' strip; HOLD Fn for F-keys."
echo "============================================================"
