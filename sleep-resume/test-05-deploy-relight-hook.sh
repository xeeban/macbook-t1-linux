#!/bin/bash
# Step 5: install the standalone relight binary + deploy the post-hibernate hook
# that schedules it (via systemd-run, +15s, detached). Verify the hook does NOTHING
# on pre/hibernate and on the s2idle family.
set -u

DIR="/home/nnishigaya/Code/xeeban/macbook-t1-linux/sleep-resume"
HOOK_SRC="$DIR/51-touchbar-relight-hibernate.sh"
HOOK_DST="/usr/lib/systemd/system-sleep/51-touchbar-relight-hibernate.sh"
BIN_SRC="$DIR/touchbar-relight.sbin.sh"
BIN_DST="/usr/local/sbin/touchbar-relight"

echo "=== installing standalone relight binary ==="
sudo install -D -m 755 "$BIN_SRC" "$BIN_DST"
echo "installed $BIN_DST"

echo
echo "=== deploying relight hook ==="
sudo cp -v "$HOOK_SRC" "$HOOK_DST"
sudo chmod 755 "$HOOK_DST"

echo
echo "=== dry-run: 'pre hibernate' must do NOTHING (no scheduling before a freeze) ==="
sudo "$HOOK_DST" pre hibernate; echo "pre/hibernate exit=$? (silent + 0 = correct)"

echo
echo "=== dry-run: 'post suspend' must do NOTHING (50- hook owns the s2idle family) ==="
sudo "$HOOK_DST" post suspend; echo "post/suspend exit=$? (silent + 0 = correct)"

echo
echo "=== sanity: run the standalone relight binary by hand (should reset + relight now) ==="
echo "    (skipping auto-run; run it yourself if you want: sudo $BIN_DST)"

echo
echo "=== hooks in sleep dir (expect 50-, 51-, 60- ; NO .bak) ==="
ls -la /usr/lib/systemd/system-sleep/
echo "=== relight binary present? ==="
ls -la "$BIN_DST"

echo
echo "Deploy good if both dry-runs were silent. Next: ./test-02-hibernate.sh then ./test-03-check-resume.sh"
echo "On resume, ~15s later a transient unit 'touchbar-relight.service' runs the USB reset and lights the bar."
echo "test-03 should show 'touchbar-relight: USB reset /sys/bus/usb/devices/.../' with a REAL path + 'relight done'."
