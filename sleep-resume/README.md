# Fixing suspend/resume on a T1 MacBook Pro (Linux)

Why a **long** sleep — but never a short one — brought the machine back to a lock screen that silently refused every keystroke.

> **TL;DR** — On a **MacBookPro13,2 (T1)** running Arch Linux (kernel `7.0.10`, `s2idle`), short naps resumed perfectly but longer sleeps came back to a rendered lock screen that ignored all keyboard and trackpad input — only a hard power-down recovered it. The cause was **not** the sleep state. The out-of-tree `apple_ibridge` Touch Bar driver's HID suspend callback, `appleib_hid_suspend()`, walked an array of virtual sub-devices and dereferenced one that had already been **freed** (`appleib_remove_device()` destroys them but never clears the array slots). The resulting use-after-free Oopsed a kernel PM worker, and *that* cascade wedged HID input on resume. The fix is three small driver changes — chiefly **NULL the `sub_hdevs[]` slots the instant they're destroyed** — plus a repaired udev rule. `s2idle` was correct all along.

> **Status:** diagnosed and patched **2026-06-05**; module rebuilt, installed, and disassembly-verified. Final resume confirmation is pending the first post-reboot suspend test (see [Validation](#validation)).

This is a sequel to [the Touch Bar fix](../touch-bar/) on the same machine — and it closes a thread that writeup left dangling.

---

## The machine

- **MacBookPro13,2** — a 2016 13" MacBook Pro with the **T1** Touch Bar, living its second life as a distraction-free "writer's deck."
- **Arch Linux**, GNOME on Wayland, kernel **`7.0.10-arch1-1`**.
- Sleep mode: **`s2idle`** (forced with `mem_sleep_default=s2idle`). The Touch Bar already works thanks to a [patched `apple-ib-drv`](../touch-bar/).

## The symptom — and why duration was the whole clue

| Sleep length | What happened |
|---|---|
| **Short** (a minute or two) | Touch the trackpad or a key → lock screen → type the password → back to the desktop. Flawless. |
| **Longer** (left idle) | No wake on input. After a while the lock screen *does* paint — but it accepts **no keystrokes**. Frozen. Only a hard power-down recovers it. |

The lock screen rendering is the key tell: **the kernel resumed** (the GPU and compositor came back) but **input was dead**. That's not a sleep-state failure — it's an input-stack failure on resume. And the dependence on *duration* meant something was arming itself only after the machine had been idle a while.

## The trap: "it must be deep-sleep / S3 incompatibility"

Every instinct (and most forum threads) says *old Mac + bad resume = broken S3 deep sleep*. That's a dead end here:

| Theory | Reality |
|---|---|
| Switch from `s2idle` to `deep` (S3) | **Backwards.** Intel Macs have broken S3 *resume* in Apple's firmware; `s2idle` is the **correct** choice. The Arch wiki says so outright. |
| It's `suspend-then-hibernate` escalating | Not configured — `sleep.conf`/`logind.conf` were stock defaults. |
| The `applespi` keyboard isn't resuming | On this model the keyboard **isn't on USB at all** (see below) — it's SPI, and independent. |
| `usbcore.quirks=05ac:8600:k` to stop autosuspend | Wrong knob — `k` is `USB_QUIRK_NO_LPM` (USB-3 link power). The kernel's quirk flags (`BIT(0)`–`BIT(18)` in `usb/quirks.h`) include **no** autosuspend toggle at all; use a `power/control=on` udev rule or `usbcore.autosuspend=-1` instead. |

The real answer was in the kernel log, not the wiki.

## Reading the crash

A prior boot's journal held the smoking gun — a kernel Oops:

```
RIP: appleib_hid_suspend+0x49/0x130 [apple_ibridge]
Call Trace:
  appleib_hid_suspend
  hid_suspend
  usb_suspend_both
  usb_runtime_suspend     ← runtime PM, not system suspend
  rpm_suspend
  pm_runtime_work
  worker_thread
```

Two facts fall out of this immediately:

1. **The faulting driver is `apple_ibridge`** — the Touch Bar bridge — in its **suspend** path.
2. **The trigger is USB runtime autosuspend** (`pm_runtime_work → usb_runtime_suspend`). The iBridge powers itself down after sitting idle. *That's the duration dependency.* Short nap → the autosuspend timer hasn't elapsed → clean resume. Long idle → the iBridge runtime-suspends → the callback runs → it crashes.

Decoding the faulting instruction (`mov rax,[rax+0x90]`, with `rax` holding non-NULL garbage) against the struct layout (`pahole`) pinned it exactly:

```
[rax+0x90]  = hid_driver->suspend     (struct hid_driver.suspend is at offset 0x90)
       rax  = sub_hdev->driver        (a garbage-but-non-NULL hid_driver *)
```

So `appleib_hid_suspend()` was calling `sub_hdev->driver->suspend(...)` where **`sub_hdev->driver` was a dangling pointer**.

### Why the existing guard didn't save it

The [Touch Bar fix](../touch-bar/) had already added a guard here, suspecting exactly this area:

```c
/* the touchbar-fix guard — necessary but not sufficient */
if (sub_hdev && sub_hdev->driver) {
    rc = forward(sub_hdev->driver, sub_hdev, args);
    ...
}
```

But the crash decode shows `sub_hdev->driver` was **non-NULL garbage**. A `&&` test only rejects a *NULL* pointer; it sails straight past a freed-but-non-NULL one and then faults dereferencing it. The guard was one level too shallow.

## A counter-intuitive wiring fact

You'd assume the dead keyboard means the keyboard device failed to resume. On a T1 MacBookPro13,2 it's stranger than that:

```
Apple SPI Keyboard   phys=applespi/input0   ─┐  driven by applespi (SPI bus)
Apple SPI Touchpad   phys=applespi/input1   ─┘  NOT on the iBridge USB device

USB iBridge 05ac:8600 ──► only the Touch Bar + virtual HIDs hang off here
```

The keyboard and trackpad are **SPI** devices, completely independent of the iBridge USB device that crashed. So the input death isn't the keyboard suspending wrong — it's **collateral damage**: the Oops kills a kernel PM worker and taints the HID subsystem, and the whole input-handling layer comes back wedged. Fix the iBridge crash and the input wedge goes with it.

## Two trigger paths (and why the obvious mitigation isn't enough)

The same buggy callback, `appleib_hid_suspend()`, is reachable two ways:

| Path | Entry | Stopped by `power/control=on`? |
|---|---|---|
| **Runtime autosuspend** | `pm_runtime_work → usb_runtime_suspend` | ✅ Yes — this is the logged Oops |
| **System suspend** (`s2idle`) | `usb_dev_suspend → hid_suspend` | ❌ **No** — system suspend quiesces every device regardless of its runtime-PM control flag |

This matters enormously. Disabling USB autosuspend (the popular "fix") only severs the *runtime* path. But the real-world symptom — GNOME idle-suspends after 15 minutes — drives a **full system `s2idle`**, which calls the same crashing callback through a path that ignores `power/control` entirely. **The only complete fix is to make the driver callback itself crash-proof.**

## Root cause: a use-after-free on the sub-device array

`apple_ibridge` demuxes the iBridge into virtual HID sub-devices, kept in a fixed array:

```c
struct hid_device *sub_hdevs[ARRAY_SIZE(appleib_sub_hid_ids)];
```

On teardown:

```c
/* appleib_remove_device() — the bug */
for (i = 0; i < ARRAY_SIZE(hdev_info->sub_hdevs); i++) {
    if (hdev_info->sub_hdevs[i])
        hid_destroy_device(hdev_info->sub_hdevs[i]);   // frees it…
        /* …but the slot is never set back to NULL */
}
```

After `hid_destroy_device()` the memory is freed, but **the array still points at it**. If a PM callback (`appleib_hid_suspend`) walks the array during or after a teardown, it reads a freed `sub_hdev`, follows its now-garbage `->driver`, and Oopses. The fixed-size guard "slots stay NULL" assumption only holds for slots that were *never used* — not for ones that were used and then freed.

## A dead end worth documenting: the unload/reload hook that livelocked

The canonical community advice for "the Touch Bar driver crashes on resume" is a `systemd` `system-sleep` hook that **unloads** `apple_ibridge` before sleep and **reloads** it after. Before trusting that in a sleep hook, we tested the unload/reload cycle **while awake** — no suspend, no risk of a hard wedge:

```sh
sudo rmmod apple_touchbar apple_ibridge     # clean (exit 0), bar goes dark
sudo modprobe apple_ibridge apple_touchbar  # … and never returns
```

`rmmod` was clean and the reload re-probed every HID device with **zero Oops** — but `modprobe apple_ibridge` then **livelocked at 99.5% CPU**, stuck inside the synchronous HID re-probe in module-init. It was unkillable (`kill -9` did nothing — a kernel busy-loop), and only a reboot cleared the pegged core. The module *was* loaded and the bar *did* work; init simply never returned.

**That disqualifies the unload/reload hook**: a resume-time `modprobe` would peg a core on every wake. Testing it awake — instead of discovering it during a real resume — turned a would-be hard wedge into a harmless observation. Lesson: **de-risk destructive steps in the safe state first.**

## The patch

Three changes to the DKMS source (`/usr/src/apple-ib-drv-*/apple-ibridge.c`), all backed up as `*.pre-resume-fix`:

**1. NULL the slot on destroy — the core fix** (`appleib_remove_device`):

```c
if (hdev_info->sub_hdevs[i]) {
    hid_destroy_device(hdev_info->sub_hdevs[i]);
    hdev_info->sub_hdevs[i] = NULL;   /* kill the dangling pointer at its source */
}
```

**2. Harden the suspend/resume walk** (`appleib_forward_int_op`) so it can't be fooled by a stale `drvdata`, an `ERR_PTR` slot, or a cleared driver:

```c
if (!hdev_info)                       /* drvdata can be NULL during remove races */
    return 0;
...
sub_hdev = READ_ONCE(hdev_info->sub_hdevs[i]);
if (!sub_hdev || IS_ERR(sub_hdev))    /* unused slots are NULL; add() can leave ERR_PTR */
    continue;
drv = READ_ONCE(sub_hdev->driver);    /* snapshot once */
if (!drv)
    continue;
rc = forward(drv, sub_hdev, args);
```

**3. Make the add() error-unwind NULL-safe** (`appleib_add_device`) — which also fixes a latent out-of-bounds read, since the original returned `sub_hdevs[i]` *after* `while (i-- > 0)` had left `i == -1`:

```c
if (IS_ERR(hdev_info->sub_hdevs[i])) {
    void *err = hdev_info->sub_hdevs[i];        /* capture before the loop mangles i */
    while (i-- > 0)
        if (hdev_info->sub_hdevs[i])            /* skip NULL (unmatched) slots */
            hid_destroy_device(hdev_info->sub_hdevs[i]);
    return err;
}
```

Plus a **repaired udev rule** that re-pins the iBridge to `power/control=on` (and `autosuspend_delay_ms=-1`) on `add`/`bind`/`change`. With the driver now crash-proof on both paths this is belt-and-suspenders, but it also stops the wasteful runtime churn — and the previous rule was silently half-broken (its `power/control=on` never stuck, because `39-usbmuxd.rules` re-touches the same `05ac:8600` device).

## Reproduce it

> For a T1 MacBook (MacBookPro13,2 / 14,x) on a modern kernel, with the [Touch Bar driver](../touch-bar/) already installed.

```sh
# 1. Confirm the bug class: an appleib_hid_suspend Oops in the journal,
#    and the iBridge armed for autosuspend.
journalctl -k | grep -i appleib_hid_suspend
cat /sys/bus/usb/devices/1-3/power/control      # 05ac:8600 "iBridge"

# 2. Patch /usr/src/apple-ib-drv-*/apple-ibridge.c with the three changes above
#    (back up the originals first: cp apple-ibridge.c apple-ibridge.c.pre-resume-fix)

# 3. Rebuild + install for your running kernel (DKMS re-signs for Secure Boot)
sudo dkms build  apple-ib-drv/<version> -k "$(uname -r)" --force
sudo dkms install apple-ib-drv/<version> -k "$(uname -r)" --force

# 4. (belt-and-suspenders) keep the iBridge out of USB autosuspend
sudo tee /etc/udev/rules.d/99-apple-ibridge-no-autosuspend.rules >/dev/null <<'EOF'
ACTION=="add",    SUBSYSTEM=="usb", ATTR{idVendor}=="05ac", ATTR{idProduct}=="8600", TEST=="power/control", ATTR{power/control}="on", ATTR{power/autosuspend_delay_ms}="-1"
ACTION=="bind",   SUBSYSTEM=="usb", ATTR{idVendor}=="05ac", ATTR{idProduct}=="8600", TEST=="power/control", ATTR{power/control}="on"
ACTION=="change", SUBSYSTEM=="usb", ATTR{idVendor}=="05ac", ATTR{idProduct}=="8600", TEST=="power/control", ATTR{power/control}="on"
EOF

# 5. Reboot to load the patched module. (Don't try to hot-reload apple_ibridge —
#    its module-init livelocks on a manual reload; see the dead end above.)
sudo reboot
```

> ⚠️ Same persistence caveat as the Touch Bar fix: **DKMS rebuilds the patch across kernel updates, but an AUR package update of `apple-ib-drv-dkms-git` overwrites the patched source.** Pin it with `IgnorePkg` and re-apply from the `*.pre-resume-fix` backups after any deliberate update.

## Validation

The fix is rigorously **diagnosed** (oops decode + `pahole` offsets + disassembly of the rebuilt module confirming the new guards are present) and **built/installed**, but the final proof is a clean resume — which needs an actual suspend. Validate **in person**, never unattended, because a failed resume on this machine means a hard power-down:

```sh
# After reboot, with you watching, a short self-waking suspend:
sudo rtcwake -m mem -s 20      # wakes itself after 20 s
# Then a real long idle. Check the journal came back clean:
journalctl -k -b | grep -i appleib_hid_suspend    # expect nothing
```

If a residual crash ever appears, the fallback is to no-op the HID suspend/resume forwarding entirely (the bar re-inits on resume), mirroring the community unload approach without the livelock.

## Lessons

- **The symptom lied; the call trace didn't.** "Bad resume" screamed *deep-sleep incompatibility*; the kernel log said *driver use-after-free*. Read the trace.
- **A guard is only as deep as the pointer you actually dereference.** `if (sub_hdev && sub_hdev->driver)` looked safe but faulted one level down, on `driver->suspend`.
- **Fix lifecycle bugs at the source.** Validating pointers in the hot path is a band-aid; clearing the slot when you free it removes the dangling pointer entirely.
- **De-risk in the safe state.** Exercising the unload/reload cycle *awake* caught a livelock that would have been a recurring hard wedge if shipped in a sleep hook.
- **Know which mitigation covers which path.** Disabling runtime autosuspend never touches the system-suspend path — the one that actually bites on a long idle.

## Upstream

The missing `sub_hdevs[i] = NULL` in `appleib_remove_device()` (and the shallow suspend-walk guard) live in the `apple-ib-drv` forks (t2linux, AdityaGarg8). If you maintain one, this is worth upstreaming so the next T1 owner doesn't lose a resume cycle to it.

---

*Written up by [Nori Nishigaya](https://github.com/xeeban) · Victoria, BC. More at [Emergent Insights](https://emergentinsights.substack.com/).*
