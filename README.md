# Resurrecting the Touch Bar on a T1 MacBook Pro (Linux)

How a one-line no-op in an out-of-tree driver kept the Touch Bar **dark on every kernel** — and the patch that finally lit it up.

> **TL;DR** — On a **MacBookPro13,2 (T1)** running Arch Linux (kernel `7.0.10`), the Touch Bar stayed dark because the out-of-tree `apple-ib-drv` driver (rev `r307`) shipped its `appleib_ll_parse()` HID hook **gutted to a no-op**. With no report descriptor, the kernel's `hid_add_device()` rejected the bar's virtual sub-devices with `-ENODEV`, so nothing ever bound. Restoring the real `hid_parse_report()` call (plus two robustness fixes) lights the bar — esc, F-keys, brightness, and volume all work and survive reboot.

There are [slides](./presentation.md) too (Marp).

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
```

Verify:

```sh
ls /sys/bus/hid/devices/ | grep 1D6B      # expect 0301 (TB) + 0302 (ALS)
# 0301 should be bound to driver "apple-touchbar"
sudo dmesg | grep -iE 'oops|BUG|warning'  # expect none
```

> ⚠️ **DKMS rebuilds the patch on kernel updates, so the fix persists — but an AUR package update of `apple-ib-drv-dkms-git` would overwrite the source.** Re-apply from the `*.orig` backups, or pin/ignore the package in your AUR helper.

## Lessons

- **Trust the contract, not the forum.** The fix came from reading `hid_add_device()` in the kernel source — not from stacking community workarounds for the wrong problem.
- **A false comment is worse than no comment.** `/* we've already called hid_parse_report() */` sent people past the real bug for multiple revisions.
- **Know your hardware's shape.** T1 ≠ T2; the demux distinction invalidated half the advice online.
- **Old hardware is worth saving.** A 2016 laptop is now a clean, single-purpose writing machine.

## Upstream

The gutted `appleib_ll_parse()` no-op appears in the `apple-ib-drv` forks (t2linux, AdityaGarg8). If you maintain one of those, this is worth fixing upstream so the next person with a T1 doesn't lose a weekend to it.

---

*Written up by [Nori Nishigaya](https://github.com/xeeban) · Victoria, BC. More at [Emergent Insights](https://emergentinsights.substack.com/).*
