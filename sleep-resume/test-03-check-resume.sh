#!/bin/bash
# Step 3: after resuming, confirm a clean hibernate cycle with NO deadlock.
set -u

echo "=== hibernate cycle + Touch Bar events this boot ==="
journalctl -b 0 -k 2>/dev/null | grep -iE \
  'hibernation entry|hibernation exit|refusing to freeze|appletb|apple_touchbar|Device or resource busy' \
  | tail -25

echo
echo "=== DANGEROUS old hook must NOT have run on hibernate (should be NOTHING) ==="
journalctl -b 0 2>/dev/null | grep -iE 'apple-ibridge-sleep-hook.*hibernate' | tail -10 \
  || echo "(none — good: the unload/reload hook skipped hibernate)"

echo
echo "=== SAFE relight: scheduling + the transient unit's USB reset (REAL path + 'relight done') ==="
journalctl -b 0 2>/dev/null | grep -iE 'touchbar-relight' | tail -10 \
  || echo "(no relight line — if bar is dark, relight hook may not be deployed)"

echo
echo "=== relight FAILURE flags (want NONE of these) ==="
journalctl -b 0 2>/dev/null | grep -iE "terminated by signal KILL|USB reset  \(|deauthorize failed|reauthorize failed|05ac:8600 not found" | tail -6 \
  || echo "(none — good: not killed mid-reset, path not blank, reset succeeded)"
echo "=== iBridge authorized state now (must be 1, NOT stuck at 0) ==="
for d in /sys/bus/usb/devices/*/; do [ -f "$d/idVendor" ] || continue; \
  if [ "$(cat "$d/idVendor" 2>/dev/null)" = "05ac" ] && [ "$(cat "$d/idProduct" 2>/dev/null)" = "8600" ]; then \
    echo "  iBridge $d authorized=$(cat ${d}authorized 2>/dev/null)"; fi; done

echo
echo "=== apple modules loaded now ==="
lsmod | grep -iE 'apple_touchbar|apple_ibridge' || echo '(touchbar/ibridge NOT loaded -- bar would be dark)'

echo
echo "GOOD = entry->exit clean, NO 'refusing to freeze', NO appletb_remove/D-state,"
echo "       NO 'apple-ibridge-sleep-hook ... hibernate', a 'touchbar-relight' line present,"
echo "       modules loaded, AND Touch Bar LIT automatically on resume."
echo "Report the output (and whether the Touch Bar is lit) back to Claude."
