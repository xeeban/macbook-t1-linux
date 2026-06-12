# apple_ibridge Teardown GPF — Root-Cause Analysis (overnight run, 2026-06-11→12)

**Status: ROOT CAUSE FOUND AND PROVEN.** It is not a use-after-free in the teardown path at all —
it is a **heap out-of-bounds WRITE at probe time** (every boot), which plants a landmine in the
device's devres list. Every teardown then detonates it. The crash registers were matched
**byte-for-byte** against this machine's live report descriptor (see §4 — this is as close to a
court-room proof as kernel debugging gets without KASAN).

Bug is present in pristine upstream r307 (`apple-ibridge.c.orig` has identical indexing); it was
not introduced by any of our local patches.

- Driver source analyzed: `/usr/src/apple-ib-drv-r307.4afd309/apple-ibridge.c` (r307 + local resume fixes)
- Kernel: `7.0.10-arch1-1`; crash record: `journalctl -b -4` @ Jun 11 20:13:50
- Fix: [`ibridge-teardown-fix.insert.c`](ibridge-teardown-fix.insert.c), applied by
  [`patch-ibridge-teardown-and-build.sh`](patch-ibridge-teardown-and-build.sh)
- Test/apply steps: [`MORNING-PLAN.md`](MORNING-PLAN.md)

---

## 1. The buggy code

`struct appleib_hid_dev_info` (apple-ibridge.c:89–93):

```c
struct appleib_hid_dev_info {
	struct hid_device	*hdev;
	struct hid_device	*sub_hdevs[ARRAY_SIZE(appleib_sub_hid_ids)];   /* == 2 */
	bool			sub_open[ARRAY_SIZE(appleib_sub_hid_ids)];     /* == 2 */
};
```

x86-64 layout: `hdev` @0, `sub_hdevs[0]` @8, `sub_hdevs[1]` @16, `sub_open[0..1]` @24–25,
padding to **sizeof = 32**. Allocated with `devm_kzalloc(&hdev->dev, 32, …)` (line 425).

`appleib_add_device()` (apple-ibridge.c:418–453) — the fatal loop:

```c
	for (i = 0; i < hdev->maxcollection; i++) {                 /* line 431 */
		usage = hdev->collection[i].usage;
		dev_id = appleib_find_dev_id_for_usage(usage);
		if (!dev_id) {
			hid_warn(hdev, "Unknown collection encountered with usage %x\n", usage);
		} else {
			hdev_info->sub_hdevs[i] = appleib_add_sub_dev(hdev_info, dev_id);  /* line 439: OOB WRITE */
			...
```

**The array is indexed by the raw collection index `i`, but is sized by the number of distinct
sub-device IDs (2).** `hdev->maxcollection` counts *every* collection in the report descriptor —
including *nested* ones — not just top-level application collections.

Meanwhile `appleib_remove_device()` (455–471), `appleib_hid_raw_event()` (101–105),
`appleib_forward_int_op()` (183–201) and `appleib_set_open()` (269–283) all iterate only
`i < ARRAY_SIZE(sub_hdevs)` = 2.

## 2. Ground truth from the live machine

The iBridge (05ac:8600, config 1) exposes two HID interfaces to this driver:

| Parent hdev | USB iface | descriptor | collections (decoded from `/sys/.../report_descriptor`) |
|---|---|---|---|
| `0003:05AC:8600.0003` | 1-3:1.2 | 83 B | `coll[0]` APPLICATION usage `0x00010006` (TB mode/keys) → maxcollection = **1** |
| `0003:05AC:8600.0005` | 1-3:1.3 | 634 B | see below → maxcollection = **7** |

Parent `.0005`'s 634-byte descriptor, decoded collection-by-collection:

```
coll[0] depth=0 type=APPLICATION usage=0x00200041   ← ALS            → matches sub id [1]
coll[1] depth=1 type=LOGICAL     usage=0x00200316   ← nested sensor  → no match
coll[2] depth=1 type=LOGICAL     usage=0x00200309   ← nested sensor  → no match
coll[3] depth=1 type=LOGICAL     usage=0x00200319   ← nested sensor  → no match
coll[4] depth=1 type=LOGICAL     usage=0x00200201   ← nested sensor  → no match
coll[5] depth=1 type=LOGICAL     usage=0x00200202   ← nested sensor  → no match
coll[6] depth=0 type=APPLICATION usage=0xFF120001   ← TB DISPLAY     → matches sub id [0]
```

Corroborated by the journal at **every single boot** (e.g. current boot 20:53:54): exactly five
`Unknown collection encountered with usage 2003xx/2002xx` warnings — those are `coll[1..5]`,
which proves the loop walks indices 1–5 and therefore reaches **`i = 6`**. Also corroborated by
sub-device creation order in sysfs: ALS `1D6B:0302.0006` was created *before* TB-display
`1D6B:0301.0007` (ALS is `coll[0]`, display is `coll[6]`).

So on parent `.0005`, every boot:

- `coll[0]` (ALS) → `sub_hdevs[0] = ALS sub_hdev` — in bounds;
- `coll[6]` (TB display) → **`sub_hdevs[6] = TB-display sub_hdev`** — offset 8 + 6×8 = **56**,
  i.e. **24 bytes past the end of the 32-byte devm allocation**. 8-byte heap OOB write of a
  `struct hid_device *`.

## 3. What the OOB write lands on

`devm_kzalloc` prepends a `struct devres` header (`devres_node` = `list_head entry`(16) +
`dr_release_t release`(8) + `name`(8) + `size`(8) = 40 bytes on this kernel), so the allocation is
40 + 32 = 72 bytes → **kmalloc-96 slab**. The OOB write hits `data+56` = `devres+96` = **the first
8 bytes of the adjacent kmalloc-96 slab object**.

Who is the neighbour? Immediately before `appleib_hid_probe()` runs, the driver core's
`really_probe()` calls `devres_open_group()`, which allocates a `struct devres_group`
(2×devres_node + id + color ≈ 92 B → also **kmalloc-96**). The group node and our `hdev_info`
devres are allocated back-to-back from the same per-CPU slab during the same probe, so they are
adjacent with high probability. **The first 8 bytes of a devres node/group are `entry.next` — a
`list_head` pointer.** The OOB write replaces a devres list pointer with the TB-display
`hid_device *`.

This is exactly what the crash backtrace shows being walked:
`hid_destroy_device(parent) → device_del → device_release_driver_internal → hid_device_remove →
devres_release_group → remove_nodes → __list_del_entry_valid_or_report` — the **probe devres
group of parent `.0005`** being released at driver unbind.

*(Confidence: the OOB write and its size/offset are proven; the precise identity of the neighbour
object is a high-confidence inference — slab adjacency is probabilistic, which is also why the
symptom occasionally varied between WARNING, GPF, and D-state wedge across attempts.)*

## 4. The smoking gun: crash registers ARE the report descriptor

Crash record (Jun 11 20:13:50, `journalctl -b -4`, process `51-touchbar-rel` writing
`authorized=0`):

```
WARNING lib/list_debug.c:62 __list_del_entry_valid_or_report  ... RCX/RDX = 018501a141092005
Oops: general protection fault, probably for non-canonical address 0x25011503160a2005
RIP: remove_nodes.isra.0+0x39
RAX: 25011503160a2005   RCX: 018501a141092005   RSI: 018501a141092005
Call Trace: remove_nodes ← devres_release_group ← hid_device_remove ←
            device_release_driver_internal ← device_del ← hid_destroy_device ←
            usbhid_disconnect ← usb_unbind_interface ← ... ← usb_disable_device
```

First 16 bytes of the live `.0005` report descriptor (`xxd`):

```
00000000: 0520 0941 a101 8501  0520 0a16 0315 0125
```

Read as little-endian u64s:

- bytes 0–7 `05 20 09 41 a1 01 85 01` = **0x018501a141092005** — the crash's RCX/RSI/RDX
- bytes 8–15 `05 20 0a 16 03 15 01 25` = **0x25011503160a2005** — the crash's RAX **and the GPF
  fault address**

Mechanism, hop by hop:

1. `remove_nodes()` walks the corrupted devres list and reads the planted `entry.next` = the
   TB-display `hid_device *`.
2. It treats that `hid_device` as a `devres_node`. The **first member of `struct hid_device` is
   `const __u8 *dev_rdesc`** (linux/hid.h:640–641 on this kernel) — for the TB-display sub-device
   this points at the kmemdup'd copy of the parent's 634-byte descriptor (made by
   `appleib_ll_parse()` → `hid_parse_report()`). So `entry.next` of the fake node = pointer to
   descriptor bytes.
3. It hops there and reads the descriptor's first 16 bytes as `{next, prev}` =
   `{0x018501a141092005, 0x25011503160a2005}` → `__list_del_entry_valid_or_report` fires the
   WARNING printing exactly those values, then the walker dereferences
   `0x25011503160a2005` → **GPF (non-canonical)**.

Every value in the crash record is accounted for. Root cause: **the `sub_hdevs[6]` out-of-bounds
write in `appleib_add_device()`**.

## 5. Why it "only" crashes on teardown (and why hibernate is irrelevant)

The corruption is planted at probe — **every cold boot, immediately, in the success path**. The
clobbered 8 bytes belong to devres bookkeeping that is only *read* when the parent device's
driver is unbound or the device deleted. Cold boots never unbind, so the machine runs fine.
The moment *anything* tears down parent `.0005` — `authorized` 0-write, `modprobe -r`, sysfs
unbind, USB re-enumeration — `devres_release_group()` walks the planted pointer and GPFs.

This precisely explains the empirical record in
`../sleep-resume/TOUCHBAR-RELIGHT-ANALYSIS.md`: *"every USB teardown of the iBridge crashes
(GPF) or deadlocks"* — including on a perfectly live, never-hibernated endpoint. The
half-dead post-hibernate endpoint was never the cause of the GPF (it is a separate, bounded
concern — see §8).

## 6. Secondary defects found (same root: collection-index vs slot mismatch)

1. **TB-display sub-device is never destroyed.** `appleib_remove_device()` (455–471) only walks
   slots 0–1; the display sub-device lives in phantom "slot 6" (i.e. nowhere). Even when the GPF
   is dodged, the display sub-device survives its parent as an orphan: registered, driver bound,
   `driver_data` pointing at `hdev_info` which devres frees at parent unbind → genuine
   use-after-free on any subsequent `ll_*` call through it, plus a leak.
2. **`sub_open` flag mis-slotting.** During the display sub-device's probe, `hid_hw_open()` →
   `appleib_set_open()` (264–286) can't find it in `sub_hdevs[]` (it's never stored in 0–1), so
   the "first unset slot" fallback marks `sub_open[1] = true` while `sub_hdevs[1] == NULL`.
   `appleib_hid_raw_event()` (101–105) then calls `hid_input_report(NULL, …)` for every parent
   input report — survivable only because modern hid-core NULL-checks, and input reports for the
   display sub-device are silently dropped.
3. **ERR_PTR transiently stored in `sub_hdevs[i]`** (line 439–441) where `raw_event` (no
   IS_ERR guard) could see it.
4. **Log noise**: the five per-boot "Unknown collection" warnings are nested LOGICAL collections
   that can never match — the warning should only fire for APPLICATION collections.

## 7. The fix (see `ibridge-teardown-fix.insert.c`)

**Index `sub_hdevs[]` by the matched sub-device-ID slot, not by collection index:**

```c
idx = dev_id - appleib_sub_hid_ids;     /* 0 = Touch Bar, 1 = ALS — provably < ARRAY_SIZE */
```

plus: skip-and-warn on duplicate slot match, store the new sub-device only after
`appleib_add_sub_dev()` succeeds (never an ERR_PTR), full-array cleanup on error, warn only for
unmatched APPLICATION collections, and a NULL/ERR guard in `appleib_hid_raw_event()`.

`idx` is computed from the pointer returned by `appleib_find_dev_id_for_usage()`, which can only
be `&appleib_sub_hid_ids[0]` or `[1]` (370–380), and `sub_hdevs[]` is sized by
`ARRAY_SIZE(appleib_sub_hid_ids)` — **in-bounds by construction**.

Post-fix behavior on this machine:

- parent `.0003`: TB-mode sub at slot 0 (unchanged).
- parent `.0005`: ALS at slot **1**, TB-display at slot **0** (slots swap vs. today's 0/phantom-6;
  nothing consumes slot positions externally — `appletb`/`hid-generic` only see the hid_devices).
- `appleib_remove_device()` now destroys **both** sub-devices of `.0005` → no orphan, no leak,
  no planted pointer → teardown/re-enumeration is memory-safe.

### Why it cannot regress cold boot

- Same number of sub-devices created, in the same order (ALS then display — creation order
  follows collection order, unchanged), same names, same ids → udev/driver matching unchanged.
- The probe-time `hid_hw_open` "first unset slot" fallback still resolves correctly: when the
  display sub-device probes (it is created *after* ALS is stored in slot 1), slot 0 is the first
  unset slot — exactly where the display device is then stored. Verified for both parents.
- The APPLICATION-type filter only affects *warnings* for collections that can never match the
  usage map (all four mapped usages live on APPLICATION collections — verified in the decoded
  descriptors above).
- Error-path cleanup now NULLs slots after destroy → idempotent vs. `appleib_remove_device`,
  no double-destroy.

### Adversarial self-review (attempts to break the patch)

| Attack | Outcome |
|---|---|
| Two collections on one parent matching the *same* dev_id (e.g. OS X config: `0x00010006` + `0x000D0005` both → TB) | Duplicate-slot guard skips the second with a warning; no overwrite-leak. (Driver forces config 1 anyway, apple-ibridge.c:483–486.) |
| Naive alternative "clamp `i` to 2" | **Rejected**: would silently never create the display sub-device (`coll[6]`) → permanently dark Touch Bar. This is why slot-indexing is the only minimal correct fix. |
| Double-destroy: error path then `appleib_remove_device` | Error path destroys + NULLs and returns ERR; probe then never calls remove with those subs (`hid_set_drvdata` happens only on success, line 506); remove skips NULLs. |
| ALS-driver opening during *its* probe (before slot stored) | `sub_open[0]` would be mis-flagged (TB-display slot). hid-generic does not open at probe, so unreachable today; raw_event NULL-guard makes it harmless regardless. Residual quirk inherited from r307's fallback design — documented, not worth more churn. |
| Report arriving between `ll_open` (flag set via fallback) and slot store | raw_event NULL/ERR guard drops it; previously this was a `hid_input_report(NULL)` call. |
| Leak check | Every successfully created sub lands in a guarded slot ∈ {0,1} and is destroyed exactly once (remove or error path). The pre-fix leak of the display sub is *fixed*. |
| Compile/style | Verified by an out-of-tree test build of the patched source against `7.0.10-arch1-1` headers (see build log note in MORNING-PLAN); kernel style, tabs, no new includes (`HID_COLLECTION_APPLICATION`, `ERR_CAST` already available via linux/hid.h + linux/err.h). |

## 8. What this fix does NOT claim to solve

The **D-state wedge** of 2026-06-11 19:24 (drain + deauthorize against the half-dead post-resume
endpoint) is a distinct hazard: `appletb_remove`'s `cancel_delayed_work_sync` + post-cancel USB
commands against a non-responsive endpoint. Notes:

- All USB commands in those paths are now time-bounded (`usb_control_msg` 2000 ms timeouts;
  `-EPIPE`-only retry loops, ≤5 tries) — a dead endpoint returns `-ENODEV/-EPROTO` immediately,
  it does not STALL — so the *driver* side should not park in D forever.
- Part of that night's wedge cascade is plausibly downstream of this same memory corruption
  (a kworker that GPF'd/blocked while holding USB locks drags everything into D). With the
  corruption gone, teardown behavior is at least *sane*; whether the half-dead-endpoint
  power-cycle is fully clean must be proven by the staged tests in MORNING-PLAN (live-endpoint
  cycle first, then post-hibernate).

## 9. Bottom line

One line of bad indexing (`sub_hdevs[i]` with `i` ∈ [0, maxcollection) instead of the matched
2-slot id index) produces, on this exact hardware, a deterministic 8-byte heap OOB write at every
boot, which corrupts the parent HID device's devres group and makes **every** teardown of the
iBridge crash — which is the sole blocker between us and the proven relight mechanism
(USB re-enumeration). Fix the indexing → teardown becomes safe → post-resume power-cycle relight
becomes viable.
