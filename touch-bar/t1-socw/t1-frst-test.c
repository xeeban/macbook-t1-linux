// t1-frst-test.c — invoke the iBridge ASOC.FRST ACPI method to HARDWARE-RESET
// the T1 SOC, forcing a cold firmware boot. This is the real cold-boot lever:
//
//   Method (FRST) {  \_SB.PCI0.LPCB.EC.SOCR = 1; Sleep(15ms);
//                    \_SB.PCI0.LPCB.EC.SOCR = 0; Sleep(600ms); }
//
// i.e. it toggles the Embedded Controller's SOCR ("SOC Reset") line — a PHYSICAL
// reset of the T1 chip, unlike SOCW which only posts a soft MBOX message to the
// running firmware. A real T1 reset should re-run its boot-time init, including
// the ambient-light->backlight brightness servo => FULL brightness.
//
// On insmod: call FRST() once. The T1 resets; USB 05ac:8600 (bus 1-3) should
// DISCONNECT and RE-ENUMERATE (cold boot); apple_dfr_cfgsel re-selects config 2,
// appletbdrm freshly probes, udev restarts dfrd. Watch the backlight.
//
// Build: make    Test: sudo insmod t1-frst-test.ko ; sudo rmmod t1_frst_test
// NOTE: FRST is the bare reset (no DFU GPIO), the same primitive macOS firmware
// uses. Touch Bar/Touch ID drop for ~1-2s during the reset. Reboot recovers.
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/acpi.h>

#define FRST_PATH "\\_SB_.PCI0.XHC1.RHUB.ASOC.FRST"

static int __init frst_init(void)
{
	acpi_handle h;
	acpi_status s;

	s = acpi_get_handle(NULL, FRST_PATH, &h);
	if (ACPI_FAILURE(s)) {
		pr_err("t1-frst: cannot resolve %s: %s\n", FRST_PATH,
		       acpi_format_exception(s));
		return -ENXIO;
	}

	pr_info("t1-frst: calling FRST() — hardware-resetting the T1 SOC via EC.SOCR\n");
	s = acpi_evaluate_object(h, NULL, NULL, NULL);
	if (ACPI_FAILURE(s)) {
		pr_warn("t1-frst: FRST() failed: %s\n", acpi_format_exception(s));
		return -EIO;
	}
	pr_info("t1-frst: FRST() ok; T1 should reset + re-enumerate. rmmod me.\n");
	return 0;
}

static void __exit frst_exit(void) { }

module_init(frst_init);
module_exit(frst_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("xeeban");
MODULE_DESCRIPTION("One-shot T1 iBridge hardware reset via ASOC.FRST (EC.SOCR)");
