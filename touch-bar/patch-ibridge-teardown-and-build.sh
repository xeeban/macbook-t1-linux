#!/bin/bash
# Patch apple-ibridge.c to fix the teardown GPF: appleib_add_device() indexed
# sub_hdevs[] (2 slots) by the raw collection index (up to 7 on the T1's combined
# display/ALS interface), writing the TB-display hid_device pointer 24 bytes past
# the devm allocation -- corrupting the parent's devres group and crashing EVERY
# iBridge teardown / USB re-enumeration (GPF in remove_nodes). Fix = index by the
# matched sub-device-id slot + NULL/ERR guards. Full writeup:
# IBRIDGE-TEARDOWN-UAF-ANALYSIS.md
#
# SAFE: source patch + dkms rebuild only; no module load/unload, no USB writes.
# Takes effect after REBOOT. Reversible: revert-ibridge-teardown-patch.sh.
#
# Usage:  sudo ./patch-ibridge-teardown-and-build.sh
# Dry-run against a copy (no root, no dkms):
#         SRC=/path/to/copy/apple-ibridge.c NO_DKMS=1 ./patch-ibridge-teardown-and-build.sh
set -eu

SRC="${SRC:-/usr/src/apple-ib-drv-r307.4afd309/apple-ibridge.c}"
VER=apple-ib-drv/r307.4afd309
INSERT="$(dirname "$0")/ibridge-teardown-fix.insert.c"
NO_DKMS="${NO_DKMS:-0}"

if [ "$NO_DKMS" != 1 ]; then
	[ "$(id -u)" -eq 0 ] || { echo "run as root: sudo $0"; exit 1; }
fi
[ -f "$SRC" ] || { echo "source not found: $SRC"; exit 1; }
[ -f "$INSERT" ] || { echo "insert block not found: $INSERT"; exit 1; }

echo "=== backing up / restoring pre-patch source ==="
BAK="$SRC.bak-preteardownfix"
if [ -f "$BAK" ]; then
	echo "backup exists -> restoring it before (re)applying patch"
	cp -v "$BAK" "$SRC"
else
	cp -v "$SRC" "$BAK"
fi

echo "=== applying patch ==="
python3 - "$SRC" "$INSERT" <<'PYEOF'
import sys

src, insert_path = sys.argv[1], sys.argv[2]
text = open(src).read()

if 'Duplicate collection with usage' in text:
    print("unexpectedly already patched (backup restore failed?) -- aborting"); sys.exit(4)

raw_lines = open(insert_path).read().split('\n')
m1 = [i for i, l in enumerate(raw_lines) if 'SECTION 1:' in l and '===8<===' in l]
m2 = [i for i, l in enumerate(raw_lines) if 'SECTION 2:' in l and '===8<===' in l]
if len(m1) != 1 or len(m2) != 1 or m2[0] <= m1[0]:
    print("malformed insert file -- aborting"); sys.exit(5)
sec1 = '\n'.join(raw_lines[m1[0] + 1:m2[0]]).strip('\n')
sec2 = '\n'.join(raw_lines[m2[0] + 1:]).strip('\n')

# --- SECTION 1: replace the whole appleib_add_device() function ---
start_marker = 'static struct appleib_hid_dev_info *appleib_add_device(struct hid_device *hdev)'
end_marker   = 'static void appleib_remove_device(struct hid_device *hdev)'
s = text.find(start_marker)
e = text.find(end_marker)
if s < 0 or e < 0 or e <= s:
    print("SECTION 1 anchors not found -- aborting, no change made"); sys.exit(2)
text = text[:s] + sec1 + '\n\n' + text[e:]

# --- SECTION 2: replace the raw_event forwarding loop ---
old_loop = (
    '\tfor (i = 0; i < ARRAY_SIZE(hdev_info->sub_hdevs); i++) {\n'
    '\t\tif (READ_ONCE(hdev_info->sub_open[i]))\n'
    '\t\t\thid_input_report(hdev_info->sub_hdevs[i], report->type,\n'
    '\t\t\t\t\t data, size, 0);\n'
    '\t}'
)
if old_loop not in text:
    print("SECTION 2 anchor (raw_event loop) not found -- aborting, no change made"); sys.exit(3)
text = text.replace(old_loop, sec2, 1)

open(src, 'w').write(text)
print("patched OK (both sections)")
PYEOF

echo
echo "=== sanity: new code present, old OOB indexing gone ==="
grep -q 'Duplicate collection with usage' "$SRC" || { echo "  PATCH MISSING -- aborting"; exit 3; }
grep -q 'idx = dev_id - appleib_sub_hid_ids' "$SRC" || { echo "  slot indexing missing -- aborting"; exit 3; }
if grep -q 'sub_hdevs\[i\] = appleib_add_sub_dev' "$SRC"; then
	echo "  OLD OOB INDEXING STILL PRESENT -- aborting"; exit 3
fi
echo "  patch present, OOB indexing removed"

if [ "$NO_DKMS" = 1 ]; then
	echo
	echo "=== NO_DKMS=1: skipping dkms (dry-run mode) ==="
	exit 0
fi

echo
echo "=== DKMS rebuild + install (force) ==="
dkms build "$VER" --force
dkms install "$VER" --force

echo
echo "=== DONE. Now: sudo reboot. Then follow MORNING-PLAN.md ==="
echo "  Cold-boot check: bar lit; journal must show NO 'Unknown collection' warnings"
echo "  and NO list_del/GPF. Then live-endpoint power-cycle test BEFORE any hibernate."
