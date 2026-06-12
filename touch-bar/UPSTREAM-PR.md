# Upstream PR draft — t2linux/apple-ib-drv (branch `mbp15`)

> ## ✅ FILED → https://github.com/t2linux/apple-ib-drv/pull/11
> Opened 2026-06-12 (`xeeban:fix/appleib-add-device-oob → mbp15`, +45/−13). The patch was regenerated
> against pristine upstream (this repo's DKMS tree already carries the round-one `ll_parse` fix that
> upstream lacks, which shifts the line numbers) and **build-tested clean** against 7.0.10 headers before
> filing. Note: `t2linux/apple-ib-drv` has **issues disabled**, so the PR carries the full bug report.
> The draft below is what was filed.
>
> Patch file: [`ibridge-teardown-fix.preview.diff`](./ibridge-teardown-fix.preview.diff) ·
> Analysis: [`IBRIDGE-TEARDOWN-UAF-ANALYSIS.md`](./IBRIDGE-TEARDOWN-UAF-ANALYSIS.md)

---

## Title

```
apple-ibridge: fix heap out-of-bounds write in appleib_add_device() (sub_hdevs[] indexed by raw collection index)
```

## Body

### What

`appleib_add_device()` indexes the fixed-size `sub_hdevs[]` array by the **raw HID collection index** `i`, but the array only has `ARRAY_SIZE(appleib_sub_hid_ids)` (= 2) slots. On the T1's combined display/ALS interface the report descriptor has **7 collections**, so the Touch Bar display (collection index **6**) is written to `sub_hdevs[6]` — **24 bytes past the end of the `devm`-allocated `appleib_hid_dev_info`**. This is a heap out-of-bounds write on **every probe / every boot** of a T1 iBridge; it corrupts the adjacent slab object and later **GPFs on any teardown** of the device (driver unload, USB unbind/re-enumerate, suspend/resume teardown).

This PR indexes `sub_hdevs[]` by the **matched sub-device-id slot** instead, which is in-bounds by construction, and adds the small guards that fall out of doing it correctly.

### Root cause

```c
/* apple-ibridge.c, appleib_add_device() — pristine mbp15, ~line 403/411 */
for (i = 0; i < hdev->maxcollection; i++) {       /* iterates ALL collections (7 on T1) */
    usage  = hdev->collection[i].usage;
    dev_id = appleib_find_dev_id_for_usage(usage);
    if (!dev_id)
        hid_warn(hdev, "Unknown collection ... usage %x\n", usage);   /* the 5 nested sensor ones */
    else
        hdev_info->sub_hdevs[i] = appleib_add_sub_dev(hdev_info, dev_id);  /* i can be 6 → OOB */
}
```

`hdev->maxcollection` counts every collection (the ALS application collection, its five nested sensor sub-collections — the familiar `Unknown collection encountered with usage 2003xx/2002xx` boot warnings — and the Touch Bar display), while `sub_hdevs[]` is only ever sized/iterated as 2 slots everywhere else (`appleib_remove_device()`, `appleib_forward_int_op()`, raw-event). So `i` is simply the wrong index here.

`appleib_find_dev_id_for_usage()` returns a pointer into `appleib_sub_hid_ids[]`, so the correct slot is `dev_id - appleib_sub_hid_ids` (`0` = Touch Bar, `1` = ALS).

### Proof

On a later teardown the corruption surfaces as a GPF whose "pointers" are **byte-for-byte the first 16 bytes of the live report descriptor** (a `hid_device`'s first member is `dev_rdesc`):

```
WARNING: ... lib/list_debug.c ... __list_del_entry_valid_or_report+0x...
Oops: general protection fault, probably for non-canonical address 0x25011503160a2005 [#1] SMP
RIP: 0010:remove_nodes.isra.0+0x...
Call Trace:
  hid_destroy_device+0x68/0x80
```

### The change

`apple-ibridge.c`, two hunks (full diff attached):

1. **`appleib_add_device()`** — index `sub_hdevs[]` by `idx = dev_id - appleib_sub_hid_ids` (in-bounds), warn only for unmatched **application** collections (silences the 5 benign nested-collection warnings), guard against a duplicate collection mapping to an already-filled slot, never store an `ERR_PTR` in the array, and clean up the full array on the error path. Correct indexing also means the **display sub-device is now actually destroyed** by `appleib_remove_device()` (previously it lived at the OOB index, so it was orphaned and UAF'd on parent unbind) and the `sub_open[]` flag lands on the right slot.
2. **raw-event forwarder** — read `sub_hdevs[i]` once and NULL/`IS_ERR`-guard it before `hid_input_report()` (a slot can be transiently unpopulated; never forward into a NULL/ERR device).

No behavior change on the normal cold-boot path: both sub-devices still register and bind; the bar lights as before, minus the 5 bogus warnings.

### Testing

- **Hardware:** MacBookPro13,2 (T1), Arch Linux, kernels 6.x and 7.0.10.
- **Before:** every boot logs 5 `Unknown collection` warnings; any iBridge teardown (`echo 0/1 > .../authorized`, `modprobe -r apple_touchbar`) GPFs in `remove_nodes()`/`hid_destroy_device()` or D-state-deadlocks `modprobe`; recovery is a hard reboot.
- **After:** warnings gone; the same `authorized` 0→1 power-cycle and full `modprobe -r`/reload run clean (no GPF, no D-state, process stays `R`), repeatedly. Touch Bar + ALS bind and function normally across reboots.
- Patch builds clean against 7.0.10 headers (both modules, no new warnings). Adversarial review (double-free / leak / cold-boot / OS-X-config duplicate-match) in the analysis doc.

### Note

This is a generic memory-safety bug — it isn't suspend-specific; that's just one reliable trigger. (Fixing it also unblocked an automatic post-hibernate Touch Bar relight downstream, but that's a separate system-integration change, not part of this PR.)

Credit to **Ronald Tschalär** (@roadrunner2) for the original driver.
