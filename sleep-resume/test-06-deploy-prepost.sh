#!/bin/bash
# Deploy the SAFE pre/post-hibernate Touch Bar relight hook (unbind=pre/live, bind=post/deadlock-free),
# and REMOVE the old deadlocking deauthorize binary so nothing can invoke it.
set -u

DIR="/home/nnishigaya/Code/xeeban/macbook-t1-linux/sleep-resume"
HOOK_SRC="$DIR/51-touchbar-relight-hibernate.sh"
HOOK_DST="/usr/lib/systemd/system-sleep/51-touchbar-relight-hibernate.sh"

echo "=== removing old deadlocking deauthorize binary (if present) ==="
sudo rm -fv /usr/local/sbin/touchbar-relight

echo
echo "=== deploying SAFE pre/post hook ==="
sudo cp -v "$HOOK_SRC" "$HOOK_DST"
sudo chmod 755 "$HOOK_DST"

echo
echo "=== dry-run: 'pre suspend' / 'post suspend' must do NOTHING (we only touch hibernate) ==="
sudo "$HOOK_DST" pre suspend;  echo "pre/suspend  exit=$? (silent+0 correct)"
sudo "$HOOK_DST" post suspend; echo "post/suspend exit=$? (silent+0 correct)"

echo
echo "=== sleep dir (expect 50-, 51-, 60- ; NO .bak, NO /usr/local/sbin/touchbar-relight) ==="
ls -la /usr/lib/systemd/system-sleep/
ls -la /usr/local/sbin/touchbar-relight 2>/dev/null || echo "  (good: /usr/local/sbin/touchbar-relight removed)"

echo
echo "Next: sudo ./test-02-hibernate.sh  (type go, power on, watch the bar) then sudo ./test-03-check-resume.sh"
echo "SAFE BY CONSTRUCTION: unbind happens pre-hibernate on a LIVE endpoint; post-resume only BINDs"
echo "(bind never calls appletb_remove -> cannot deadlock). Worst case is a dark bar, never a hang."
