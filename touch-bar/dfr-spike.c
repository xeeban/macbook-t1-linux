// SPDX-License-Identifier: GPL-2.0
/*
 * dfr-spike — Phase-2 userspace spike: drive the T1 (MacBookPro13,2) Touch Bar
 * in "display mode" by replaying the upstream `appletbdrm` (T2) wire protocol
 * against the T1 iBridge (05ac:8600).
 *
 * This is a REVERSIBLE experiment. It assumes the device has ALREADY been
 * switched to USB configuration 2 (the wrapper `dfr-spike.sh` does that via
 * sysfs, which lets the kernel cleanly tear down the config-1 drivers). It then
 * claims the config-2 Audio/Video interface (class 0x10, interface 3), whose
 * bulk endpoints are OUT 0x02 / IN 0x85, and speaks the protocol.
 *
 * Protocol, structs, magic values and the GINF->REDY->CLRD/frame sequence are
 * ported 1:1 from drivers/gpu/drm/tiny/appletbdrm.c (GPL-2.0, Kerem Karabay).
 * The ONLY hypothesised T1 delta is the USB product id (8600 vs 8302); the RE
 * origin (DFRDisplayKm) was itself a T1 driver, so the frame format should match.
 *
 * x86_64 host (little-endian) assumed: the kernel uses cpu_to_le32(), which is a
 * no-op on LE, so the literal magic values below are byte-identical on the wire.
 *
 * Build:  gcc -O2 -Wall -o dfr-spike dfr-spike.c -lusb-1.0
 * Use:    via dfr-spike.sh (handles the config switch + restore). Needs root.
 *
 * Stages (argv[1]):
 *   probe        open device, print active config + the interface 3 endpoints. No protocol I/O.
 *   info         send GINF, print returned width/height/bpp/pixel_format. PROVES the protocol. No pixels.
 *   clear        info, then signal REDY, then CLRD (blank the panel).
 *   frame [RRGGBB] info, REDY, then paint the whole bar one solid colour (default FF00FF magenta).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include <libusb-1.0/libusb.h>

#define VID        0x05ac
#define PID        0x8600
#define IFACE      3
#define EP_OUT     0x02
#define EP_IN      0x85
#define TIMEOUT_MS 2000
#define BIG_TIMEOUT_MS 6000

/* FourCC message codes (host-order uint32 == on-wire bytes on LE) */
#define MSG_CLRD 0x434c5244u /* CLRD clear display      */
#define MSG_GINF 0x47494e46u /* GINF get information    */
#define MSG_UDCL 0x5544434cu /* UDCL update complete    */
#define MSG_REDY 0x52454459u /* REDY signal readiness   */
#define PIXFMT   0x52474241u /* "RGBA" tag, actually BGR888 */
#define BPP_EXPECTED 24

#pragma pack(push, 1)
struct req_header {           /* 16 */
	uint16_t unk00;
	uint16_t unk02;
	uint32_t unk04;
	uint32_t unk08;
	uint32_t size;
};
struct simple_request {       /* 32 */
	struct req_header h;
	uint32_t msg;
	uint8_t  unk14[8];
	uint32_t size;
};
struct resp_header {          /* 20 */
	uint8_t  unk00[16];
	uint32_t msg;
};
struct info_resp {            /* 65 */
	struct resp_header h;
	uint8_t  unk14[12];
	uint32_t width;
	uint32_t height;
	uint8_t  bpp;
	uint32_t bytes_per_row;
	uint32_t orientation;
	uint32_t bitmap_info;
	uint32_t pixel_format;
	uint32_t width_inches;
	uint32_t height_inches;
};
struct frame_hdr {            /* 12 (then pixels) */
	uint16_t begin_x;
	uint16_t begin_y;
	uint16_t width;
	uint16_t height;
	uint32_t buf_size;
};
struct fb_footer {            /* 80 */
	uint8_t  unk00[12];
	uint32_t unk0c;
	uint8_t  unk10[12];
	uint32_t unk1c;
	uint64_t timestamp;
	uint8_t  unk28[12];
	uint32_t unk34;
	uint8_t  unk38[20];
	uint32_t unk4c;
};
struct fb_req_hdr {           /* 48 (then data[]) */
	struct req_header h;
	uint16_t unk10;
	uint8_t  msg_id;
	uint8_t  unk13[29];
};
struct fb_resp {              /* 40 */
	struct resp_header h;
	uint8_t  unk14[12];
	uint64_t timestamp;
};
#pragma pack(pop)

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static void fourcc(uint32_t v, char out[5])
{
	out[0] = v & 0xff; out[1] = (v >> 8) & 0xff;
	out[2] = (v >> 16) & 0xff; out[3] = (v >> 24) & 0xff; out[4] = 0;
}

static libusb_device_handle *dev;

static int bulk_out(const void *buf, int len)
{
	int transferred = 0;
	int to = (len > 4096) ? BIG_TIMEOUT_MS : TIMEOUT_MS;
	int r = libusb_bulk_transfer(dev, EP_OUT, (unsigned char *)buf, len,
				     &transferred, to);
	if (r) { fprintf(stderr, "  bulk OUT failed: %s\n", libusb_error_name(r)); return r; }
	if (transferred != len) { fprintf(stderr, "  bulk OUT short: %d/%d\n", transferred, len); return -1; }
	return 0;
}

/* send a 32-byte simple request carrying `msg` */
static int send_msg(uint32_t msg)
{
	struct simple_request rq;
	memset(&rq, 0, sizeof(rq));
	rq.h.unk00 = 2;
	rq.h.unk02 = 0x1512;
	rq.h.size  = sizeof(rq) - sizeof(rq.h); /* 16 */
	rq.msg     = msg;
	rq.size    = rq.h.size;
	return bulk_out(&rq, sizeof(rq));
}

/* read `len` bytes into buf; transparently swallow ONE leading REDY signal */
static int read_resp(void *buf, int len, uint32_t expected)
{
	int transferred = 0, tries = 0;
	char a[5], b[5];
retry:
	transferred = 0;
	int r = libusb_bulk_transfer(dev, EP_IN, (unsigned char *)buf, len,
				     &transferred, TIMEOUT_MS);
	if (r) { fprintf(stderr, "  bulk IN failed: %s\n", libusb_error_name(r)); return r; }
	uint32_t msg;
	memcpy(&msg, (uint8_t *)buf + offsetof(struct resp_header, msg), 4);
	if (msg == MSG_REDY) {
		if (tries++ == 0) { fprintf(stderr, "  (got REDY readiness signal, re-reading)\n"); goto retry; }
		fprintf(stderr, "  unexpected second REDY\n"); return -1;
	}
	if (msg != expected) {
		fourcc(expected, a); fourcc(msg, b);
		fprintf(stderr, "  unexpected response: expected %s got %s (size %d)\n", a, b, transferred);
		return -1;
	}
	if (transferred != len) { fprintf(stderr, "  response short: %d/%d\n", transferred, len); return -1; }
	return 0;
}

static int do_info(struct info_resp *info)
{
	char fc[5];
	if (send_msg(MSG_GINF)) return -1;
	if (read_resp(info, sizeof(*info), MSG_GINF)) return -1;
	fourcc(info->pixel_format, fc);
	printf("GINF: width=%u height=%u bpp=%u bytes_per_row=%u orientation=%u pixel_format=%s(0x%08x)\n",
	       info->width, info->height, info->bpp, info->bytes_per_row,
	       info->orientation, fc, info->pixel_format);
	if (info->bpp != BPP_EXPECTED)
		printf("  WARNING: bpp %u != expected %u\n", info->bpp, BPP_EXPECTED);
	if (info->pixel_format != PIXFMT)
		printf("  WARNING: pixel_format 0x%08x != expected 0x%08x\n", info->pixel_format, PIXFMT);
	if (info->bpp == BPP_EXPECTED && info->pixel_format == PIXFMT)
		printf("  => T1 SPEAKS THE appletbdrm PROTOCOL. Custom rendering is reachable.\n");
	return 0;
}

static int do_frame(struct info_resp *info, uint8_t R, uint8_t G, uint8_t B)
{
	uint32_t dev_w = info->width, dev_h = info->height;
	uint32_t buf_size = dev_w * dev_h * 3;          /* BGR888 */
	size_t frames_size = sizeof(struct frame_hdr) + buf_size;
	size_t req_size = (sizeof(struct fb_req_hdr) + frames_size + sizeof(struct fb_footer) + 15) & ~((size_t)15);
	uint8_t *req = calloc(1, req_size);
	if (!req) { fprintf(stderr, "  OOM (%zu bytes)\n", req_size); return -1; }
	uint64_t ts = now_ns();

	struct fb_req_hdr *h = (struct fb_req_hdr *)req;
	h->h.unk00 = 2;
	h->h.unk02 = 0x12;
	h->h.unk04 = 9;
	h->h.size  = req_size - sizeof(struct req_header);
	h->unk10   = 1;
	h->msg_id  = (uint8_t)ts;

	struct frame_hdr *fr = (struct frame_hdr *)(req + sizeof(struct fb_req_hdr));
	fr->begin_x = 0;
	fr->begin_y = 0;
	fr->width   = dev_w;
	fr->height  = dev_h;
	fr->buf_size = buf_size;
	uint8_t *px = (uint8_t *)fr + sizeof(struct frame_hdr);
	for (uint32_t i = 0; i < dev_w * dev_h; i++) { px[3*i+0] = B; px[3*i+1] = G; px[3*i+2] = R; }

	struct fb_footer *ft = (struct fb_footer *)(req + sizeof(struct fb_req_hdr) + frames_size);
	ft->unk0c = 0xfffe;
	ft->unk1c = 0x80001;
	ft->unk34 = 0x80002;
	ft->unk4c = 0xffff;
	ft->timestamp = ts;

	printf("frame: %ux%u solid R=%u G=%u B=%u  (request %zu bytes)\n", dev_w, dev_h, R, G, B, req_size);
	if (bulk_out(req, req_size)) { free(req); return -1; }

	struct fb_resp resp;
	memset(&resp, 0, sizeof(resp));
	if (read_resp(&resp, sizeof(resp), MSG_UDCL)) { free(req); return -1; }
	if (resp.timestamp != ts)
		printf("  note: response ts %llu != request ts %llu\n",
		       (unsigned long long)resp.timestamp, (unsigned long long)ts);
	printf("  => UDCL received. A FRAME WAS ACCEPTED. Look at the Touch Bar.\n");
	free(req);
	return 0;
}

int main(int argc, char **argv)
{
	const char *stage = argc > 1 ? argv[1] : "probe";
	int rc = 1;

	if (libusb_init(NULL)) { fprintf(stderr, "libusb_init failed\n"); return 1; }
	dev = libusb_open_device_with_vid_pid(NULL, VID, PID);
	if (!dev) { fprintf(stderr, "device %04x:%04x not found\n", VID, PID); goto out_exit; }

	int cfg = -1;
	libusb_get_configuration(dev, &cfg);
	printf("active USB configuration: %d\n", cfg);
	if (cfg != 2) {
		fprintf(stderr, "ERROR: device is in config %d, not 2. Run via dfr-spike.sh "
				"which switches to config 2 first.\n", cfg);
		goto out_close;
	}

	libusb_set_auto_detach_kernel_driver(dev, 1);
	int r = libusb_claim_interface(dev, IFACE);
	if (r) { fprintf(stderr, "claim interface %d failed: %s\n", IFACE, libusb_error_name(r)); goto out_close; }
	printf("claimed interface %d (bulk OUT 0x%02x / IN 0x%02x)\n", IFACE, EP_OUT, EP_IN);

	if (!strcmp(stage, "probe")) {
		printf("probe OK: in config 2, interface %d claimable. No protocol I/O performed.\n", IFACE);
		rc = 0;
	} else {
		struct info_resp info;
		memset(&info, 0, sizeof(info));
		if (do_info(&info) == 0) {
			if (!strcmp(stage, "info")) {
				rc = 0;
			} else if (!strcmp(stage, "clear")) {
				if (send_msg(MSG_REDY) == 0 && send_msg(MSG_CLRD) == 0) {
					printf("CLRD sent (panel cleared).\n"); rc = 0;
				}
			} else if (!strcmp(stage, "frame")) {
				uint32_t rgb = 0xFF00FF;
				if (argc > 2) rgb = (uint32_t)strtoul(argv[2], NULL, 16);
				if (send_msg(MSG_REDY) == 0)
					rc = do_frame(&info, (rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
			} else {
				fprintf(stderr, "unknown stage '%s' (probe|info|clear|frame)\n", stage);
			}
		}
	}

	libusb_release_interface(dev, IFACE);
out_close:
	libusb_close(dev);
out_exit:
	libusb_exit(NULL);
	return rc;
}
