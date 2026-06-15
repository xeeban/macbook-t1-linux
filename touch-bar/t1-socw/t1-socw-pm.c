// t1-socw-pm.c — minimal platform driver that binds the iBridge ACPI device
// (APP7777 = \_SB.PCI0.XHC1.RHUB.ASOC) and power-cycles the T1 across system
// sleep via the ASOC.SOCW method — exactly what the stock apple-ibridge driver's
// suspend/resume do, but WITHOUT the HID half (so it doesn't claim the config-2
// digitizer / fight dfrd).
//
//   .probe   -> SOCW(1)   (ensure T1 powered on)
//   .suspend -> SOCW(0)   (power OFF the T1 — takes real effect because the USB
//                          device/port is already suspending at this point)
//   .resume  -> SOCW(1)   (power ON -> T1 COLD-BOOTS its firmware -> ALS servo
//                          re-initializes -> FULL brightness on resume)
//
// Our config-2 stack blacklists apple-ibridge, so APP7777 is otherwise unbound
// and the T1 is never power-cycled across sleep -> ALS stays dormant -> dim.
// This restores the stock power-cycle behavior. Works for BOTH S3 suspend and
// hibernate (the SOCW(0) rides the system-suspend power-down either way).
//
// Build: make    Load: sudo insmod t1-socw-pm.ko    Then: sudo systemctl suspend
// SAFE: identical power transition to the stock driver's every-sleep behavior.
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/acpi.h>

struct t1pm { acpi_handle socw; };

static int t1pm_socw(struct device *dev, struct t1pm *d, u64 v)
{
	acpi_status s = acpi_execute_simple_method(d->socw, NULL, v);
	if (ACPI_FAILURE(s)) {
		dev_warn(dev, "SOCW(%llu) failed: %s\n", v,
			 acpi_format_exception(s));
		return -EIO;
	}
	dev_info(dev, "SOCW(%llu) ok\n", v);
	return 0;
}

static int t1pm_probe(struct platform_device *pdev)
{
	struct t1pm *d;
	acpi_status s;

	d = devm_kzalloc(&pdev->dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;

	s = acpi_get_handle(ACPI_HANDLE(&pdev->dev), "SOCW", &d->socw);
	if (ACPI_FAILURE(s)) {
		dev_err(&pdev->dev, "cannot get SOCW handle: %s\n",
			acpi_format_exception(s));
		return -ENXIO;
	}

	platform_set_drvdata(pdev, d);
	t1pm_socw(&pdev->dev, d, 1);	/* ensure powered on */
	dev_info(&pdev->dev,
		 "bound APP7777; T1 SOCW power-cycle on suspend/resume enabled\n");
	return 0;
}

/* Modern dev_pm_ops — the legacy platform_driver.suspend/.resume fields are
 * NOT invoked by current kernels (verified: SOCW never fired at suspend). */
static int t1pm_suspend(struct device *dev)
{
	return t1pm_socw(dev, dev_get_drvdata(dev), 0);
}

static int t1pm_resume(struct device *dev)
{
	return t1pm_socw(dev, dev_get_drvdata(dev), 1);
}

/* DEFINE_SIMPLE_DEV_PM_OPS maps suspend/resume to ALL system-sleep transitions
 * (suspend, hibernate freeze/thaw/poweroff/restore) — so this covers S3 AND S4. */
static DEFINE_SIMPLE_DEV_PM_OPS(t1pm_pm_ops, t1pm_suspend, t1pm_resume);

static const struct acpi_device_id t1pm_acpi_match[] = {
	{ "APP7777", 0 },
	{ },
};
MODULE_DEVICE_TABLE(acpi, t1pm_acpi_match);

static struct platform_driver t1pm_driver = {
	.probe		= t1pm_probe,
	.driver		= {
		.name		  = "t1-socw-pm",
		.acpi_match_table = t1pm_acpi_match,
		.pm		  = pm_sleep_ptr(&t1pm_pm_ops),
	},
};
module_platform_driver(t1pm_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("xeeban");
MODULE_DESCRIPTION("T1 iBridge SOCW power-cycle across sleep (stock PM behavior, no HID)");
