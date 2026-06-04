# Agent Spec — Make the T1 MacBook Pro Touch Bar work on Linux

**Purpose:** Hand this file to a coding agent (Claude Code, etc.) and say:

> *"Follow `AGENT_SPEC.md` to make my Touch Bar work. Stop at every GO/NO-GO gate and at every step that needs me to physically look at the bar."*

This is a complete, self-contained spec + plan. It is written so an autonomous agent can execute it deterministically, with verification gates between phases and explicit guardrails that encode mistakes already paid for. Read the whole file before running anything.

---

## 0. Mission & definition of done

**Goal:** A lit Touch Bar showing **esc + F1–F12**, with **Fn** toggling to media (**brightness/volume**), that **survives reboot** and auto-loads at boot, with **no kernel oops/warnings**.

**Done when ALL are true:**
- [ ] `ls /sys/bus/hid/devices/ | grep 1D6B` shows both `1D6B:0301` (Touch Bar) and `1D6B:0302` (ALS).
- [ ] The `0003:1D6B:0301.*` device is bound to driver `apple-touchbar`.
- [ ] `sudo dmesg | grep -iE 'oops|BUG|call trace|warning'` → nothing touchbar-related.
- [ ] **Human confirms visually**: bar is lit; esc works; a brightness key works.
- [ ] Survives **2–3 reboots** unattended.

**The agent cannot self-certify the visual checks.** Those require a human looking at the hardware. Pause and ask.

---

## 1. Preconditions — verify BEFORE touching anything

Run these and STOP if any fails or is unexpected. Report findings to the human.

```sh
# Hardware: must be a T1 iBridge Mac. Expect a MacBookPro13,2 / 14,2 class board.
cat /sys/devices/virtual/dmi/id/product_name 2>/dev/null
# iBridge must be present on USB as 05ac:8600:
lsusb | grep -i '05ac:8600' || echo "NO iBridge 05ac:8600 — this spec does NOT apply (not a T1, or different bridge)"
# Kernel + Secure Boot state (affects module signing):
uname -r
mokutil --sb-state 2>/dev/null || echo "mokutil absent — assume Secure Boot off unless told otherwise"
# Distro / AUR helper (commands below assume Arch + yay; adapt for others):
command -v yay pacman dkms || true
```

**Hard gate P-1 (hardware):** If `05ac:8600` is absent, **STOP**. This fix is specific to the **T1** iBridge bar. A T2 Mac (`05ac:8302/8102`) uses the *mainline* `appletbdrm` stack instead — different problem, do not proceed here.

**Why this matters:** the entire bug below lives in the out-of-tree iBridge **demux** driver, which only exists because the T1 multiplexes the bar behind `05ac:8600`. T2 has no demux and no need for this driver.

---

## 2. Background the agent must hold in context

- The T1 bar hides behind one multiplexed USB composite: **iBridge `05ac:8600`**.
- The out-of-tree driver `apple-ib-drv` provides two modules:
  - `apple_ibridge` — **demuxes** `8600` into virtual HID sub-devices `1d6b:0301` (Touch Bar) and `1d6b:0302` (ALS).
  - `apple_touchbar` — binds `1d6b:0301` and drives the bar's firmware.
- **There is no in-tree iBridge demux.** Mainline `appletbdrm`/`hid-appletb-*` match only T2 IDs and will never bind a T1. Do not waste cycles on them.
- **Known root cause** (see §4): some revisions ship `appleib_ll_parse()` as a no-op, so sub-devices get no report descriptor and the kernel rejects them with `-ENODEV` → dark bar on **every** kernel.

---

## 3. Plan overview (phases with gates)

```
A. Install OOT driver        → B. Diagnose (is appleib_ll_parse a no-op?)  🚦
C. Patch the DKMS source     → D. Build + sign + install                    🚦
E. Configure load + boot     → F. Reboot (NOT live churn) → human visual    🚦
G. Persist + reboot-stress   → H. Hygiene/cleanup
```

Each 🚦 is a hard GO/NO-GO. Do not cross one on assumption.

---

## 4. The phases

### Phase A — Install the out-of-tree driver
```sh
yay -S --needed apple-ib-drv-dkms-git
```
If a different fork is already installed (`dkms status | grep apple-ib`), use it; don't stack two.

### Phase B — Diagnose 🚦
Load the demux module alone and check whether the sub-devices register.
```sh
sudo modprobe apple_ibridge
sleep 2
ls /sys/bus/hid/devices/ | grep -i 1D6B || echo "NO 1D6B sub-devices — demux produced nothing"
sudo dmesg | grep -iE 'ibridge|hid_add_device|ENODEV|parse' | tail -20
```
Then inspect the source for the known bug:
```sh
SRC=$(ls -d /usr/src/apple-ib-drv-*/ | head -1)
grep -n -A6 'appleib_ll_parse' "$SRC/apple-ibridge.c"
```
**Interpret:**
- If `appleib_ll_parse()` is a no-op (a `return 0;` / empty body, often with a comment claiming `hid_parse_report()` was "already called") **and** no `1D6B` sub-devices appear → **GO to Phase C**. This is the documented bug.
- If the function already calls `hid_parse_report(...)` and sub-devices DO appear but the bar is still dark → this is a *different* failure (firmware/display/suspend). **STOP and report**; this spec's patch won't help. Capture full `dmesg`.

> ⛔ **GUARDRAIL (do not skip):** Do **not** loop rapid `modprobe -r` / rebind / reprobe cycles to "experiment." On a half-broken driver this can wedge a module in **D-state** with a negative refcount, which is **unrecoverable without a reboot** and can taint the rest of the session. Load once, observe, and prefer a **reboot** over live unbind/rebind churn throughout.

### Phase C — Patch the DKMS source
Edit `$SRC/apple-ibridge.c` and `$SRC/apple-touchbar.c`. **Back up each file as `*.orig` first.**

**Fix 1 (the essential one) — restore the real parse in `appleib_ll_parse()`:**
The function must copy the *parent* iBridge device's fixed-up report descriptor into the virtual sub-device. Restore it to the upstream form:
```c
/* The handler receives the virtual sub-device `hdev`; locate the parent
 * iBridge hid_device that owns the post-fixup descriptor and parse it in.
 * The exact accessor for the parent varies by fork — in this driver it is
 * reached via the per-sub-device info struct. Restore to upstream: */
return hid_parse_report(hdev, parent->rdesc, parent->rsize);
```
- The source buffer is the parent's **`->rdesc`** (post `appleib_report_fixup`), **not** `->dev_rdesc`.
- This is a single clean alloc that `hid_parse_report()` kmemdups into the sub-device's `dev_rdesc`; there is no double-free.
- **Source of truth:** if unsure of the exact parent accessor in your fork, restore the original body via `git log -p`/`git blame` on the upstream repo, or diff against an older working revision. The semantic requirement is fixed: **`appleib_ll_parse()` must end with a `hid_parse_report()` of the parent's fixed-up descriptor.**

**Fix 2 (robustness) — NULL-guard the forward op:**
In `appleib_forward_int_op()`, guard the sub-device driver pointer; the touchbar-only interface has a `NULL` ALS slot and will oops on suspend without this:
```c
if (sub_hdev && sub_hdev->driver) { /* ... existing forward ... */ }
```

**Fix 3 (robustness) — lifecycle hardening in `apple-touchbar.c`:**
- When an interface is removed, NULL the cached `disp_field` / `disp_field_aux1` / `mode_field`.
- NULL-guard those fields in `appletb_set_tb_disp()` / `appletb_set_tb_mode()`.
- Call `cancel_delayed_work_sync()` on **every** removal path (not only the active one).
These kill a rebind/suspend use-after-free.

> Fixes 2 and 3 are not strictly required to *light* the bar, but without them the driver can oops on suspend/rebind. Include them.

### Phase D — Build, sign, install 🚦
```sh
VER=$(dkms status | sed -n 's#^apple-ib-drv/\([^,]*\).*#\1#p' | head -1)
sudo dkms build  apple-ib-drv/$VER -k "$(uname -r)" --force
sudo dkms install apple-ib-drv/$VER -k "$(uname -r)" --force
```
- **Secure Boot:** if `mokutil --sb-state` says enabled, the modules must be signed with an enrolled MOK or they won't load. Use the machine's existing MOK/signing hook; if none exists, ask the human (enrolling a MOK requires a reboot + password they set).
- **GATE D-1:** build must complete with **no errors**. Warnings about "out-of-tree" / "module verification failed: signature missing" are benign for DKMS and are NOT the problem.

### Phase E — Configure default mode + one-shot boot bind
```sh
echo 'options apple_touchbar fnmode=2 idle_timeout=-1 dim_timeout=-1' \
  | sudo tee /etc/modprobe.d/apple-ib-tb.conf
printf 'apple_ibridge\napple_touchbar\n' | sudo tee /etc/modules-load.d/touchbar.conf
```
- `fnmode=2` defaults the bar to function keys with Fn toggling media. Adjust to taste later.
- Ensure no leftover blacklist of `apple_ibridge`/`apple_touchbar` from earlier experiments: `grep -r apple /etc/modprobe.d/`.
- Keep `hid_sensor_hub` blacklisted only if it was hijacking the interface; otherwise leave defaults.

### Phase F — Reboot, then human visual check 🚦
```sh
# Prefer a clean reboot over any live module surgery (see Phase B guardrail).
sudo reboot
```
After reboot, the agent verifies the machine-checkable half:
```sh
lsmod | grep -E 'apple_ibridge|apple_touchbar'
ls /sys/bus/hid/devices/ | grep 1D6B                 # expect 0301 + 0302
# 0003:1D6B:0301.* should be bound to apple-touchbar:
for d in /sys/bus/hid/devices/*1D6B*0301*; do basename "$(readlink -f "$d/driver" 2>/dev/null)"; done
sudo dmesg | grep -iE 'oops|BUG|call trace|warning'  # expect none
```
**GATE F-1 (human):** Ask the human to look at the bar and confirm: lit? esc works? a brightness key works? **Do not mark the task done without this.** If machine checks pass but the bar is dark, capture `dmesg` and report — the remaining suspects are firmware/display/power, outside this patch.

### Phase G — Persist + stress
- Reboot **2–3 more times**; confirm the bar comes up every time (re-run Phase F checks).
- Test suspend/resume. If the bar doesn't return after sleep (USB timeout in `appletbdrm_probe` / T-chip cuts power is a known 7.x issue), add a sleep hook that unloads the modules before suspend and reloads + re-enumerates (`echo 0 > .../bConfigurationValue; sleep 1; echo 1 > ...`) on resume. Do this **after** the bar works at boot — don't let it block the main win.

### Phase H — Hygiene / cleanup
- If you created any temporary passwordless-sudo grant to run unattended steps, **remove it now** and verify sudo requires a password again. Validate any `/etc/sudoers*` edit with `visudo -c` **before** it takes effect, and never leave a `NOPASSWD: ALL` line behind.
- Remove scratch files and disabled-config backups you created.
- Note for the human: **DKMS rebuilds the patch on kernel updates (good), but an AUR update of `apple-ib-drv-dkms-git` will overwrite the patched source.** Re-apply from the `*.orig` backups, or pin/ignore the package in the AUR helper.

---

## 5. Guardrails (the expensive lessons, collected)

1. **Reboot beats live churn.** Never loop `modprobe -r`/rebind on this driver — it can wedge a module in D-state (unrecoverable without reboot). Load once, observe, reboot to apply.
2. **The agent cannot confirm the bar is lit.** Always gate completion on a human visual check.
3. **T1 ≠ T2.** If you find yourself reaching for `appletbdrm`/`hid-appletb-*`, you're on the wrong path for a T1.
4. **Secure Boot:** unsigned modules silently fail to load. Check `mokutil --sb-state` early.
5. **Sudoers safety:** validate with `visudo -c` before applying; remove any temporary NOPASSWD grant in Phase H; never leave one behind.
6. **Benign noise:** "out-of-tree module taints kernel" and "module verification failed: signature missing" are normal for DKMS — not the bug.
7. **Don't trust the comment.** The bug hid behind a comment asserting work was already done. Verify against the kernel's actual `hid_add_device()` contract and against upstream source, not in-tree comments.

## 6. Rollback

```sh
SRC=$(ls -d /usr/src/apple-ib-drv-*/ | head -1)
sudo cp "$SRC/apple-ibridge.c.orig" "$SRC/apple-ibridge.c"   # restore originals
sudo cp "$SRC/apple-touchbar.c.orig" "$SRC/apple-touchbar.c"
sudo dkms install apple-ib-drv/$VER -k "$(uname -r)" --force
# or remove entirely:  yay -R apple-ib-drv-dkms-git
```

## 7. What to report back to the human

A short status with: hardware confirmation, whether `appleib_ll_parse()` was a no-op, the exact 3 edits made, build/sign result, post-reboot machine checks, and an explicit request for the visual confirmation. Surface anything that contradicts this spec (different fork layout, sub-devices appearing despite a no-op, etc.) instead of forcing the steps.
