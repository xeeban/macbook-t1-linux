#!/bin/bash
# Step 4 (CORRECTION): systemd-sleep executes EVERY executable file in
# /usr/lib/systemd/system-sleep/ regardless of name -- so the .bak backup of the
# old hook was still running the dangerous unload on hibernate. Move it OUT of the
# directory (and de-exec it) so ONLY the fixed hook remains active.
set -u

SLEEPDIR="/usr/lib/systemd/system-sleep"
OLD="$SLEEPDIR/50-apple-ibridge-touchbar.sh.bak-20260611-164800"
STASH="/home/nnishigaya/Code/xeeban/macbook-t1-linux/sleep-resume/old-hook-pre-fix-20260611.bak"

echo "=== before: executable hooks systemd-sleep will run ==="
ls -la "$SLEEPDIR"

if [ -f "$OLD" ]; then
    echo
    echo "=== moving old backup OUT of the hooks dir ==="
    sudo mv -v "$OLD" "$STASH"
    sudo chmod -x "$STASH" 2>/dev/null || true
else
    echo "(old .bak already gone — nothing to move)"
fi

echo
echo "=== after: hooks systemd-sleep will run (should be ONLY the fixed hook + 60-brcmfmac-wifi.sh) ==="
ls -la "$SLEEPDIR"

echo
echo "=== confirm the remaining apple hook skips hibernate ==="
grep -n 'suspend|suspend-then\|hibernate' "$SLEEPDIR/50-apple-ibridge-touchbar.sh"

echo
echo "Now run ./test-02-hibernate.sh again — THIS time the journal must show NO"
echo "'apple-ibridge-sleep-hook ... before hibernate' line. Then ./test-03-check-resume.sh."
