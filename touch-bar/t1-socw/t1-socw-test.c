// t1-socw-test.c — one-shot T1 (iBridge) power-cycle via the ASOC.SOCW ACPI
// method, to test whether a T1 cold-boot restores FULL Touch Bar brightness
// after sleep (the thing the stock apple-ibridge driver does on suspend/resume,
// and which our config-2 stack skips because it blacklists apple-ibridge).
//
//   SOCW(0) = power OFF the T1   (stock calls this on suspend)
//   SOCW(1) = power ON  the T1   (stock calls this on resume -> T1 cold-boots)
//
// On insmod this pulses SOCW(0) -> 2s -> SOCW(1). The USB device 05ac:8600 will
// disconnect then re-enumerate; apple_dfr_cfgsel re-selects config 2, appletbdrm
// re-probes, and the udev SYSTEMD_WANTS restarts dfrd — same as a fresh boot.
// If the T1 truly cold-boots, the bar should come back at FULL brightness.
//
// Build: make   (needs kernel headers; DKMS already works on this box)
// Test:  sudo insmod t1-socw-test.ko   (watch the bar; then: sudo rmmod t1_socw_test)
//        dmesg | tail to see the SOCW results.
// SAFE: this is exactly the power transition the stock driver performs every
// sleep cycle. A reboot recovers from any wedge.
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/acpi.h>
#include <linux/delay.h>

#define SOCW_PATH "\\_SB_.PCI0.XHC1.RHUB.ASOC.SOCW"

static int socw(acpi_handle h, u64 arg)
{
	acpi_status s = acpi_execute_simple_method(h, NULL, arg);
	if (ACPI_FAILURE(s)) {
		pr_warn("t1-socw: SOCW(%llu) failed: %s\n", arg,
			acpi_format_exception(s));
		return -EIO;
	}
	pr_info("t1-socw: SOCW(%llu) ok\n", arg);
	return 0;
}

static int __init t1socw_init(void)
{
	acpi_handle h;
	acpi_status s;

	s = acpi_get_handle(NULL, SOCW_PATH, &h);
	if (ACPI_FAILURE(s)) {
		pr_err("t1-socw: cannot resolve %s: %s\n", SOCW_PATH,
		       acpi_format_exception(s));
		return -ENXIO;
	}

	pr_info("t1-socw: power-cycling the T1 (SOCW 0 -> 1)\n");
	socw(h, 0);
	msleep(2000);
	socw(h, 1);
	pr_info("t1-socw: done; T1 should re-enumerate. rmmod me.\n");
	return 0;
}

static void __exit t1socw_exit(void) { }

module_init(t1socw_init);
module_exit(t1socw_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("xeeban");
MODULE_DESCRIPTION("One-shot T1 iBridge SOCW power-cycle (Touch Bar relight test)");
