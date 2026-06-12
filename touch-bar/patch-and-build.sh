#!/bin/bash
# Patch apple-touchbar.c so set_tb_disp() sends the display report via a DIRECT
# usb_control_msg (like set_tb_mode) instead of hid_hw_request() through the
# iBridge's usbhid queue -- which goes stale across hibernate and silently drops
# the SET_REPORT, leaving the Touch Bar dark on resume. Then rebuild + install via DKMS.
#
# SAFE: no USB teardown anywhere; worst case is the bar still dark. Reversible
# (backs up the original; see revert-driver-patch.sh). Run as root, then REBOOT to
# load the new module, then hibernate-test (no sleep hook needed -- the driver
# relights itself on resume via reset_resume -> worker -> set_tb_disp).
set -eu

SRC=/usr/src/apple-ib-drv-r307.4afd309/apple-touchbar.c
VER=apple-ib-drv/r307.4afd309
INSERT="$(dirname "$0")/disp-direct-usb.insert.c"
ANCHOR='hid_hw_request(tb_dev->disp_iface.hdev, report, HID_REQ_SET_REPORT);'

[ "$(id -u)" -eq 0 ] || { echo "run as root: sudo $0"; exit 1; }
[ -f "$SRC" ] || { echo "source not found: $SRC"; exit 1; }
[ -f "$INSERT" ] || { echo "insert block not found: $INSERT"; exit 1; }

echo "=== backing up / restoring stock source ==="
BAK="$SRC.bak-predispfix"
if [ -f "$BAK" ]; then
	echo "backup exists -> restoring stock source before (re)applying patch"
	cp -v "$BAK" "$SRC"
else
	cp -v "$SRC" "$BAK"
fi

echo "=== applying patch (replace the single hid_hw_request disp line) ==="
python3 - "$SRC" "$INSERT" "$ANCHOR" <<'PYEOF'
import sys
src, insert_path, anchor = sys.argv[1], sys.argv[2], sys.argv[3]
text = open(src).read()
if 'set touch bar display to' in text:
    print("unexpectedly already patched (backup restore failed?) -- aborting"); sys.exit(4)
block = open(insert_path).read().rstrip('\n')
lines = text.split('\n')
out, done = [], False
for ln in lines:
    if (not done) and (anchor in ln) and ('else' not in ln) and ('tb_dev->is_t1' not in ln):
        out.append(block)        # replace the whole anchor line with the conditional block
        done = True
    else:
        out.append(ln)
if not done:
    print("ANCHOR NOT FOUND -- aborting, no change made"); sys.exit(2)
open(src, 'w').write('\n'.join(out))
print("patched OK")
PYEOF

echo
echo "=== sanity: the new direct-USB path is present ==="
grep -q 'set touch bar display to' "$SRC" && echo "  patch present" || { echo "  PATCH MISSING -- aborting"; exit 3; }

echo
echo "=== DKMS rebuild + install (force) ==="
dkms build "$VER" --force
dkms install "$VER" --force

echo
echo "=== DONE. Now: sudo reboot, then hibernate-test ==="
echo "  After reboot (bar lit), just: sudo systemctl hibernate  (power button to resume)."
echo "  On resume the driver should relight the bar itself -- NO sleep hook involved."
echo "  Check: journalctl -k -b | grep -iE 'touch bar display|Touchbar'  (a 'Failed to set"
echo "  touch bar display' line would mean the report format needs adjusting -- still safe, just dark)."
