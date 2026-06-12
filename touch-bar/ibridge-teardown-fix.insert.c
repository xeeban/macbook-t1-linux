/*
 * ibridge-teardown-fix.insert.c
 *
 * Fix for the apple_ibridge teardown GPF (heap OOB write planted at probe).
 * See IBRIDGE-TEARDOWN-UAF-ANALYSIS.md for the full root-cause writeup.
 *
 * Applied by patch-ibridge-teardown-and-build.sh to
 * /usr/src/apple-ib-drv-r307.4afd309/apple-ibridge.c
 *
 * SECTION 1 replaces the whole appleib_add_device() function
 *   (everything between its signature line and the
 *    "static void appleib_remove_device" line).
 * SECTION 2 replaces the forwarding loop inside appleib_hid_raw_event().
 */

/* ===8<=== SECTION 1: appleib_add_device (full function replacement) ===8<=== */
static struct appleib_hid_dev_info *appleib_add_device(struct hid_device *hdev)
{
	struct appleib_hid_dev_info *hdev_info;
	struct hid_device *sub_hdev;
	struct hid_device_id *dev_id;
	unsigned int usage;
	size_t idx;
	int i;

	hdev_info = devm_kzalloc(&hdev->dev, sizeof(*hdev_info), GFP_KERNEL);
	if (!hdev_info)
		return ERR_PTR(-ENOMEM);

	hdev_info->hdev = hdev;

	for (i = 0; i < hdev->maxcollection; i++) {
		usage = hdev->collection[i].usage;
		dev_id = appleib_find_dev_id_for_usage(usage);

		if (!dev_id) {
			/*
			 * Only application (top-level) collections can map to
			 * sub-devices; nested collections (e.g. the ALS's five
			 * LOGICAL sensor sub-collections, usages 0x0020yyyy)
			 * are expected -- don't warn about them.
			 */
			if (hdev->collection[i].type == HID_COLLECTION_APPLICATION)
				hid_warn(hdev,
					 "Unknown collection encountered with usage %x\n",
					 usage);
			continue;
		}

		/*
		 * Index sub_hdevs[] by the slot of the MATCHED id in
		 * appleib_sub_hid_ids (0 = Touch Bar, 1 = ALS), NOT by the
		 * raw collection index i: sub_hdevs[] only has
		 * ARRAY_SIZE(appleib_sub_hid_ids) (= 2) entries, while
		 * hdev->maxcollection counts EVERY collection in the report
		 * descriptor, including nested ones. On the T1's combined
		 * display/ALS interface the descriptor has 7 collections
		 * (ALS at [0], five nested sensor collections, the Touch Bar
		 * display at [6]), so indexing by i wrote sub_hdevs[6] --
		 * 24 bytes past the end of this devm allocation -- planting
		 * a hid_device pointer on top of the adjacent devres node in
		 * the slab. devres_release_group() then walked that pointer
		 * as a list node on driver unbind and GPF'd inside
		 * remove_nodes() (with report-descriptor bytes showing up as
		 * "list pointers"), crashing every iBridge teardown or USB
		 * re-enumeration. It also meant the display sub-device was
		 * never destroyed by appleib_remove_device() (orphan + UAF).
		 */
		idx = dev_id - appleib_sub_hid_ids;

		if (hdev_info->sub_hdevs[idx]) {
			hid_warn(hdev,
				 "Duplicate collection with usage %x for sub-device slot %zu; ignoring\n",
				 usage, idx);
			continue;
		}

		sub_hdev = appleib_add_sub_dev(hdev_info, dev_id);

		if (IS_ERR(sub_hdev)) {
			for (idx = 0; idx < ARRAY_SIZE(hdev_info->sub_hdevs); idx++) {
				if (hdev_info->sub_hdevs[idx]) {
					hid_destroy_device(hdev_info->sub_hdevs[idx]);
					hdev_info->sub_hdevs[idx] = NULL;
				}
			}
			return ERR_CAST(sub_hdev);
		}

		/*
		 * Store only a successfully added sub-device: an ERR_PTR must
		 * never be visible in sub_hdevs[] to raw_event/PM forwarders.
		 */
		hdev_info->sub_hdevs[idx] = sub_hdev;
	}

	return hdev_info;
}

/* ===8<=== SECTION 2: appleib_hid_raw_event forwarding loop ===8<=== */
	for (i = 0; i < ARRAY_SIZE(hdev_info->sub_hdevs); i++) {
		struct hid_device *sub_hdev =
			READ_ONCE(hdev_info->sub_hdevs[i]);

		/*
		 * NULL/ERR-guard: a sub_open[] flag can be set (via the
		 * probe-time first-free-slot fallback in appleib_set_open())
		 * just before the matching sub_hdevs[] slot is filled in, and
		 * a destroyed sub-device's slot is NULLed in
		 * appleib_remove_device(). Never forward into a NULL/ERR
		 * sub-device.
		 */
		if (sub_hdev && !IS_ERR(sub_hdev) &&
		    READ_ONCE(hdev_info->sub_open[i]))
			hid_input_report(sub_hdev, report->type,
					 data, size, 0);
	}
