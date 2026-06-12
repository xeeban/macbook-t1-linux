# Resurrecting the Touch Bar on a T1 MacBook Pro (Linux)

How a one-line no-op in an out-of-tree driver kept the Touch Bar **dark on every kernel** — and the patch that finally lit it up.

> **TL;DR** — On a **MacBookPro13,2 (T1)** running Arch Linux (kernel `7.0.10`), the Touch Bar stayed dark because the out-of-tree `apple-ib-drv` driver (rev `r307`) shipped its `appleib_ll_parse()` HID hook **gutted to a no-op**. With no report descriptor, the kernel's `hid_add_device()` rejected the bar's virtual sub-devices with `-ENODEV`, so nothing ever bound. Restoring the real `hid_parse_report()` call (plus two robustness fixes) lights the bar — esc, F-keys, brightness, and volume all work and survive reboot.

There are [slides](./presentation.md) too (Marp). And a **[sequel](#sequel--the-touch-bar-goes-dark-after-hibernate)**: lighting the bar at boot was round one — keeping it lit *across hibernate* was an ~8-reboot, all-night hunt with two more kernel bugs and [a story of its own](./THE-RELIGHT-HUNT.md).

**Want your own agent to just do it?** Hand [`AGENT_SPEC.md`](./AGENT_SPEC.md) to a coding agent (Claude Code, etc.) and say *"follow AGENT_SPEC.md to make my Touch Bar work."* It's a self-contained spec + phased plan with GO/NO-GO gates, the exact patch, guardrails, and rollback.

---

## The machine

- **MacBookPro13,2** — a 2016 13" MacBook Pro with the **T1** Touch Bar, reborn as a distraction-free "writer's deck."
- **Arch Linux**, GNOME on Wayland, kernel **`7.0.10-arch1-1`**.
- Everything came up clean except the Touch Bar: no esc, no F-keys, no brightness/volume — and this model has **no physical function row**, so those keys were genuinely missing.

## Three popular dead ends

The bar was dark on every kernel. The common advice all failed:

| Theory | Result |
|---|---|
| Blacklist `hid_sensor_hub` (it hijacks the HID interface) | Frees the interface, but bar still dark |
| Disable USB autosuspend via udev | No change |
| Switch to the in-tree mainline drivers (`appletbdrm`, `hid-appletb-kbd/-bl`) | They only match **T2** IDs — they can't bind a T1 at all |

## How a T1 Touch Bar is actually wired

The T1 bar sits behind a single multiplexed **iBridge** USB composite device, which must be de-multiplexed in software:

```
USB iBridge  05ac:8600          one physical device, many functions
      │      (needs a software demux)
      ├─►  1d6b:0301            virtual HID — the Touch Bar
      └─►  1d6b:0302            virtual HID — ambient light sensor
```

- Something has to **demux** `05ac:8600` into the virtual `1d6b:*` sub-devices.
- Only then can a touchbar driver bind `1d6b:0301` and drive the bar's firmware.
- **T2** Macs expose the bar directly as `05ac:8302/8102` — no demux needed.

That T1-vs-T2 difference is the whole trap: **there is no in-tree iBridge demux**, so on a T1 the out-of-tree `apple-ib-drv` (`apple_ibridge` + `apple_touchbar`) is the only driver that can work. "Just use mainline" is a dead end here.

## Narrowing in

With the OOT driver loaded, the virtual sub-devices never appeared:

```sh
$ ls /sys/bus/hid/devices/ | grep 1D6B
# (nothing)
```

`apple_ibridge` loaded without error, the demux was running, the sub-devices were being *built* — but the kernel rejected them silently. Why?

## Root cause: a HID parse hook gutted to a no-op

In `apple-ibridge.c`, this revision shipped its lower-level HID parse hook emptied out:

```c
/* r307 — broken */
static int appleib_ll_parse(struct hid_device *hdev)
{
    /* we've already called hid_parse_report() */   // ← false
    return 0;                                        // ← does nothing
}
```

The upstream version copies the **parent's fixed-up report descriptor** into each virtual sub-device:

```c
/* upstream / the fix */
return hid_parse_report(hdev, parent->rdesc, parent->rsize);
```

A comment claiming the work was "already done" — when it wasn't — quietly disabled the one step that makes a sub-device valid.

## Why a no-op parse = a permanently dark bar

The kernel's `hid_add_device()` has enforced this contract since v3.10:

```c
ll_driver->parse(hdev);
if (!hdev->dev_rdesc)
    return -ENODEV;          // no descriptor → reject the device
```

So the chain was:

1. `parse()` is a no-op →
2. `dev_rdesc` stays `NULL` →
3. `hid_add_device()` returns `-ENODEV` →
4. the virtual sub-device never registers →
5. `apple_touchbar` has nothing to bind →
6. **dark bar.**

The crucial insight: this breaks on **every kernel**, not just 7.0. It was never a "new kernel regression" — the driver revision was simply broken, which is exactly why the sensor-hub / autosuspend / mainline rabbit holes all led nowhere.

## The patch

Three changes to the DKMS source (`/usr/src/apple-ib-drv-*/`), all backed up as `*.orig`:

1. **`appleib_ll_parse()`** (`apple-ibridge.c`) — restore the real parse so each sub-device gets a descriptor:
   ```c
   return hid_parse_report(hdev, parent->rdesc, parent->rsize);
   ```
   The source buffer is the parent's **post-fixup `->rdesc`** (a single clean alloc — no double-free).

2. **`appleib_forward_int_op()`** (`apple-ibridge.c`) — NULL-guard `sub_hdev->driver`. The touchbar-only interface has a `NULL` ALS slot, which was causing a suspend-path oops.

3. **`apple-touchbar.c`** — lifecycle hardening against a rebind/suspend use-after-free: clear the cached display/mode fields when the interface is removed, NULL-guard them on use, and always `cancel_delayed_work_sync()` on remove.

## Reproduce it

> For a T1 MacBook (MacBookPro13,2 / 14,2) on a modern kernel.

```sh
# 1. Install the out-of-tree driver
yay -S apple-ib-drv-dkms-git

# 2. Patch the DKMS source: in /usr/src/apple-ib-drv-*/apple-ibridge.c,
#    restore appleib_ll_parse() to:
#        return hid_parse_report(hdev, parent->rdesc, parent->rsize);
#    (plus the two robustness guards above)

# 3. Rebuild + install for your running kernel (sign for Secure Boot if enabled)
sudo dkms build  apple-ib-drv/<version> -k "$(uname -r)" --force
sudo dkms install apple-ib-drv/<version> -k "$(uname -r)" --force

# 4. Default the bar to function keys, and unbind/reprobe the iBridge once at boot
echo 'options apple_touchbar fnmode=2 idle_timeout=-1 dim_timeout=-1' \
  | sudo tee /etc/modprobe.d/apple-ib-tb.conf

# 5. Reboot → esc + F-keys + brightness/volume
sudo reboot

# 6. PIN the package so a future AUR update can't silently wipe your patched
#    source. IgnorePkg is honored by both pacman -Syu and yay.
sudo sed -i 's/^#IgnorePkg\s*=.*/IgnorePkg    = apple-ib-drv-dkms-git/' /etc/pacman.conf
#    (if you already have an active IgnorePkg line, just append the name to it)
```

Verify:

```sh
ls /sys/bus/hid/devices/ | grep 1D6B      # expect 0301 (TB) + 0302 (ALS)
# 0301 should be bound to driver "apple-touchbar"
sudo dmesg | grep -iE 'oops|BUG|warning'  # expect none
```

> ⚠️ **DKMS rebuilds the patch on kernel updates, so the fix persists — but an AUR package update of `apple-ib-drv-dkms-git` would overwrite the patched source.** That's why step 6 pins it via `IgnorePkg`. `pacman`/`yay` will still *notify* you that an update exists (good — you stay informed), they just won't install it. When you genuinely want to update: temporarily remove the `IgnorePkg` entry (or `yay -S apple-ib-drv-dkms-git`), then **re-apply the patch from the `*.orig` backups before rebuilding**.

## Sequel — the Touch Bar goes dark after hibernate

Lighting the bar at boot was round one. Round two showed up months later, once [hibernate (S4)](../hibernate/) became the way to step away from the machine: **after every resume from hibernate, the Touch Bar came back dark.** No esc, no F-keys — until the next full reboot.

It took an ~8-reboot, all-night hunt (full story: [**THE-RELIGHT-HUNT.md**](./THE-RELIGHT-HUNT.md)) to land three things:

### Bug A — a heap out-of-bounds write that crashes *every* iBridge teardown

The real villain, and it bites every T1 regardless of hibernate. `appleib_add_device()` fills the **2-slot** `sub_hdevs[]` array but indexes it by the **raw HID collection number**. The T1's combined display/ALS interface has **7 collections** (ALS `[0]`, five nested sensor collections — the `Unknown collection` boot warnings — and the Touch Bar display at `[6]`), so it writes `sub_hdevs[6]`: **24 bytes past the end of a 32-byte `devm` allocation**, clobbering the adjacent devres node. From then on *any* teardown (`modprobe -r`, USB unbind, a re-enumerate) walks the planted pointer and **GPFs** in `remove_nodes` / `__list_del_entry_valid`. (Proof it's the descriptor: the crash-register addresses are byte-for-byte the first 16 bytes of the live report descriptor — a `hid_device`'s first member is `dev_rdesc`.)

**Fix:** index `sub_hdevs[]` by the *matched* sub-device-id slot (`dev_id - appleib_sub_hid_ids`, always `{0,1}`), not the raw collection index. → [`ibridge-teardown-fix.insert.c`](./ibridge-teardown-fix.insert.c) · [`IBRIDGE-TEARDOWN-UAF-ANALYSIS.md`](./IBRIDGE-TEARDOWN-UAF-ANALYSIS.md) · apply with [`patch-ibridge-teardown-and-build.sh`](./patch-ibridge-teardown-and-build.sh).

> This one was **found and proven by an overnight [Fable](https://www.anthropic.com) agent** doing read-only kernel forensics while the machine sat idle — the load-bearing discovery of the whole effort. The same bug means the "first crash" most T1 users hit on driver unload is *this*, not bad luck.

### Bug B — the display report rode a stale queue

`set_tb_disp()` sent the display report through the iBridge's `usbhid` interrupt-OUT queue, which goes stale across hibernate (the report is silently dropped → dark bar), while `set_tb_mode()` used a direct control transfer that survives. Re-routing `set_tb_disp` through a synchronous `hid_hw_raw_request` fixed a `-32`/-EPIPE stall (visible even at cold boot). → [`disp-direct-usb.insert.c`](./disp-direct-usb.insert.c) · [`patch-and-build.sh`](./patch-and-build.sh).

### The relight itself — rebuild the whole stack

A successful display command *still* won't relight the firmware post-S4; nor will a USB re-enumerate or a bus reset (they reuse the stale `apple_ibridge` demux instance). Per a [survey of nine years of prior art](./POST-HIBERNATE-RELIGHT-INVESTIGATION.md), **nobody had relit a post-hibernate T1/T2 Touch Bar from Linux without a reboot** (macOS does it with a `DRLC` wake command over a config-2 bulk protocol Linux doesn't use). What *does* work is a **full reload of the `apple_ibridge` stack** — destroy and re-create the virtual HIDs and run a fresh `apple_touchbar` probe (the cold-boot light-up path):

```sh
modprobe -r apple_touchbar ; modprobe -r apple_ibridge ; modprobe apple_ibridge ; modprobe apple_touchbar
```

This is only **safe** once Bug A is fixed — before the fix, that exact `modprobe -r` is what hard-deadlocked the machine on hour one.

### Deploy the automatic relight

```sh
# 1. apply both driver patches (DKMS), reboot
cd touch-bar
sudo ./patch-and-build.sh                 # Bug B (set_tb_disp)
sudo ./patch-ibridge-teardown-and-build.sh # Bug A (the OOB — the important one)
sudo reboot

# 2. install the post-hibernate relight hook + reload helper
sudo ./deploy-relight-reload.sh
```

The hook (`sleep-resume/51-touchbar-relight-hibernate.sh`) runs *only* on hibernate resume and schedules the reload **~5 s later, detached, in a time-bounded transient unit** (`RuntimeMaxSec=90`) — so even a stalled reload can never hang the resume path. The freeze path stays clean (the `50-` hook still skips hibernate). Verify:

```sh
sudo systemctl hibernate    # power-button to resume; bar relights ~5–8 s later
journalctl -b 0 | grep -iE 'touchbar-relight|tb-relight-reload'
```

> **Minor caveat:** during the reload there's a brief `hid-generic` bind-race on the old-generation virtual HIDs (harmless — they're destroyed; `apple_touchbar` wins the fresh ones via HID rebind). If a resume ever comes back dark, re-run `/usr/local/sbin/touchbar-relight-reload`.

## Lessons

- **Trust the contract, not the forum.** The fix came from reading `hid_add_device()` in the kernel source — not from stacking community workarounds for the wrong problem.
- **A false comment is worse than no comment.** `/* we've already called hid_parse_report() */` sent people past the real bug for multiple revisions.
- **Know your hardware's shape.** T1 ≠ T2; the demux distinction invalidated half the advice online.
- **Old hardware is worth saving.** A 2016 laptop is now a clean, single-purpose writing machine.

## Upstream

> **Filed:** the heap-OOB fix (#2 below) is up as a PR — **[t2linux/apple-ib-drv#11](https://github.com/t2linux/apple-ib-drv/pull/11)** (build-tested, scoped to the T1 iBridge path, inert on T2).

Three things worth fixing in the `apple-ib-drv` forks (t2linux, AdityaGarg8) so the next T1 owner doesn't lose a weekend:

1. **The gutted `appleib_ll_parse()` no-op** (round one) — a dark bar on every kernel.
2. **The `appleib_add_device()` heap out-of-bounds write** (round two, [Bug A](#bug-a--a-heap-out-of-bounds-write-that-crashes-every-ibridge-teardown)) — the big one: it corrupts kernel memory on **every boot of every T1**, so *any* driver unload/teardown GPFs. This is a real, generic memory-safety bug, not hibernate-specific. The [analysis doc](./IBRIDGE-TEARDOWN-UAF-ANALYSIS.md) is essentially a ready-made bug report.
3. **`set_tb_disp()` on a stale `usbhid` queue** ([Bug B](#bug-b--the-display-report-rode-a-stale-queue)) — a `-32` stall; the synchronous `hid_hw_raw_request` form is more robust.

The post-hibernate **relight** (full stack reload) is a system-integration fix (a sleep hook), not a driver change — but the prior-art survey in [`POST-HIBERNATE-RELIGHT-INVESTIGATION.md`](./POST-HIBERNATE-RELIGHT-INVESTIGATION.md) suggests it may be the **first documented host-side T1 Touch Bar relight after hibernate**, so it's worth sharing with the t2linux/t1linux community regardless.

---

*Written up by [Nori Nishigaya](https://github.com/xeeban) · Victoria, BC. More at [Emergent Insights](https://emergentinsights.substack.com/).*
