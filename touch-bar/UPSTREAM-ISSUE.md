# Upstream issue draft — apple-ib-drv

> File against whichever `apple-ib-drv` fork you use (e.g. AdityaGarg8/apple-ib-drv, t2linux).
> The headline is the heap out-of-bounds write; the other two are noted as related. Copy from the `---` below.

---

**Title:** `appleib_add_device()`: heap out-of-bounds write to `sub_hdevs[]` corrupts memory on every T1 probe → GPF on any iBridge teardown

### Summary

On the T1 (MacBookPro13,2 and similar — iBridge `05ac:8600`), `appleib_add_device()` indexes the fixed-size `sub_hdevs[]` array by the **raw HID collection index**, but that array only has `ARRAY_SIZE(appleib_sub_hid_ids)` (= 2) slots. The T1's combined display/ALS HID interface has **7 collections**, so the driver writes a `struct hid_device *` to `sub_hdevs[6]` — well past the end of the `devm`-allocated `appleib_hid_dev_info`. This is a **heap out-of-bounds write that happens on every probe / every boot**. It silently corrupts the adjacent slab object; the latent corruption then manifests as a **general-protection fault on any later teardown** of the iBridge (driver unload, USB unbind/re-enumerate, or suspend/resume teardown).

This is a generic memory-safety bug — it is **not** specific to suspend/hibernate; that's just one path that reliably trips it.

### Affected

- **Hardware:** T1 iBridge Macs — `05ac:8600` demuxed to virtual HIDs `1d6b:0301` (Touch Bar) + `1d6b:0302` (ALS). Reproduced on MacBookPro13,2.
- **Driver:** out-of-tree `apple-ib-drv`, `apple_ibridge` / `apple-ibridge.c`, rev r307 (DKMS). The offending code is present in current forks.
- **Kernel:** observed on 6.x and 7.0.10 (Arch). Independent of kernel version.

### Root cause

`appleib_add_device()` (in `apple-ibridge.c`):

```c
struct appleib_hid_dev_info {
    ...
    struct hid_device *sub_hdevs[ARRAY_SIZE(appleib_sub_hid_ids)];  /* only 2 slots */
};

static struct appleib_hid_dev_info *appleib_add_device(struct hid_device *hdev)
{
    ...
    for (i = 0; i < hdev->maxcollection; i++) {           /* iterates ALL collections (7 on T1) */
        usage  = hdev->collection[i].usage;
        dev_id = appleib_find_dev_id_for_usage(usage);
        if (!dev_id) {
            hid_warn(hdev, "Unknown collection ... usage %x\n", usage);   /* the 5 nested ones */
        } else {
            hdev_info->sub_hdevs[i] = appleib_add_sub_dev(hdev_info, dev_id);  /* <-- OOB: i can be 6 */
            ...
        }
    }
    ...
}
```

On the T1 display/ALS interface the report descriptor has **7 collections**: the ALS at index `0`, **five nested sensor sub-collections** (these are the familiar `Unknown collection encountered with usage 2003xx/2002xx` warnings logged on every boot), and the **Touch Bar display at index 6**. The match for the display therefore stores its `hid_device *` at `sub_hdevs[6]`, i.e. **24 bytes past the end of the 32-byte allocation**, clobbering the adjacent `devres` bookkeeping node in the slab.

`appleib_remove_device()` and the report forwarders (`appleib_forward_int_op()`, raw-event) correctly iterate `i < ARRAY_SIZE(sub_hdevs)`, so the *array* is only ever 2 slots everywhere except this write — confirming `i` is the wrong index here.

### Proof it's the descriptor being written as a pointer

When a teardown later walks the corrupted devres list, it GPFs with non-canonical "pointers" that are **byte-for-byte the first 16 bytes of the live HID report descriptor** (a `hid_device`'s first member is `dev_rdesc`):

```
WARNING: ... lib/list_debug.c ... __list_del_entry_valid_or_report+0x...
Oops: general protection fault, probably for non-canonical address 0x25011503160a2005 [#1] SMP
RIP: 0010:remove_nodes.isra.0+0x...
Call Trace:
 hid_destroy_device+0x68/0x80
 ...
```

(`0x25011503160a2005`, `0x018501a141092005` etc. decode to the descriptor's opening `Usage Page (Digitizer) … ` bytes.)

### Reproduce

On a T1 (after the bar is otherwise working):

```sh
# Any teardown of the iBridge will GPF (here: a USB re-enumerate):
echo 0 | sudo tee /sys/bus/usb/devices/<iBridge>/authorized
echo 1 | sudo tee /sys/bus/usb/devices/<iBridge>/authorized
# -> general protection fault in remove_nodes()/hid_destroy_device(); a `modprobe -r apple_touchbar`
#    can instead D-state-deadlock the unkillable modprobe. Recovery is a hard reboot.
```

The five `Unknown collection encountered with usage 200...` lines in `dmesg` on every boot are the tell that `maxcollection > ARRAY_SIZE(sub_hdevs)`.

### Fix

Index `sub_hdevs[]` by the **matched sub-device-id slot**, not the raw collection index. `appleib_find_dev_id_for_usage()` returns a pointer into `appleib_sub_hid_ids[]`, so `dev_id - appleib_sub_hid_ids` is the slot (`0` = Touch Bar, `1` = ALS) and is in-bounds by construction:

```c
for (i = 0; i < hdev->maxcollection; i++) {
    usage  = hdev->collection[i].usage;
    dev_id = appleib_find_dev_id_for_usage(usage);
    if (!dev_id) {
        /* only application collections are candidates; the nested sensor
         * sub-collections are expected — don't warn about them */
        if (hdev->collection[i].type == HID_COLLECTION_APPLICATION)
            hid_warn(hdev, "Unknown collection ... usage %x\n", usage);
        continue;
    }

    idx = dev_id - appleib_sub_hid_ids;          /* {0,1}, in-bounds */
    if (hdev_info->sub_hdevs[idx])               /* dup-collection guard */
        continue;

    sub_hdev = appleib_add_sub_dev(hdev_info, dev_id);
    if (IS_ERR(sub_hdev)) {
        /* clean up all populated slots */
        ...
        return ERR_CAST(sub_hdev);
    }
    hdev_info->sub_hdevs[idx] = sub_hdev;          /* never store an ERR_PTR */
}
```

While here, two adjacent robustness fixes (same patch): NULL/ERR-guard the forwarder's `sub_hdevs[i]` deref, and destroy the display sub-device in `appleib_remove_device()` (it was the orphaned slot, so it leaked + UAF'd on parent unbind). Full diff and a line-by-line analysis with the adversarial review are here: <link to ibridge-teardown-fix.preview.diff / IBRIDGE-TEARDOWN-UAF-ANALYSIS.md>.

### Two related bugs found alongside (separate, lower priority)

1. **`appletb_set_tb_disp()` rides the stale `usbhid` interrupt-OUT queue across suspend** — it sends the display report via `hid_hw_request()` (async, through the iBridge usbhid queue), which goes stale across hibernate so the SET_REPORT is silently dropped; a `-32`/-EPIPE also shows at cold boot. Routing it through a synchronous `hid_hw_raw_request()` (like `set_tb_mode`'s direct control transfer) fixes it.
2. **Post-hibernate the Touch Bar firmware stays dark** even with a successful display command; the only host-side relight we found is a **full `apple_ibridge` stack reload** on resume (re-create the virtual HIDs + fresh probe). This is only safe *after* the OOB above is fixed (before it, the reload's `modprobe -r` is exactly what GPFs/deadlocks). Writeup + an automated post-resume hook available if useful.

---

*Reported from the [macbook-t1-linux](https://github.com/xeeban/macbook-t1-linux) project (T1 MacBookPro13,2 on Arch). The OOB root cause was isolated by an automated kernel-forensics pass; happy to provide the full crash logs, the report descriptor dump, or a PR.*
