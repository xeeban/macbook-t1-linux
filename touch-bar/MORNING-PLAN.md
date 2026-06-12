# MORNING PLAN — apply + test the iBridge teardown fix (2026-06-12)

**TL;DR from overnight:** The teardown GPF is a **heap out-of-bounds write planted at every
boot** in `appleib_add_device()` — `sub_hdevs[]` (2 slots) indexed by raw collection index (the
TB-display collection is index **6** of 7 on the combined display/ALS interface). The planted
`hid_device*` corrupts the parent's devres group; **every** teardown then GPFs in
`remove_nodes()`. Proof: the crash registers are byte-for-byte the first 16 bytes of the live
report descriptor. Full writeup: [`IBRIDGE-TEARDOWN-UAF-ANALYSIS.md`](IBRIDGE-TEARDOWN-UAF-ANALYSIS.md).
Fix = index by matched id slot. **The patched source already test-compiled clean overnight**
against `7.0.10-arch1-1` headers (both `.ko`s built, no warnings); the exact diff that will be
applied is [`ibridge-teardown-fix.preview.diff`](ibridge-teardown-fix.preview.diff).

Key reframe: the GPF was **never about the half-dead post-hibernate endpoint** — the corruption
exists from boot, so even a live-endpoint teardown crashed. That gives us a safe staged test:
prove teardown safety on a LIVE endpoint first (step 3), *then* try the post-hibernate relight.

> 🖐 Steps marked **[PHYSICAL]** need you at the machine (power button / eyeballing the bar).

---

## Step 0 — Preconditions (2 min)

```bash
cd ~/Code/xeeban/macbook-t1-linux/touch-bar
less ibridge-teardown-fix.preview.diff        # review the exact change
dkms status | grep apple-ib                   # expect: apple-ib-drv/r307.4afd309 ... installed
```

Note: this patches `apple-ibridge.c` only. The earlier `apple-touchbar.c` disp fix
(`patch-and-build.sh`) is untouched and must stay applied — it is.

## Step 1 — Apply patch + rebuild (5 min)

```bash
sudo ./patch-ibridge-teardown-and-build.sh
```

It backs up to `apple-ibridge.c.bak-preteardownfix`, applies both sections, sanity-greps, and
runs `dkms build/install --force`. Abort-safe: any anchor mismatch exits without modifying.

**Revert at any point:** `sudo ./revert-ibridge-teardown-patch.sh` then reboot.

## Step 2 — Cold-boot validation **[PHYSICAL]** (5 min)

```bash
sudo reboot
```

After login — visual: **Touch Bar lit** as usual. Then:

```bash
# 1. No more bogus warnings (pre-fix: 5 per boot) and no crash markers:
journalctl -k -b | grep -E "Unknown collection|Duplicate collection|list_del|protection fault|Oops|BUG"
#    -> expect EMPTY (the 5 'Unknown collection 2003xx/2002xx' lines must be GONE)

# 2. Both sub-devices exist (TB x2 + ALS):
ls /sys/bus/hid/devices/    # expect two 1D6B:0301 and one 1D6B:0302, drivers as before
grep . /sys/bus/hid/devices/0003:1D6B:030*/uevent | grep DRIVER
#    -> apple-touchbar x2; 1D6B:0302 -> hid-generic

# 3. Bar functions: tap Esc / fn-row, check sysfs knobs exist:
ls /sys/bus/hid/devices/0003:05AC:8600.*/../*/idle_timeout 2>/dev/null || \
  find /sys/bus/hid/devices -name idle_timeout
```

If anything is off → revert (Step 1 note) and we're back to last night's known state.

## Step 3 — THE GATE: live-endpoint power-cycle (no hibernate) **[PHYSICAL]** (10 min)

This is the exact operation that GPF'd before, on a fully awake machine. Save your work first
(a regression here can still oops). Open TWO terminals.

Terminal A (watch):
```bash
sudo journalctl -k -f
```

Terminal B:
```bash
# find the iBridge device (verify before writing!):
for d in /sys/bus/usb/devices/[0-9]*; do
  [ -f $d/idVendor ] && [ "$(cat $d/idVendor 2>/dev/null)$(cat $d/idProduct 2>/dev/null)" = "05ac8600" ] && echo $d
done
# (was 1-3 last night)
D=/sys/bus/usb/devices/1-3        # <- adjust if different

echo 0 | sudo tee $D/authorized   # tears down BOTH HID ifaces -> the old crash path
sleep 3
echo 1 | sudo tee $D/authorized   # re-enumerate -> fresh probes -> cold-boot light-up path
```

PASS criteria (all):
- Terminal A: clean disconnect/probe lines; **NO** `list_del`, `general protection`, `Oops`,
  `WARNING`, no SEGV of the tee process.
- Both `tee` writes return promptly (< ~10 s).
- No D-state leftovers: `ps -eo state,pid,comm | awk '$1=="D"'` → empty (give it 30 s).
- **[PHYSICAL]** Bar goes dark on deauthorize and **RELIGHTS** within a few seconds of
  reauthorize (this is `appletb_remove` clearing `active` + fresh `appletb_probe` — the
  cold-boot mechanism, now over a safe teardown).
- Bar works (Esc, fn keys); `ls /sys/bus/hid/devices/` shows freshly numbered virtual devs.

FAIL → screenshot/save the journal, revert, reboot, and we analyze; do NOT proceed to Step 4.

## Step 4 — Post-hibernate relight, manual **[PHYSICAL]** (15 min)

```bash
sudo systemctl hibernate
```
Resume with the **power button**. Expect: system up, Touch Bar **dark** (as always). Then run the
same cycle as Step 3 (same `$D`):

```bash
echo 0 | sudo tee $D/authorized; sleep 3; echo 1 | sudo tee $D/authorized
```

Caution — this is the half-dead endpoint case:
- The `authorized=0` write may take seconds (bounded USB timeouts). If it blocks **>60 s** or
  any process shows D-state, do not stack more commands; sync what you can and hard-reboot.
  (With the corruption fixed this is much less likely, but it's the one residual risk — see
  analysis §8.)
- PASS = same criteria as Step 3, ending with a **LIT, working Touch Bar after hibernate** —
  the goal of this whole campaign.

## Step 5 — Automate (only after 3+4 pass twice)

Re-enable a relight hook, but run the cycle **detached with a timeout** so a wedge can never
hang the resume path, e.g. in `51-touchbar-relight-hibernate.sh` (post case):

```bash
systemd-run --unit=tb-relight --on-active=10 \
  /bin/sh -c 'echo 0 > /sys/bus/usb/devices/1-3/authorized; sleep 3; echo 1 > /sys/bus/usb/devices/1-3/authorized'
```

(plus `TimeoutStartSec`/`RuntimeMaxSec=30` hardening; device path resolution by vid:pid, not
hardcoded port, if you want it robust). Then a full lid-close → lid-open cycle test. Update
`../sleep-resume/TOUCHBAR-RELIGHT-ANALYSIS.md` status when done.

## Step 6 — Aftercare

- `/log` the outcome to the daily note.
- Consider upstreaming: this OOB exists in pristine r307 (`apple-ibridge.c.orig` line ~411) —
  worth a PR/issue against the apple-ib-drv repo with the analysis doc.
- DKMS will carry the patched source forward on kernel updates automatically (it rebuilds from
  `/usr/src/apple-ib-drv-r307.4afd309/`, which now contains the fix). If you ever re-extract
  pristine r307 sources, re-run both patch scripts.

## Quick revert (any stage)

```bash
sudo ./revert-ibridge-teardown-patch.sh && sudo reboot
```
Restores last-night's source (resume/disp fixes intact, teardown fix removed). Known state:
bar lit on cold boot, dark after hibernate, NO iBridge teardowns attempted.
