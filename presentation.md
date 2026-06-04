---
marp: true
theme: default
paginate: true
size: 16:9
style: |
  @import url('https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700;800&display=swap');
  @import url('https://fonts.googleapis.com/css2?family=JetBrains+Mono:wght@400;600&display=swap');

  section {
    font-family: Inter, sans-serif;
    background:
      radial-gradient(circle at 84% 16%, rgba(255,107,53,0.18) 0%, rgba(255,107,53,0) 42%),
      radial-gradient(circle at 14% 82%, rgba(59,130,246,0.18) 0%, rgba(59,130,246,0) 46%),
      linear-gradient(140deg, #F2F8FF 0%, #E9F4FF 62%, #F4FAFF 100%);
    color: #0F1419;
    padding: 58px;
    position: relative;
    overflow: hidden;
    border-top: 10px solid #FF6B35;
  }

  section::after {
    content: "XEEBAN · github.com/xeeban";
    position: absolute;
    right: 28px;
    bottom: 14px;
    font-size: 11px;
    letter-spacing: 0.08em;
    color: #004E89;
    font-weight: 700;
  }

  h1 { color: #004E89; font-size: 2.1em; font-weight: 800; line-height: 1.06; margin-bottom: 0.2em; max-width: 88%; }
  h2 { color: #004E89; font-size: 1.06em; font-weight: 600; margin: 0.06em 0 0.7em; max-width: 84%; }
  h3 { color: #004E89; font-weight: 700; margin: 0 0 0.34em; }
  p, li { color: #0F1419; line-height: 1.34; }
  p { margin: 0.25em 0 0.72em; max-width: 92%; }
  ul, ol { margin: 0.28em 0 0.7em; }
  li { margin-bottom: 0.26em; }
  strong { color: #004E89; }
  ul li::marker { color: #FF6B35; }
  .accent { color: #FF6B35; font-weight: 700; }
  code { font-family: 'JetBrains Mono', monospace; font-size: 0.86em; background: rgba(0,78,137,0.08); padding: 1px 5px; border-radius: 5px; }
  pre { background: #0F1B2A; border-radius: 10px; padding: 14px 16px; font-size: 0.7em; line-height: 1.4; box-shadow: 0 8px 20px rgba(0,78,137,0.12); }
  pre code { background: none; color: #E6F0FF; padding: 0; }

  .band { display: inline-block; background: #004E89; color: #FFFFFF; border-radius: 999px; padding: 6px 12px; font-size: 0.72em; font-weight: 700; margin-bottom: 10px; }
  .quote { border-left: 6px solid #FF6B35; background: rgba(0,78,137,0.06); border-radius: 8px; padding: 12px 14px; font-size: 0.95em; line-height: 1.4; max-width: 94%; }
  .split { display: grid; grid-template-columns: 1fr 1fr; gap: 16px; }
  .card { background: rgba(255,255,255,0.92); border: 1px solid rgba(0,78,137,0.2); border-radius: 12px; padding: 14px; box-shadow: 0 2px 6px rgba(0,78,137,0.08); min-width: 0; }
  .card p { margin: 0; max-width: 100%; }
  table { width: 100%; border-collapse: collapse; font-size: 0.82em; background: rgba(255,255,255,0.94); box-shadow: 0 8px 20px rgba(0,78,137,0.08); }
  th { background: #004E89; color: #FFFFFF; text-align: left; padding: 8px 10px; }
  td { padding: 7px 10px; border-bottom: 1px solid rgba(0,78,137,0.12); }
---

<!-- Slide 1: Title -->

<span class="band">LINUX · APPLE T1 · KERNEL HACKING</span>

# Resurrecting the Touch Bar on a T1 MacBook Pro

## How a one-line no-op in an out-of-tree driver kept the bar dark on *every* kernel — and the patch that finally lit it

**Nori Nishigaya** ·  MacBookPro13,2 (T1) · Arch Linux · kernel `7.0.10`
*A writer's-deck debugging story · June 2026*

---

<!-- Slide 2: The goal -->

# The goal: a distraction-free "writer's deck"

An old **2016 MacBook Pro 13" (MacBookPro13,2)** reborn as a dedicated writing machine running Arch Linux + GNOME/Wayland.

Everything came up clean — GNOME, Obsidian, Claude Code, Ghostty — except one stubborn piece of hardware:

<div class="quote">
The <span class="accent">Touch Bar</span> stayed <strong>dark</strong>. No esc key. No F-keys. No brightness or volume. On a laptop with <em>no physical function row</em>, that's not cosmetic — it's missing keys.
</div>

This is the story of why it was dark, and the patch that fixed it.

---

<!-- Slide 3: The symptom & the false trails -->

# The symptom — and three popular dead ends

The bar was **dark on every kernel I tried.** The internet's usual advice all failed:

<div class="split">
<div class="card">

### ❌ "Blame the sensor hub"
Blacklisting `hid_sensor_hub` frees the HID interface — but the bar stayed dark.

</div>
<div class="card">

### ❌ "Blame USB autosuspend"
udev `no-autosuspend` rules changed nothing.

</div>
</div>

<div class="card" style="margin-top:14px;">

### ❌ "Switch to the mainline in-tree drivers"
`appletbdrm` / `hid-appletb-kbd` / `-bl` are real and in-tree on 7.0 — but they only match the **T2** post-demux IDs. They can't even *see* a T1. (More on this next.)

</div>

---

<!-- Slide 4: How the T1 Touch Bar actually works -->

# First, how a T1 Touch Bar is wired

The T1 bar hides behind a single multiplexed **iBridge** USB composite device:

<pre><code>USB iBridge  05ac:8600   ← one physical device, many functions
      │   (must be de-multiplexed in software)
      ├─►  1d6b:0301     virtual HID — the Touch Bar
      └─►  1d6b:0302     virtual HID — ambient light sensor</code></pre>

- A driver must **demux** `8600` into those virtual `1d6b:*` sub-devices.
- Only then can a touchbar driver bind `0301` and drive the bar's firmware.
- **T2** Macs expose the bar as `05ac:8302/8102` directly — *no demux needed.*

That difference is the whole trap.

---

<!-- Slide 5: Two driver worlds -->

# Two driver worlds — and why mainline can't help a T1

<table>
<tr><th>Driver stack</th><th>Demuxes iBridge 8600?</th><th>Works on T1?</th></tr>
<tr><td><strong>In-tree (mainline)</strong><br><code>appletbdrm</code>, <code>hid-appletb-kbd/-bl</code></td><td>No — matches only T2 <code>8302/8102</code></td><td>❌ Never binds</td></tr>
<tr><td><strong>Out-of-tree</strong><br><code>apple-ib-drv</code> (<code>apple_ibridge</code> + <code>apple_touchbar</code>)</td><td><strong>Yes</strong> — this is its whole reason to exist</td><td>✅ <em>if it works</em></td></tr>
</table>

<div class="quote" style="margin-top:18px;">
There is <strong>no in-tree iBridge demux.</strong> So for a T1, the out-of-tree <code>apple-ib-drv</code> is the only game in town. The "just use mainline" advice is a dead end on this hardware.
</div>

So the OOT driver was installed (AUR `apple-ib-drv-dkms-git`, rev `r307`)… and the bar was *still* dark.

---

<!-- Slide 6: Narrowing in -->

# Narrowing in: nothing was registering

With the OOT driver loaded, the virtual sub-devices simply **never appeared** in sysfs:

<pre><code>$ ls /sys/bus/hid/devices/ | grep 1D6B
# (nothing)</code></pre>

- `apple_ibridge` loaded without error.
- But `1d6b:0301` / `0302` were **never created**, so `apple_touchbar` had nothing to bind.
- No crash, no obvious log — just silence.

The demux was running. The sub-devices were being *built*. So **why did the kernel reject them?**

---

<!-- Slide 7: The root cause -->

# Root cause: a HID parse hook gutted to a no-op

Inside `apple-ibridge.c`, this revision shipped its lower-level HID parse hook **emptied out**:

<pre><code>// r307 — appleib_ll_parse()
static int appleib_ll_parse(struct hid_device *hdev)
{
    /* we've already called hid_parse_report() */   // ← false!
    return 0;                                        // ← does nothing
}</code></pre>

The upstream version is supposed to copy the **parent's fixed-up report descriptor** into each virtual sub-device:

<pre><code>return hid_parse_report(hdev, parent->rdesc, parent->rsize);</code></pre>

A comment claiming the work was "already done" — when it wasn't — quietly disabled the one step that makes a sub-device valid.

---

<!-- Slide 8: Why that one line kills the bar -->

# Why a no-op parse = a permanently dark bar

The kernel's `hid_add_device()` has, since v3.10, enforced this contract:

<pre><code>ll_driver->parse(hdev);
if (!hdev->dev_rdesc)
    return -ENODEV;          // no descriptor → reject the device</code></pre>

<div class="split">
<div class="card">

### The chain
1. `parse()` is a no-op
2. `dev_rdesc` stays **NULL**
3. `hid_add_device()` → **-ENODEV**
4. sub-device never registers
5. `apple_touchbar` never binds
6. **dark bar**

</div>
<div class="card">

### The kicker
This breaks on **every kernel**, not just 7.0.

It was never a "new kernel regression." The driver revision was simply **broken** — which is exactly why sensor-hub, autosuspend, and mainline rabbit holes all led nowhere.

</div>
</div>

---

<!-- Slide 9: The patch -->

# The patch: restore the parse + harden lifecycle

<span class="band">3 changes · DKMS source · MOK-signed</span>

1. **`appleib_ll_parse()`** — restore the real call so each sub-device gets a descriptor:
<pre><code>return hid_parse_report(hdev, parent->rdesc, parent->rsize);</code></pre>
   *(source is the parent's post-fixup `->rdesc`, a single clean alloc — no double-free)*

2. **`appleib_forward_int_op()`** — NULL-guard `sub_hdev->driver` (the touchbar-only interface has a NULL ALS slot → was a suspend-path oops).

3. **`apple-touchbar.c`** — clear cached display/mode fields on interface removal + always `cancel_delayed_work_sync()` on remove (kills a rebind/suspend use-after-free).

Built clean, `dkms install`-ed for `7.0.10`, signed for Secure Boot.

---

<!-- Slide 10: Proof -->

# Proof: the bar lights up

<pre><code>$ ls /sys/bus/hid/devices/ | grep 1D6B
0003:1D6B:0301.0006   ← Touch Bar   (driver: apple-touchbar)
0003:1D6B:0302.0008   ← ALS

$ sudo dmesg | grep -iE 'oops|BUG|warning'
# (none)</code></pre>

<div class="split">
<div class="card">

### ✅ Working
- **Esc** + **F1–F12**
- **Fn** toggles to media
- **Brightness / volume** keys
- Survives **reboot**, auto-loads at boot

</div>
<div class="card">

### 🧰 Verified the right way
Sub-devices register · `apple-touchbar` binds · **zero** oops/warnings · confirmed across multiple reboots.

</div>
</div>

---

<!-- Slide 11: Reproduce it -->

# Reproduce it on your own T1

<span class="band">MacBookPro13,2 / 14,2 · T1 · modern kernel</span>

1. Install the OOT driver: `yay -S apple-ib-drv-dkms-git`
2. In the DKMS source (`/usr/src/apple-ib-drv-*/`), restore `appleib_ll_parse()` to call `hid_parse_report(hdev, parent->rdesc, parent->rsize)` (+ the two robustness guards).
3. `sudo dkms build/install apple-ib-drv/<ver> -k $(uname -r) --force` (sign for Secure Boot if enabled).
4. `options apple_touchbar fnmode=2` + a one-shot iBridge unbind/reprobe at boot.
5. Reboot → esc + F-keys + brightness.

⚠️ DKMS rebuilds the patch on kernel updates — but an **AUR package update would overwrite it.** Re-apply from the `*.orig` backups (or pin the package).

> Full patch + commands in the repo README. PRs/issues welcome.

---

<!-- Slide 12: Lessons -->

# What this debugging taught me

<div class="split">
<div class="card">

### 🔬 Trust the contract, not the forum
The fix came from reading `hid_add_device()` in the *kernel source* — not from stacking community workarounds for the wrong problem.

</div>
<div class="card">

### 🕳️ A false comment is worse than no comment
`/* we've already called hid_parse_report() */` sent everyone past the actual bug for revisions.

</div>
</div>
<div class="split" style="margin-top:14px;">
<div class="card">

### 🧩 Know your hardware's shape
T1 ≠ T2. The demux distinction invalidated half the advice online.

</div>
<div class="card">

### ♻️ Old hardware is worth saving
A 2016 laptop is now a clean, capable, single-purpose writing machine.

</div>
</div>

---

<!-- Slide 13: Resources -->

# Resources & contact

<div class="split">
<div class="card">

### 📦 This writeup
**github.com/xeeban/macbook-t1-touchbar-linux**
Patch, exact commands, and the slides.

### 🔗 Upstream drivers
`apple-ib-drv` (t2linux / AdityaGarg8 forks) — the gutted `appleib_ll_parse()` no-op is worth reporting upstream.

</div>
<div class="card">

### 👋 Find me
**github.com/xeeban** · NOW page
**Emergent Insights** — emergentinsights.substack.com
Victoria, BC 🇨🇦

</div>
</div>

<div class="quote" style="margin-top:18px;">
If this helped you light up a T1 bar on Linux — say hi. That's why it's public.
</div>
