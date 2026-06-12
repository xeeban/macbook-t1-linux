#!/bin/bash
# Deploy the WORKING post-hibernate Touch Bar relight: install the reload binary + the post-hibernate
# hook that schedules it detached. Run as root, then test with `sudo systemctl hibernate`.
set -u

REPO=/home/nnishigaya/Code/xeeban/macbook-t1-linux
BIN_SRC="$REPO/touch-bar/touchbar-relight-reload.sbin.sh"
BIN_DST=/usr/local/sbin/touchbar-relight-reload
HOOK_SRC="$REPO/sleep-resume/51-touchbar-relight-hibernate.sh"
HOOK_DST=/usr/lib/systemd/system-sleep/51-touchbar-relight-hibernate.sh

[ "$(id -u)" -eq 0 ] || { echo "run as root: sudo $0"; exit 1; }

echo "=== install reload binary ==="
install -D -m 755 "$BIN_SRC" "$BIN_DST"; echo "installed $BIN_DST"

echo "=== install post-hibernate hook ==="
install -m 755 "$HOOK_SRC" "$HOOK_DST"; echo "installed $HOOK_DST"

echo "=== dry-run: hook must do NOTHING on pre/hibernate and on suspend ==="
"$HOOK_DST" pre hibernate;  echo "pre/hibernate exit=$? (silent+0 correct)"
"$HOOK_DST" post suspend;   echo "post/suspend  exit=$? (silent+0 correct)"

echo "=== sleep dir (expect 50-, 51-, 60-) ==="
ls -la /usr/lib/systemd/system-sleep/

echo
echo "Now test:  sudo systemctl hibernate   (power-button resume; bar should relight ~5-8s after resume)."
echo "Then:      journalctl -b 0 | grep -iE 'touchbar-relight|tb-relight-reload'"
echo "           journalctl -k -b | grep -iE 'tb:|Touchbar' | tail"
