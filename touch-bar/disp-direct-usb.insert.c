	if (tb_dev->is_t1) {
		/*
		 * Send the display report via a SYNCHRONOUS hid_hw_raw_request()
		 * instead of hid_hw_request(). hid_hw_request() queues the report
		 * on the iBridge's usbhid interrupt-OUT URB, an async path that
		 * goes stale across hibernate (reset_resume never re-inits it) and
		 * silently drops the SET_REPORT -> dark Touch Bar on resume.
		 * hid_hw_raw_request() bottoms out in a usb_control_msg SET_REPORT
		 * on EP0 (a CLASS request -- NOT the vendor quirk set_tb_mode uses),
		 * which survives hibernate for the same reason set_tb_mode's direct
		 * control transfer does, and returns a real error code. Retried on
		 * -EPIPE like set_tb_mode (the T1 firmware transiently STALLs).
		 */
		int rlen = hid_report_len(report);
		u8 *rbuf = hid_alloc_report_buf(report, GFP_KERNEL);

		if (rbuf) {
			int tries = 0;

			hid_output_report(report, rbuf);
			do {
				rc = hid_hw_raw_request(tb_dev->disp_iface.hdev,
							report->id, rbuf, rlen,
							report->type,
							HID_REQ_SET_REPORT);
				if (rc != -EPIPE)
					break;
				usleep_range(1000 << tries, 3000 << tries);
			} while (++tries < 5);

			if (rc < 0)
				dev_err(tb_dev->log_dev,
					"Failed to set touch bar display to %u (%d)\n",
					disp, rc);
			kfree(rbuf);
		} else {
			rc = -ENOMEM;
		}
	} else {
		hid_hw_request(tb_dev->disp_iface.hdev, report, HID_REQ_SET_REPORT);
	}
