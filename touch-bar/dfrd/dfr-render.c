// SPDX-License-Identifier: GPL-2.0
/*
 * dfr-render — minimal libdrm/KMS renderer for the T1 Touch Bar
 * (appletbdrm DRM card, MacBookPro13,2 / iBridge 05ac:8600).
 *
 * What it does:
 *   1. finds the DRM card whose driver name is "appletbdrm"
 *   2. becomes DRM master (requires the card to be OFF seat0 — see
 *      99-touchbar-dfr.rules — or run from a bare VT)
 *   3. sets the native mode (60 x 2170 PORTRAIT; the panel is physically
 *      2170 x 60 landscape, rotated 90°)
 *   4. allocates a dumb buffer (XRGB8888 — appletbdrm converts to the
 *      panel's RGB888 internally) and draws a labeled function row
 *   5. stays alive holding the mode; SIGUSR1 cycles to the next layout
 *      (dfr-touchd cycles on SIGUSR1 too, so signal both to stay in sync)
 *
 * Geometry (proven on hardware — see DFR-CUSTOM-RENDERING-FEASIBILITY.md):
 *   - framebuffer is mode.hdisplay (60, SHORT axis) wide by
 *     mode.vdisplay (2170, LONG axis) tall
 *   - fb row index == position along the physical bar; row 0 == physical
 *     LEFT edge (validated by the dfr-switch "buttons" demo)
 *   - the SHORT-axis direction (which fb column is the physical TOP of the
 *     bar) is NOT yet hardware-validated: if labels render vertically
 *     mirrored, rerun with --flip-short. If the bar is left/right
 *     reversed, rerun with --flip-long (and tell dfr-touchd nothing — it
 *     maps touch, which is already proven left-to-right).
 *
 * Build: make    Run (as root, modules loaded): ./dfr-render -l fn
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/mman.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include "dfr-layout.h"

#define DRIVER_NAME "appletbdrm"

/* colors (XRGB8888) */
#define COL_BG  0xFF0A0A0Au
#define COL_BTN 0xFF2E2E2Eu
#define COL_TXT 0xFFE8E8E8u
#define COL_ESC 0xFF3A2E2Eu   /* slightly warm tint so ESC is findable by feel */

static volatile sig_atomic_t g_stop, g_cycle, g_setlayout = -1;
static void on_stop(int s)  { (void)s; g_stop = 1; }
static void on_usr1(int s)  { (void)s; g_cycle = 1; }
static void on_rt(int s)    { g_setlayout = s - SIGRTMIN; }  /* SIGRTMIN+i -> set layout i */

/* ------------------------------------------------------------------ */
/* 5x7 bitmap font: rows top->bottom, bit 4 = leftmost column           */
/* Glyphs: 0-9 A-Z + - < > |  (space renders blank)                     */
/* ------------------------------------------------------------------ */
static const uint8_t font5x7[][7] = {
	/* 0 */ {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},
	/* 1 */ {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
	/* 2 */ {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},
	/* 3 */ {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},
	/* 4 */ {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},
	/* 5 */ {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
	/* 6 */ {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},
	/* 7 */ {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
	/* 8 */ {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
	/* 9 */ {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
	/* A */ {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},
	/* B */ {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
	/* C */ {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},
	/* D */ {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C},
	/* E */ {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},
	/* F */ {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
	/* G */ {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F},
	/* H */ {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
	/* I */ {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},
	/* J */ {0x07,0x02,0x02,0x02,0x02,0x12,0x0C},
	/* K */ {0x11,0x12,0x14,0x18,0x14,0x12,0x11},
	/* L */ {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
	/* M */ {0x11,0x1B,0x15,0x15,0x11,0x11,0x11},
	/* N */ {0x11,0x19,0x15,0x13,0x11,0x11,0x11},
	/* O */ {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
	/* P */ {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
	/* Q */ {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},
	/* R */ {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
	/* S */ {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},
	/* T */ {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
	/* U */ {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},
	/* V */ {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},
	/* W */ {0x11,0x11,0x11,0x15,0x15,0x15,0x0A},
	/* X */ {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
	/* Y */ {0x11,0x11,0x0A,0x04,0x04,0x04,0x04},
	/* Z */ {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},
	/* + */ {0x00,0x04,0x04,0x1F,0x04,0x04,0x00},
	/* - */ {0x00,0x00,0x00,0x1F,0x00,0x00,0x00},
	/* < */ {0x02,0x04,0x08,0x10,0x08,0x04,0x02},
	/* > */ {0x08,0x04,0x02,0x01,0x02,0x04,0x08},
	/* | */ {0x04,0x04,0x04,0x04,0x04,0x04,0x04},
};

static int glyph_index(char ch)
{
	if (ch >= '0' && ch <= '9') return ch - '0';
	if (ch >= 'A' && ch <= 'Z') return 10 + (ch - 'A');
	if (ch >= 'a' && ch <= 'z') return 10 + (ch - 'a');
	switch (ch) {
	case '+': return 36;
	case '-': return 37;
	case '<': return 38;
	case '>': return 39;
	case '|': return 40;
	default:  return -1;  /* space / unknown -> blank */
	}
}

/* ------------------------------------------------------------------ */
/* Framebuffer + physical-coordinate drawing                            */
/* ------------------------------------------------------------------ */
static uint32_t *g_fb;          /* mapped dumb buffer */
static int g_pitch32;           /* pitch in 32-bit words */
static int g_short, g_long;     /* 60, 2170 */
static int g_flip_long, g_flip_short = 1; /* T1 MacBookPro13,2: short-axis flip confirmed correct */

/* physical coords: px 0..g_long-1 left->right, py 0..g_short-1 top->bottom */
static inline void put_phys(int px, int py, uint32_t c)
{
	if (px < 0 || px >= g_long || py < 0 || py >= g_short)
		return;
	int fx = g_flip_short ? (g_short - 1 - py) : py;
	int fy = g_flip_long  ? (g_long  - 1 - px) : px;
	g_fb[(size_t)fy * g_pitch32 + fx] = c;
}

static void fill_phys(int px, int py, int w, int h, uint32_t c)
{
	for (int y = py; y < py + h; y++)
		for (int x = px; x < px + w; x++)
			put_phys(x, y, c);
}

static void draw_char(int px, int py, int scale, char ch, uint32_t c)
{
	int gi = glyph_index(ch);
	if (gi < 0)
		return;
	for (int r = 0; r < 7; r++) {
		uint8_t row = font5x7[gi][r];
		for (int col = 0; col < 5; col++) {
			if (row & (0x10 >> col))
				fill_phys(px + col * scale, py + r * scale,
					  scale, scale, c);
		}
	}
}

static int text_width(const char *s, int scale)
{
	int n = (int)strlen(s);
	return n ? (n * 6 - 1) * scale : 0;   /* 5 cols + 1 spacing, minus trailing */
}

static void draw_text(int px, int py, int scale, const char *s, uint32_t c)
{
	for (; *s; s++, px += 6 * scale)
		draw_char(px, py, scale, *s, c);
}

static void draw_layout(const struct dfr_layout *l)
{
	fill_phys(0, 0, g_long, g_short, COL_BG);
	for (int i = 0; i < l->nkeys; i++) {
		int x0, x1;
		dfr_key_extent(l, i, g_long, &x0, &x1);
		int bw = (x1 - x0) - 6;                  /* 3px gap each side */
		uint32_t bc = (l->keys[i].keycode == KEY_ESC) ? COL_ESC : COL_BTN;
		fill_phys(x0 + 3, 6, bw, g_short - 12, bc);

		/* biggest scale that fits, max 4 */
		int scale = 4;
		while (scale > 1 &&
		       (text_width(l->keys[i].label, scale) > bw - 14 ||
			7 * scale > g_short - 20))
			scale--;
		int tw = text_width(l->keys[i].label, scale);
		int tx = x0 + 3 + (bw - tw) / 2;
		int ty = (g_short - 7 * scale) / 2;
		draw_text(tx, ty, scale, l->keys[i].label, COL_TXT);
	}
}

/* ------------------------------------------------------------------ */
/* DRM plumbing                                                         */
/* ------------------------------------------------------------------ */
static int open_card(const char *override, char *path_out, size_t path_len)
{
	if (override) {
		int fd = open(override, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			fprintf(stderr, "open %s: %s\n", override, strerror(errno));
		else
			snprintf(path_out, path_len, "%s", override);
		return fd;
	}
	for (int i = 0; i < 16; i++) {
		char p[64];
		snprintf(p, sizeof p, "/dev/dri/card%d", i);
		int fd = open(p, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;
		drmVersionPtr v = drmGetVersion(fd);
		if (v && v->name && !strcmp(v->name, DRIVER_NAME)) {
			drmFreeVersion(v);
			snprintf(path_out, path_len, "%s", p);
			return fd;
		}
		if (v)
			drmFreeVersion(v);
		close(fd);
	}
	fprintf(stderr,
		"no DRM card with driver '%s' found.\n"
		"  -> are appletbdrm.ko + apple_dfr_cfgsel.ko loaded? (see DFRD-RUNBOOK.md)\n",
		DRIVER_NAME);
	return -1;
}

/* --preview out.ppm: draw the layout into an offline buffer and write a
 * LANDSCAPE (2170x60) PPM of what the physical bar will show. No DRM, no
 * root — lets the layout/font/rotation be eyeballed before a hardware run. */
static int do_preview(const struct dfr_layout *l, const char *path)
{
	g_short = 60;
	g_long = 2170;
	g_pitch32 = g_short;
	g_fb = calloc((size_t)g_short * g_long, 4);
	if (!g_fb)
		return 1;
	draw_layout(l);
	FILE *f = fopen(path, "w");
	if (!f) {
		fprintf(stderr, "fopen %s: %s\n", path, strerror(errno));
		return 1;
	}
	fprintf(f, "P6\n%d %d\n255\n", g_long, g_short);
	for (int py = 0; py < g_short; py++)
		for (int px = 0; px < g_long; px++) {
			/* read back through the same flip mapping put_phys used */
			int fx = g_flip_short ? (g_short - 1 - py) : py;
			int fy = g_flip_long  ? (g_long  - 1 - px) : px;
			uint32_t c = g_fb[(size_t)fy * g_pitch32 + fx];
			fputc((c >> 16) & 0xff, f);
			fputc((c >> 8) & 0xff, f);
			fputc(c & 0xff, f);
		}
	fclose(f);
	printf("preview (%s, layout '%s') -> %s (2170x60 PPM)\n",
	       (g_flip_long || g_flip_short) ? "flipped" : "default orientation",
	       l->name, path);
	free(g_fb);
	return 0;
}

int main(int argc, char **argv)
{
	const char *card_override = NULL, *preview_path = NULL;
	int layout_idx = 0;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--preview") && i + 1 < argc) {
			preview_path = argv[++i];
		} else if (!strcmp(argv[i], "-l") && i + 1 < argc) {
			layout_idx = dfr_layout_index(argv[++i]);
			if (layout_idx < 0) {
				fprintf(stderr, "unknown layout '%s' (have:", argv[i]);
				for (int j = 0; j < DFR_NLAYOUTS; j++)
					fprintf(stderr, " %s", dfr_layouts[j].name);
				fprintf(stderr, ")\n");
				return 2;
			}
		} else if (!strcmp(argv[i], "-c") && i + 1 < argc) {
			card_override = argv[++i];
		} else if (!strcmp(argv[i], "--flip-long")) {
			g_flip_long = 1;
		} else if (!strcmp(argv[i], "--flip-short")) {
			g_flip_short = 1;
		} else {
			fprintf(stderr,
				"usage: %s [-l fn|media] [-c /dev/dri/cardN] [--flip-long] [--flip-short]\n"
				"          [--preview out.ppm]\n"
				"  --flip-short : if labels render vertically mirrored\n"
				"  --flip-long  : if the row is left/right reversed vs touch\n"
				"  --preview    : no DRM — write what the bar would show as a PPM\n"
				"  SIGUSR1      : cycle to next layout (send to dfr-touchd too!)\n",
				argv[0]);
			return 2;
		}
	}

	if (preview_path)
		return do_preview(&dfr_layouts[layout_idx], preview_path);

	signal(SIGINT, on_stop);
	signal(SIGTERM, on_stop);
	signal(SIGUSR1, on_usr1);
	for (int i = 0; i < DFR_NLAYOUTS && SIGRTMIN + i <= SIGRTMAX; i++)
		signal(SIGRTMIN + i, on_rt);

	char card_path[64] = "";
	int fd = open_card(card_override, card_path, sizeof card_path);
	if (fd < 0)
		return 1;
	printf("card: %s (%s)\n", card_path, DRIVER_NAME);

	/* Become master. If mutter still owns the card this fails — that means
	 * the seat udev rule isn't in effect for this card yet. */
	if (drmSetMaster(fd) != 0)
		fprintf(stderr,
			"warning: drmSetMaster: %s\n"
			"  (ok if we are the only client — first opener is master;\n"
			"   if the modeset below fails with EACCES/EPERM, the desktop\n"
			"   compositor still owns the card: check 99-touchbar-dfr.rules\n"
			"   is installed and the module was (re)loaded AFTER it)\n",
			strerror(errno));

	drmModeRes *res = drmModeGetResources(fd);
	if (!res) {
		fprintf(stderr, "drmModeGetResources: %s\n", strerror(errno));
		return 1;
	}

	drmModeConnector *conn = NULL;
	for (int i = 0; i < res->count_connectors; i++) {
		drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
		if (!c)
			continue;
		if (c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
			conn = c;
			break;
		}
		drmModeFreeConnector(c);
	}
	if (!conn) {
		fprintf(stderr, "no connected connector with modes on %s\n", card_path);
		return 1;
	}

	/* prefer the DRM_MODE_TYPE_PREFERRED mode, else mode 0 */
	drmModeModeInfo *mode = &conn->modes[0];
	for (int i = 0; i < conn->count_modes; i++)
		if (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) {
			mode = &conn->modes[i];
			break;
		}
	g_short = mode->hdisplay;   /* expect 60   */
	g_long  = mode->vdisplay;   /* expect 2170 */
	printf("connector %u, mode %s (%dx%d) -> bar is %d px long, %d px tall\n",
	       conn->connector_id, mode->name, mode->hdisplay, mode->vdisplay,
	       g_long, g_short);
	if (g_short > g_long) {
		fprintf(stderr,
			"warning: mode is landscape (%dx%d)? expected portrait 60x2170 —\n"
			"  continuing, treating hdisplay as the short axis anyway\n",
			mode->hdisplay, mode->vdisplay);
	}

	/* find a CRTC for this connector */
	uint32_t crtc_id = 0;
	if (conn->encoder_id) {
		drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoder_id);
		if (enc) {
			crtc_id = enc->crtc_id;
			drmModeFreeEncoder(enc);
		}
	}
	if (!crtc_id) {
		for (int i = 0; i < conn->count_encoders && !crtc_id; i++) {
			drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoders[i]);
			if (!enc)
				continue;
			for (int j = 0; j < res->count_crtcs; j++) {
				if (enc->possible_crtcs & (1u << j)) {
					crtc_id = res->crtcs[j];
					break;
				}
			}
			drmModeFreeEncoder(enc);
		}
	}
	if (!crtc_id) {
		fprintf(stderr, "no usable CRTC found\n");
		return 1;
	}

	/* dumb buffer, 32bpp XRGB8888 (appletbdrm converts to RGB888) */
	struct drm_mode_create_dumb creq;
	memset(&creq, 0, sizeof creq);
	creq.width  = mode->hdisplay;
	creq.height = mode->vdisplay;
	creq.bpp    = 32;
	if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) {
		fprintf(stderr, "CREATE_DUMB: %s\n", strerror(errno));
		return 1;
	}
	g_pitch32 = (int)(creq.pitch / 4);

	uint32_t fb_id = 0;
	uint32_t handles[4] = { creq.handle }, pitches[4] = { creq.pitch },
		 offsets[4] = { 0 };
	if (drmModeAddFB2(fd, creq.width, creq.height, DRM_FORMAT_XRGB8888,
			  handles, pitches, offsets, &fb_id, 0) < 0) {
		fprintf(stderr, "AddFB2(XRGB8888): %s\n", strerror(errno));
		return 1;
	}

	struct drm_mode_map_dumb mreq;
	memset(&mreq, 0, sizeof mreq);
	mreq.handle = creq.handle;
	if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0) {
		fprintf(stderr, "MAP_DUMB: %s\n", strerror(errno));
		return 1;
	}
	g_fb = mmap(NULL, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED,
		    fd, (off_t)mreq.offset);
	if (g_fb == MAP_FAILED) {
		fprintf(stderr, "mmap dumb: %s\n", strerror(errno));
		return 1;
	}

	const struct dfr_layout *layout = &dfr_layouts[layout_idx];
	draw_layout(layout);

	uint32_t conn_id = conn->connector_id;
	if (drmModeSetCrtc(fd, crtc_id, fb_id, 0, 0, &conn_id, 1, mode) < 0) {
		fprintf(stderr,
			"drmModeSetCrtc: %s\n"
			"  EACCES/EPERM here == we are not DRM master: the desktop\n"
			"  compositor still holds the card. Install 99-touchbar-dfr.rules,\n"
			"  reload the modules (so the card is re-created with the rule\n"
			"  active), and retry. Quick alternative: run from a bare VT\n"
			"  (sudo chvt 3).\n",
			strerror(errno));
		return 1;
	}
	printf("mode set; layout '%s' (%d keys) on the bar.\n",
	       layout->name, layout->nkeys);
	printf("zone table (left->right):");
	for (int i = 0; i < layout->nkeys; i++)
		printf(" [%s]", layout->keys[i].label);
	printf("\nSIGUSR1 cycles layout; Ctrl-C / SIGTERM exits (bar goes dark).\n");

	while (!g_stop) {
		if (g_cycle) {
			g_cycle = 0;
			g_setlayout = (layout_idx + 1) % DFR_NLAYOUTS;
		}
		if (g_setlayout >= 0) {
			int idx = g_setlayout;
			g_setlayout = -1;
			if (idx < DFR_NLAYOUTS && idx != layout_idx) {
				layout_idx = idx;
				layout = &dfr_layouts[layout_idx];
				draw_layout(layout);
				drmModeDirtyFB(fd, fb_id, NULL, 0);
				printf("set layout '%s'\n", layout->name);
				fflush(stdout);
			}
		}
		usleep(30000);
	}

	/* teardown */
	munmap(g_fb, creq.size);
	drmModeRmFB(fd, fb_id);
	struct drm_mode_destroy_dumb dreq;
	memset(&dreq, 0, sizeof dreq);
	dreq.handle = creq.handle;
	drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
	drmModeFreeConnector(conn);
	drmModeFreeResources(res);
	drmDropMaster(fd);
	close(fd);
	printf("clean exit.\n");
	return 0;
}
