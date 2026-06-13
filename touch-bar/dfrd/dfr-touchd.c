// SPDX-License-Identifier: GPL-2.0
/*
 * dfr-touchd — T1 Touch Bar touch daemon: hidraw -> uinput.
 *
 * In USB config 2 ("display mode") the iBridge digitizer is a NON-standard
 * HID: a ~52-byte interrupt report (EP 0x83, USB interface 2) whose first
 * 4 bytes are a little-endian float32 X position in [0.5, 1.0] across the
 * bar (left edge 0.500, right edge ~0.997). hid-generic owns the interface
 * and exposes /dev/hidrawN; hidraw hands us the raw reports WITHOUT
 * unbinding the kernel driver — so this coexists with the appletbdrm
 * display stack.
 *
 * Pipeline (ported from the proven dfr-switch.c "buttons" stage):
 *   read report -> x = float32 @ offset 0 -> nx = (x - 0.5) * 2
 *   -> zone via dfr_zone_from_nx() (SAME math dfr-render uses to draw)
 *   -> inject KEY down on first contact, KEY up on release
 * Release = no report for RELEASE_MS (the firmware streams reports while a
 * finger is down; dfr-switch detected up via interrupt-transfer timeout) or
 * an explicit out-of-range X (< 0.45).
 *
 * The digitizer hidraw node is AUTO-DETECTED by walking /sys/class/hidraw:
 * the right node's HID device sits on USB interface 2 of the 05ac:8600
 * device while bConfigurationValue == 2. Override with -d /dev/hidrawN.
 *
 * Run as root (needs /dev/hidrawN + /dev/uinput).
 * SIGUSR1 cycles layout in lock-step with dfr-render (send to both).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <dirent.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <linux/uinput.h>
#include <linux/hidraw.h>

#include "dfr-layout.h"

#define USB_VID "05ac"
#define USB_PID "8600"
#define DIGITIZER_IFACE 2     /* EP 0x83 lives on config-2 interface 2 */
#define FALLBACK_IFACE  6     /* second config-2 HID interface (EP 0x87) */
#define RELEASE_MS 150        /* no report for this long => finger up */

static volatile sig_atomic_t g_stop, g_cycle;
static void on_stop(int s) { (void)s; g_stop = 1; }
static void on_usr1(int s) { (void)s; g_cycle = 1; }

/* ------------------------------------------------------------------ */
/* sysfs helpers                                                       */
/* ------------------------------------------------------------------ */
static int read_sysfs(const char *dir, const char *file, char *buf, size_t len)
{
	char p[PATH_MAX];
	snprintf(p, sizeof p, "%s/%s", dir, file);
	FILE *f = fopen(p, "r");
	if (!f)
		return -1;
	if (!fgets(buf, (int)len, f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	buf[strcspn(buf, "\n")] = 0;
	return 0;
}

static void path_parent(char *p)   /* strip last /component in place */
{
	char *s = strrchr(p, '/');
	if (s && s != p)
		*s = 0;
}

/*
 * Find the digitizer hidraw node. For each /sys/class/hidraw/hidrawN:
 *   hidrawN/device -> HID device dir (0003:05AC:8600.000X)
 *   parent dir     -> USB interface  (has bInterfaceNumber)
 *   grandparent    -> USB device     (has idVendor/idProduct/bConfigurationValue)
 * Match VID/PID 05ac:8600, config 2; prefer interface 2 over interface 6.
 * Returns 0 and fills dev_out ("/dev/hidrawN").
 */
static int find_digitizer(char *dev_out, size_t out_len, int verbose)
{
	DIR *d = opendir("/sys/class/hidraw");
	if (!d) {
		fprintf(stderr, "opendir /sys/class/hidraw: %s\n", strerror(errno));
		return -1;
	}
	char best[NAME_MAX + 8] = "", second[NAME_MAX + 8] = "";
	struct dirent *e;
	while ((e = readdir(d))) {
		if (strncmp(e->d_name, "hidraw", 6) != 0)
			continue;
		char link[PATH_MAX], hiddev[PATH_MAX];
		snprintf(link, sizeof link, "/sys/class/hidraw/%s/device", e->d_name);
		if (!realpath(link, hiddev))
			continue;

		char ifdir[PATH_MAX];
		snprintf(ifdir, sizeof ifdir, "%s", hiddev);
		path_parent(ifdir);                     /* USB interface dir */

		char val[64];
		if (read_sysfs(ifdir, "bInterfaceNumber", val, sizeof val) < 0)
			continue;                       /* not a USB-interface parent
							   (e.g. apple_ibridge virtual HID) */
		int ifnum = (int)strtol(val, NULL, 16);

		char usbdir[PATH_MAX];
		snprintf(usbdir, sizeof usbdir, "%s", ifdir);
		path_parent(usbdir);                    /* USB device dir */

		char vid[64], pid[64], cfg[64];
		if (read_sysfs(usbdir, "idVendor", vid, sizeof vid) < 0 ||
		    read_sysfs(usbdir, "idProduct", pid, sizeof pid) < 0 ||
		    read_sysfs(usbdir, "bConfigurationValue", cfg, sizeof cfg) < 0)
			continue;
		if (strcasecmp(vid, USB_VID) || strcasecmp(pid, USB_PID))
			continue;
		if (verbose)
			printf("  candidate %s: iBridge intf %d, config %s\n",
			       e->d_name, ifnum, cfg);
		if (strcmp(cfg, "2") != 0) {
			if (verbose)
				printf("    (skipped: device not in config 2 — "
				       "display stack not loaded?)\n");
			continue;
		}
		if (ifnum == DIGITIZER_IFACE)
			snprintf(best, sizeof best, "/dev/%s", e->d_name);
		else if (ifnum == FALLBACK_IFACE && !second[0])
			snprintf(second, sizeof second, "/dev/%s", e->d_name);
	}
	closedir(d);

	if (best[0]) {
		snprintf(dev_out, out_len, "%s", best);
		return 0;
	}
	if (second[0]) {
		fprintf(stderr,
			"note: no hidraw on interface %d; falling back to interface %d (%s)\n",
			DIGITIZER_IFACE, FALLBACK_IFACE, second);
		snprintf(dev_out, out_len, "%s", second);
		return 0;
	}
	fprintf(stderr,
		"no config-2 iBridge hidraw node found.\n"
		"  -> is the display stack loaded? check:\n"
		"     cat /sys/bus/usb/devices/1-3/bConfigurationValue   (expect 2)\n"
		"     ls -l /sys/bus/usb/devices/1-3:2.*/0003:*/hidraw/  (expect hidrawN)\n");
	return -1;
}

/* ------------------------------------------------------------------ */
/* uinput (ported from dfr-switch.c)                                   */
/* ------------------------------------------------------------------ */
static int g_ui = -1;

static int uinput_open(void)
{
	g_ui = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (g_ui < 0) {
		fprintf(stderr, "open /dev/uinput: %s (run as root?)\n",
			strerror(errno));
		return -1;
	}
	ioctl(g_ui, UI_SET_EVBIT, EV_KEY);
	/* register every keycode of every layout so SIGUSR1 swaps need no re-create */
	for (int l = 0; l < DFR_NLAYOUTS; l++)
		for (int k = 0; k < dfr_layouts[l].nkeys; k++)
			ioctl(g_ui, UI_SET_KEYBIT, dfr_layouts[l].keys[k].keycode);

	struct uinput_setup us;
	memset(&us, 0, sizeof us);
	us.id.bustype = BUS_USB;
	us.id.vendor  = 0x1209;
	us.id.product = 0x7401;
	strcpy(us.name, "T1 Touch Bar (dfrd)");
	if (ioctl(g_ui, UI_DEV_SETUP, &us) < 0 ||
	    ioctl(g_ui, UI_DEV_CREATE) < 0) {
		fprintf(stderr, "uinput create: %s\n", strerror(errno));
		close(g_ui);
		g_ui = -1;
		return -1;
	}
	usleep(100000);   /* let userspace (libinput) pick the device up */
	return 0;
}

static void emit(int type, int code, int value)
{
	struct input_event ev;
	memset(&ev, 0, sizeof ev);
	ev.type = (uint16_t)type;
	ev.code = (uint16_t)code;
	ev.value = value;
	if (write(g_ui, &ev, sizeof ev) < 0) { /* best-effort */ }
}

static void key_down(int k) { emit(EV_KEY, k, 1); emit(EV_SYN, SYN_REPORT, 0); }
static void key_up(int k)   { emit(EV_KEY, k, 0); emit(EV_SYN, SYN_REPORT, 0); }

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
	const char *dev_override = NULL;
	int layout_idx = 0, verbose = 0, dry_run = 0;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-l") && i + 1 < argc) {
			layout_idx = dfr_layout_index(argv[++i]);
			if (layout_idx < 0) {
				fprintf(stderr, "unknown layout '%s'\n", argv[i]);
				return 2;
			}
		} else if (!strcmp(argv[i], "-d") && i + 1 < argc) {
			dev_override = argv[++i];
		} else if (!strcmp(argv[i], "-v")) {
			verbose = 1;
		} else if (!strcmp(argv[i], "-n")) {
			dry_run = 1;   /* parse + print, no uinput injection */
		} else {
			fprintf(stderr,
				"usage: %s [-l fn|media] [-d /dev/hidrawN] [-v] [-n]\n"
				"  -v  verbose (dump raw reports + candidates)\n"
				"  -n  dry run: print taps, do not inject keys\n"
				"  SIGUSR1 cycles layout (send to dfr-render too!)\n",
				argv[0]);
			return 2;
		}
	}

	signal(SIGINT, on_stop);
	signal(SIGTERM, on_stop);
	signal(SIGUSR1, on_usr1);

	char dev[64];
	if (dev_override)
		snprintf(dev, sizeof dev, "%s", dev_override);
	else if (find_digitizer(dev, sizeof dev, verbose) < 0)
		return 1;

	int fd = open(dev, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s (run as root?)\n", dev, strerror(errno));
		return 1;
	}

	int rdesc_size = -1;
	ioctl(fd, HIDIOCGRDESCSIZE, &rdesc_size);
	char hidname[128] = "?";
	ioctl(fd, HIDIOCGRAWNAME(sizeof hidname), hidname);
	printf("digitizer: %s (\"%s\", rdesc %d bytes)\n", dev, hidname, rdesc_size);

	if (!dry_run && uinput_open() < 0)
		return 1;

	const struct dfr_layout *layout = &dfr_layouts[layout_idx];
	printf("layout '%s':", layout->name);
	for (int i = 0; i < layout->nkeys; i++)
		printf(" [%s]", layout->keys[i].label);
	printf("\nlistening%s — Ctrl-C to stop.\n", dry_run ? " (dry run)" : "");
	fflush(stdout);

	int down = 0, down_key = 0;
	int float_off = -1;        /* auto-detected: 0, or 1 if reports are ID-prefixed */
	int logged_size = 0;
	uint8_t buf[256];

	while (!g_stop) {
		if (g_cycle) {
			g_cycle = 0;
			if (down) {           /* don't leave a key stuck across swap */
				if (!dry_run) key_up(down_key);
				down = 0;
			}
			layout_idx = (layout_idx + 1) % DFR_NLAYOUTS;
			layout = &dfr_layouts[layout_idx];
			printf("switched to layout '%s'\n", layout->name);
			fflush(stdout);
		}

		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		int pr = poll(&pfd, 1, RELEASE_MS);
		if (pr < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "poll: %s\n", strerror(errno));
			break;
		}
		if (pr == 0) {                       /* silence => finger lifted */
			if (down) {
				if (!dry_run) key_up(down_key);
				if (verbose) { printf("UP\n"); fflush(stdout); }
				down = 0;
			}
			continue;
		}

		ssize_t n = read(fd, buf, sizeof buf);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "read %s: %s (device gone? config flipped?)\n",
				dev, strerror(errno));
			break;
		}
		if (!logged_size) {
			logged_size = 1;
			printf("first report: %zd bytes (expect ~52)\n", n);
			fflush(stdout);
		}
		if (verbose) {
			printf("rpt %2zd:", n);
			for (int i = 0; i < n && i < 16; i++)
				printf(" %02x", buf[i]);
			printf("\n");
		}
		if (n < 4)
			continue;

		/* locate the float32 X: offset 0 on the wire (proven); offset 1
		 * if the HID stack hands us a report-ID-prefixed buffer */
		float x = 0.0f;
		if (float_off < 0) {
			float a, b = -1.0f;
			memcpy(&a, buf, 4);
			if (n >= 5)
				memcpy(&b, buf + 1, 4);
			if (a >= 0.45f && a <= 1.05f)
				float_off = 0;
			else if (b >= 0.45f && b <= 1.05f)
				float_off = 1;
			else {
				if (verbose)
					printf("  (no plausible X at offset 0/1 — "
					       "release frame or wrong node?)\n");
				/* treat as release */
				if (down) {
					if (!dry_run) key_up(down_key);
					down = 0;
				}
				continue;
			}
			printf("float X found at report offset %d\n", float_off);
			fflush(stdout);
		}
		if (n < float_off + 4)
			continue;
		memcpy(&x, buf + float_off, 4);

		if (x < 0.45f || x > 1.05f) {        /* explicit release / junk */
			if (down) {
				if (!dry_run) key_up(down_key);
				down = 0;
			}
			continue;
		}

		float nx = (x - 0.5f) * 2.0f;
		if (nx < 0.0f) nx = 0.0f;
		if (nx > 0.9999f) nx = 0.9999f;
		int z = dfr_zone_from_nx(layout, nx);

		if (!down) {
			down = 1;
			down_key = layout->keys[z].keycode;
			if (!dry_run)
				key_down(down_key);
			printf("DOWN x=%.3f nx=%.3f -> zone %d [%s]%s\n",
			       (double)x, (double)nx, z, layout->keys[z].label,
			       dry_run ? " (dry)" : "");
			fflush(stdout);
		}
		/* while held we keep the original key even if the finger slides */
	}

	if (down && !dry_run)
		key_up(down_key);
	if (g_ui >= 0) {
		ioctl(g_ui, UI_DEV_DESTROY);
		close(g_ui);
	}
	close(fd);
	printf("clean exit.\n");
	return 0;
}
