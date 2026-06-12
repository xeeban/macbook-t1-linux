#!/bin/bash
# Step 1: deploy the edited Touch Bar sleep hook and verify it now SKIPS hibernate.
# Safe / read-only-ish: copies the hook, then dry-runs it with 'pre hibernate'
# (which must do nothing now) and greps the live file to confirm the match list.
set -u

SRC="/home/nnishigaya/Code/xeeban/macbook-t1-linux/sleep-resume/50-apple-ibridge-touchbar.sh"
DST="/usr/lib/systemd/system-sleep/50-apple-ibridge-touchbar.sh"

echo "=== backing up current live hook ==="
sudo cp -v "$DST" "${DST}.bak-20260611-164800"

echo
echo "=== deploying edited hook ==="
sudo cp -v "$SRC" "$DST"
sudo chmod 755 "$DST"

echo
echo "=== dry-run: invoke hook as 'pre hibernate' (must do NOTHING) ==="
sudo "$DST" pre hibernate
echo "exit=$?  (0 with no 'unloading...' line above = correct: hibernate is skipped)"

echo
echo "=== confirm match list no longer contains 'hibernate' ==="
grep -n 'suspend|suspend-then\|hibernate' "$DST"

echo
echo "If you see 'suspend|suspend-then-hibernate|hybrid-sleep' (NO bare 'hibernate'), deploy is good."
echo "Next: run ./test-02-hibernate.sh"
