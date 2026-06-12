# Post-Hibernate Touch Bar Relight — Root-Cause Analysis & Status

**Machine:** MacBookPro13,2 (Apple T1), Arch Linux, out-of-tree `apple-ib-drv` r307 (patched).
**Date:** 2026-06-11. **Status:** Touch Bar comes back **DARK after hibernate**; no *safe automatic*
relight found yet. Hibernate itself and `lid=hibernate` are safe. The Touch Bar matters — it is the
**Esc key** — so this is being pursued, carefully.

> Sibling docs: [`README.md`](README.md) (sleep/resume strategy), [`../hibernate/README.md`](../hibernate/README.md),
> [`../touch-bar/README.md`](../touch-bar/README.md) (the original cold-boot Touch Bar fix + driver patches).

---

## TL;DR

- **Hibernate (S4) is safe and validated.** Plain suspend (s2idle) and deep/S3 both wedge this NVMe
  machine, so hibernate is the only sleep mode. `lid=hibernate` (`/etc/systemd/logind.conf.d/10-lid-hibernate.conf`).
- After hibernate **the Touch Bar is dark.** Root cause: the driver keeps a **stale USB/HID transport**
  across hibernate and silently fails to re-drive the display.
- **The only thing that relights it is a fresh `appletb_probe` over a freshly re-enumerated iBridge
  endpoint** — and any teardown that gets there can **D-state-deadlock the whole machine** when done
  against the *half-dead post-resume* endpoint.
- **Every automatic post-resume relight attempted has failed**, the last one (drain + USB
  deauthorize/reauthorize) **deadlocked the box and required a reboot** (2026-06-11 19:24).
- **Current safe state:** the relight hook is **disabled**; accept the dark bar after hibernate (it
  lights normally on a cold boot). **Next direction:** tear the device down **before** hibernate (while
  it is still live), not after.

---

## Root cause (verified against `/usr/src/apple-ib-drv-r307.4afd309` + live state)

1. **Singleton with cached transport.** `apple_touchbar` allocates one `appletb_dev` at module load,
   freed **only at module unload**. Across hibernate it is never torn down, so it keeps **dead cached
   handles**: `mode_iface.usb_iface` (a USB interface whose `usb_device` was re-enumerated under it) and
   `disp_iface.hdev` (a usbhid path whose endpoint was reset under it).
2. **Resume does not re-establish transport.** `appletb_reset_resume` only flips `suspended=false` /
   `active=true` and re-queues the worker (`appletb_update_touchbar(force=TRUE)`). It runs **no
   `hid_parse`, no `hid_hw_start`, no `hid_hw_open`, no USB re-enumeration**. The log line
   `tb: Touchbar resumed.` is pure control flow — the wire is still dead.
3. **Failures are silent.** `appletb_set_tb_disp` rides `hid_hw_request`, which through the patched
   ibridge `ll_request` is **VOID / fire-and-forget**. A dropped `SET_REPORT` is silent and the driver
   then sets `cur_tb_disp=ON` believing it succeeded. → **This is why forcing `idle_timeout`/`fnmode`
   does nothing: the trigger is innocent, the transport is dead.**
4. **Only a fresh probe over a re-enumerated endpoint lights it.** `appletb_probe` does
   `hid_hw_start` + `hid_hw_open` + re-extracts the iface/usb_iface + runs the init light-up
   (`pnd_tb_mode=UPD` / `pnd_tb_disp=UPD`). The USB power-cycle (`authorized 0→1`) is **mandatory** — a
   bare reload reuses the stuck firmware endpoint and logs `tb: hw open failed (-19)` (ENODEV).
5. **The `active` gate (the subtle part).** The probe light-up runs **only if
   `appletb_test_and_mark_active()` returns true, i.e. `!active`**. The singleton keeps `active=true`
   across hibernate; it is cleared **only** by `appletb_remove` running on a live iface — i.e. by a USB
   **disconnect**. So a successful relight requires: **clear `active` (via appletb_remove on disconnect)
   → cold re-probe → light-up.**

## The deadlock (the constraint that bans the simple fixes)

`cancel_delayed_work_sync` lives in `appletb_remove` (apple-touchbar.c). **Every** teardown path reaches
it — `modprobe -r apple_touchbar`, sysfs unbind of a `1D6B:0301`, `modprobe -r apple_ibridge`,
`apple-ibridge-hid` unbind, whole-device `8600` deauthorize, usbhid unbind — because
`appleib_remove_device` calls `hid_destroy_device(sub_hdev)` which invokes the child `appletb_remove`
per virtual Touch Bar HID. If `tb_work` is wedged in `usb_control_msg` / `hid_hw_request` against a
**half-dead (not cleanly disconnected) endpoint**, that sync wait does not return → the process parks in
**uninterruptible `D` state, survives `SIGKILL`** → the USB workqueue kworkers wedge behind it → only a
reboot clears it. (`apple-ibridge.c` itself has zero `_sync`/`flush`; the sole hazard is the child
`appletb_remove`.)

**Empirically confirmed 2026-06-11 19:24:** `idle_timeout=-2` drain + `echo 0 > .../authorized` on the
post-resume half-dead endpoint → `D`-state on the `authorized` write, `kworker/u16` + unrelated procs
(firefox on `lru_add_drain_all`) dragged into `D`, iBridge stuck `authorized=0`, sysfs reads hung →
reboot required. The drain did **not** prevent it (the drain's own forced worker pass and/or
`appletb_remove`'s post-cancel `set_tb_mode(OFF)` `usb_control_msg` wedged on the half-dead endpoint —
exactly what the adversarial review predicted).

## What was tried (all failed)

| # | Approach | Result |
|---|----------|--------|
| 1 | Relight fired instantly on resume (inline in `51-` hook) | Raced `appletb_set_tb_worker` → kernel WARN, dark |
| 2 | `systemd-run … /bin/bash -c '…$d…'` | systemd expands `$` in ExecStart → blank device path, no-op |
| 3 | Inline `sleep 15` then reset, blocking in the sleep hook | systemd SIGKILL'd the hook mid-reset → iBridge stuck `authorized=0` |
| 4 | `systemd-run --on-active=15s` → standalone binary, USB reset only | Ran cleanly, **bar stayed dark** (transport stale; `active` not cleared the way a true cold probe needs) |
| 5 | Force display via `idle_timeout`/`fnmode` (`force=TRUE`) | Dark — trigger is innocent, wire is dead |
| 6 | **Drain (`idle_timeout=-2`) + USB deauthorize/reauthorize** | **DEADLOCKED the machine** (D-state, reboot) |

> Full multi-agent analysis (architecture + adversarial quality panel + completeness critic) is archived
> in the Claude session workflow output (run `wf_5646cca1-b8b`).

## Conclusion

**There is no safe *post-resume* relight.** Re-establishing the Touch Bar's transport requires tearing
down the USB endpoint, and tearing down the **half-dead post-resume** endpoint can D-state-deadlock the
machine.

## Safe resting state (current)

- `50-apple-ibridge-touchbar.sh` **skips hibernate** (no module unload on the freeze path).
- `lid=hibernate`, no s2idle anywhere.
- **`51-touchbar-relight-hibernate.sh` is DISABLED** (made non-executable, or removed) — systemd-sleep
  only runs executable hooks, so no relight is attempted on resume. **Accept the dark bar after
  hibernate; it lights on a cold boot.**
- The `relight-touchbar*.sh` / `touchbar-relight.sbin.sh` scripts in this dir are the failed attempts;
  **do not deploy** without solving the teardown deadlock.

## Next direction (to try — targets the root cause safely)

**Tear the device down BEFORE hibernate, while the endpoint is still LIVE**, instead of after, against
the half-dead one:

- In the sleep **`pre hibernate`** hook (runs to completion before the freeze, device still alive):
  quiesce `tb_work` (`idle_timeout=-2`) then cleanly **unbind** `apple_touchbar` (or `apple-ibridge-hid`)
  from the live device. On a **live** endpoint the worker's commands complete fast, so
  `cancel_delayed_work_sync` returns quickly → **no deadlock** — and `appletb_remove` runs, **clearing
  `active`**.
- On resume the iBridge re-enumerates (cold), udev auto-binds → fresh `appletb_probe` with `active=false`
  → **runs the init light-up** = bar lit (the cold-boot path).

**Validate the risky part WITHOUT hibernating first:** on a live, lit bar, do quiesce → unbind → rebind
and confirm (a) the unbind completes with no `D`-state, (b) the rebind relights the bar. Only if that is
clean, wire it into the `pre` hook and test a real hibernate. Keep a watchdog and a `D`-state self-check;
never tear down the half-dead post-resume endpoint again.

---

## UPDATE 2 — Session continued (2026-06-11 ~21:00): driver patch + the real blocker

Pursued an in-driver fix instead of sleep hooks. Findings:

### Pre/post sleep-hook approach — tested and ABANDONED (kernel-level failures)
- **`pre`-unbind on a LIVE endpoint is safe** (validated: `unbind` returns instantly, no `D`-state) and a
  `bind` relights a live bar. But across a real hibernate, **`post`-bind on the reset-resumed endpoint left
  the bar DARK** (HID re-enumerates, but the display `SET_REPORT` is silently dropped).
- **`post`-power-cycle-while-unbound GPF'd the kernel**: `echo 0 > authorized` → `appleib_remove_device`
  → `hid_destroy_device` → `__list_del_entry_valid` **use-after-free** (`remove_nodes`, non-canonical ptr
  = freed sub_hdev reused by report data). SEGV'd the hook, wedged a USB kworker → reboot.
- **Conclusion:** every USB teardown of the iBridge crashes (GPF) or deadlocks the buggy `apple_ibridge`
  teardown. The 50-hook (skip hibernate) stays; the 51-/relight scripts are dead — do not deploy.

### Driver patch #1 — `set_tb_disp` via `hid_hw_raw_request` (DONE, good, but not sufficient)
Root cause refinement: post-resume `set_tb_mode` works (direct `usb_control_msg` on `mode_iface.usb_iface`,
~line 256) but `set_tb_disp` sent the display report via `hid_hw_request(disp_iface.hdev)` →
`appleib_ll_request` → the iBridge's **usbhid interrupt-OUT queue, which goes STALE across hibernate** →
display report silently dropped → dark.

Fix (in `../touch-bar/`): patch `appletb_set_tb_disp` to send via **`hid_hw_raw_request`** — a synchronous
`SET_REPORT` on EP0 (a CLASS request; a first try using a direct `usb_control_msg` with `USB_TYPE_VENDOR`
copied from `set_tb_mode` returned **`-32`/-EPIPE** because vendor type is a mode-report quirk), with an
`-EPIPE` retry loop like `set_tb_mode`. **Result: the `-32` error is gone** (cold boot logs no
`Failed to set touch bar display`). Files: `touch-bar/disp-direct-usb.insert.c`, `patch-and-build.sh`,
`revert-driver-patch.sh`. **Keep this patch — it's a genuine correctness fix.**

### The REAL remaining blocker — firmware needs re-enumeration; `apple_ibridge` teardown is buggy
Even with `set_tb_disp(ON)` now **succeeding** (no error) post-resume, **the bar stays DARK** — confirmed by
a manual `idle_timeout` -2/-1 re-drive that lit nothing. So the touch-bar **firmware** requires the full USB
**re-enumeration** (cold-boot path) to relight; a successful display command alone is not enough. The
re-enumeration (USB power-cycle) is *proven* to relight the bar, but it crashes `apple_ibridge`'s sub-HID
teardown (the UAF above).

**⇒ Next fix = the `apple_ibridge` teardown UAF.** Suspect: `appleib_add_device` (apple-ibridge.c 418-453)
fills `sub_hdevs[i]` indexed by `i < hdev->maxcollection`, while `appleib_remove_device` (455-471) and the
report forwarders iterate `i < ARRAY_SIZE(sub_hdevs)` — an index/lifecycle mismatch that can free/destroy
the wrong slot or double-free. Fix it → the post-resume USB power-cycle becomes safe → bar relights
reliably (cold-boot mechanism). An overnight analysis run is drafting candidate patches for this (see
`touch-bar/` for the UAF analysis + candidate patch landing in the morning).
