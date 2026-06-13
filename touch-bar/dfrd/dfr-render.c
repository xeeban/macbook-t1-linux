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
 *      panel's RGB888 internally) and draws the active layout
 *   5. stays alive holding the mode; SIGUSR1 cycles to the next layout
 *      (dfr-touchd cycles on SIGUSR1 too, so signal both to stay in sync);
 *      SIGRTMIN+i sets layout i deterministically (driven by dfr-fnd)
 *
 * STEP 2 (this revision):
 *   - text is rendered with FreeType from JetBrainsMono Nerd Font
 *     (anti-aliased, UTF-8 labels incl. plane-15 Nerd Font icon glyphs).
 *     Font resolved via fontconfig ("JetBrainsMono Nerd Font"), overridable
 *     with env DFR_FONT=/path/to/font.ttf. Missing glyphs draw as U+FFFD.
 *   - DFR_IND_* keys (see dfr-layout.h) get live labels: battery (sysfs),
 *     kbd backlight (sysfs), wifi (nmcli, throttled), bluetooth (rfkill
 *     sysfs). While such a layout is shown, values refresh every ~2.5 s
 *     and the bar redraws only when something changed. The 30 ms main
 *     loop never blocks on CLI tools harder than that interval (nmcli is
 *     additionally wrapped in `timeout 1`).
 *
 * Geometry (proven on hardware — see DFR-CUSTOM-RENDERING-FEASIBILITY.md):
 *   - framebuffer is mode.hdisplay (60, SHORT axis) wide by
 *     mode.vdisplay (2170, LONG axis) tall
 *   - fb row index == position along the physical bar; row 0 == physical
 *     LEFT edge; short-axis flip confirmed on hardware -> g_flip_short=1
 *
 * Build: make    Run (as root, modules loaded): ./dfr-render -l media
 * Offline check (no root, no DRM): ./dfr-render -l ctrl --preview /tmp/ctrl.ppm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/mman.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include <fontconfig/fontconfig.h>

#include "dfr-layout.h"

#define DRIVER_NAME "appletbdrm"

/* colors (XRGB8888) */
#define COL_BG  0xFF0A0A0Au
#define COL_BTN 0xFF2E2E2Eu
#define COL_TXT 0xFFE8E8E8u
#define COL_ESC 0xFF3A2E2Eu   /* slightly warm tint so ESC is findable by feel */

/* type sizes for the 60px bar (buttons are bar-12 = 48px tall) */
#define FONT_PX_MAX 30
#define FONT_PX_MIN 10

#define IND_REFRESH_MS 2500   /* live-indicator poll interval */

static volatile sig_atomic_t g_stop, g_cycle, g_setlayout = -1;
static void on_stop(int s)  { (void)s; g_stop = 1; }
static void on_usr1(int s)  { (void)s; g_cycle = 1; }
static void on_rt(int s)    { g_setlayout = s - SIGRTMIN; }  /* SIGRTMIN+i -> set layout i */

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

static inline uint32_t get_phys(int px, int py)
{
	if (px < 0 || px >= g_long || py < 0 || py >= g_short)
		return COL_BG;
	int fx = g_flip_short ? (g_short - 1 - py) : py;
	int fy = g_flip_long  ? (g_long  - 1 - px) : px;
	return g_fb[(size_t)fy * g_pitch32 + fx];
}

/* alpha-blend fg over whatever is in the fb (a = coverage 0..255) */
static inline void blend_phys(int px, int py, uint32_t fg, uint8_t a)
{
	if (!a)
		return;
	if (a == 255) {
		put_phys(px, py, fg);
		return;
	}
	uint32_t bg = get_phys(px, py);
	uint32_t r = (((fg >> 16) & 0xff) * a + ((bg >> 16) & 0xff) * (255 - a)) / 255;
	uint32_t g = (((fg >>  8) & 0xff) * a + ((bg >>  8) & 0xff) * (255 - a)) / 255;
	uint32_t b = (((fg      ) & 0xff) * a + ((bg      ) & 0xff) * (255 - a)) / 255;
	put_phys(px, py, 0xFF000000u | (r << 16) | (g << 8) | b);
}

static void fill_phys(int px, int py, int w, int h, uint32_t c)
{
	for (int y = py; y < py + h; y++)
		for (int x = px; x < px + w; x++)
			put_phys(x, y, c);
}

/* ------------------------------------------------------------------ */
/* FreeType text (JetBrainsMono Nerd Font, anti-aliased, UTF-8)         */
/* ------------------------------------------------------------------ */
static FT_Library g_ftlib;
static FT_Face g_face;
static int g_cur_px = -1;

/* resolve font path: env DFR_FONT > fontconfig match > known path */
static int font_path(char *out, size_t n)
{
	const char *env = getenv("DFR_FONT");
	if (env && *env) {
		snprintf(out, n, "%s", env);
		return 0;
	}
	int ok = -1;
	if (FcInit()) {
		FcPattern *pat = FcNameParse((const FcChar8 *)"JetBrainsMono Nerd Font");
		if (pat) {
			FcConfigSubstitute(NULL, pat, FcMatchPattern);
			FcDefaultSubstitute(pat);
			FcResult res;
			FcPattern *m = FcFontMatch(NULL, pat, &res);
			if (m) {
				FcChar8 *file = NULL;
				if (FcPatternGetString(m, FC_FILE, 0, &file) == FcResultMatch && file) {
					snprintf(out, n, "%s", (const char *)file);
					ok = 0;
				}
				FcPatternDestroy(m);
			}
			FcPatternDestroy(pat);
		}
	}
	if (ok != 0) {
		snprintf(out, n, "/usr/share/fonts/TTF/JetBrainsMonoNerdFont-Regular.ttf");
		ok = access(out, R_OK) == 0 ? 0 : -1;
	}
	return ok;
}

static int ft_init(void)
{
	char path[512];
	if (font_path(path, sizeof path) < 0) {
		fprintf(stderr, "no usable font found (set DFR_FONT=/path/to/NerdFont.ttf)\n");
		return -1;
	}
	if (FT_Init_FreeType(&g_ftlib) != 0) {
		fprintf(stderr, "FT_Init_FreeType failed\n");
		return -1;
	}
	if (FT_New_Face(g_ftlib, path, 0, &g_face) != 0) {
		fprintf(stderr, "FT_New_Face(%s) failed (set DFR_FONT to override)\n", path);
		return -1;
	}
	printf("font: %s (%s %s)\n", path,
	       g_face->family_name ? g_face->family_name : "?",
	       g_face->style_name ? g_face->style_name : "?");
	return 0;
}

static void ft_px(int px)
{
	if (px != g_cur_px) {
		FT_Set_Pixel_Sizes(g_face, 0, (FT_UInt)px);
		g_cur_px = px;
	}
}

/* decode next UTF-8 codepoint; advances *ps; 0 at end, U+FFFD on junk */
static uint32_t utf8_next(const char **ps)
{
	const unsigned char *s = (const unsigned char *)*ps;
	if (!s[0])
		return 0;
	uint32_t cp;
	int len;
	if (s[0] < 0x80)        { cp = s[0];         len = 1; }
	else if ((s[0] & 0xE0) == 0xC0) { cp = s[0] & 0x1F; len = 2; }
	else if ((s[0] & 0xF0) == 0xE0) { cp = s[0] & 0x0F; len = 3; }
	else if ((s[0] & 0xF8) == 0xF0) { cp = s[0] & 0x07; len = 4; }
	else { (*ps)++; return 0xFFFD; }
	for (int i = 1; i < len; i++) {
		if ((s[i] & 0xC0) != 0x80) { (*ps)++; return 0xFFFD; }
		cp = (cp << 6) | (s[i] & 0x3F);
	}
	*ps += len;
	return cp;
}

static FT_UInt glyph_for(uint32_t cp)
{
	FT_UInt gi = FT_Get_Char_Index(g_face, cp);
	if (!gi)
		gi = FT_Get_Char_Index(g_face, 0xFFFD);  /* present in this font */
	return gi;
}

/* advance-width of UTF-8 string at pixel size px */
static int text_width_ft(const char *s, int px)
{
	ft_px(px);
	int w = 0;
	uint32_t cp;
	while ((cp = utf8_next(&s)) != 0) {
		if (FT_Load_Glyph(g_face, glyph_for(cp), FT_LOAD_DEFAULT) != 0)
			continue;
		w += (int)(g_face->glyph->advance.x >> 6);
	}
	return w;
}

/* line-box (ascent+descent) and ascent at pixel size px */
static void text_vmetrics(int px, int *asc, int *box)
{
	ft_px(px);
	int a = (int)(g_face->size->metrics.ascender >> 6);
	int d = (int)(-(g_face->size->metrics.descender >> 6));
	*asc = a;
	*box = a + d;
}

/* draw UTF-8 string, pen starting at (px,py_baseline), physical coords */
static void draw_text_ft(int x, int baseline, const char *s, int px, uint32_t col)
{
	ft_px(px);
	uint32_t cp;
	while ((cp = utf8_next(&s)) != 0) {
		if (FT_Load_Glyph(g_face, glyph_for(cp), FT_LOAD_RENDER) != 0)
			continue;
		FT_GlyphSlot gs = g_face->glyph;
		FT_Bitmap *bm = &gs->bitmap;
		int gx = x + gs->bitmap_left;
		int gy = baseline - gs->bitmap_top;
		for (unsigned int r = 0; r < bm->rows; r++) {
			const unsigned char *row = bm->buffer + (size_t)r * (size_t)bm->pitch;
			for (unsigned int c = 0; c < bm->width; c++) {
				uint8_t a;
				if (bm->pixel_mode == FT_PIXEL_MODE_MONO)
					a = (row[c >> 3] & (0x80 >> (c & 7))) ? 255 : 0;
				else
					a = row[c];
				blend_phys(gx + (int)c, gy + (int)r, col, a);
			}
		}
		x += (int)(gs->advance.x >> 6);
	}
}

/* ------------------------------------------------------------------ */
/* Live indicators (battery / kbd backlight / wifi / bluetooth)         */
/* ------------------------------------------------------------------ */
#define BAT_DIR  "/sys/class/power_supply/BAT0"
#define KBD_DIR  "/sys/class/leds/spi::kbd_backlight"

static int read_file_str(const char *path, char *buf, size_t n)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;
	if (!fgets(buf, (int)n, f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	buf[strcspn(buf, "\n")] = 0;
	return 0;
}

static int read_file_int(const char *path, int *out)
{
	char b[64];
	if (read_file_str(path, b, sizeof b) < 0)
		return -1;
	*out = atoi(b);
	return 0;
}

/* first line of `cmd` (already sh syntax); -1 on failure/empty */
static int run_cmd_line(const char *cmd, char *buf, size_t n)
{
	FILE *p = popen(cmd, "r");
	if (!p)
		return -1;
	int ok = fgets(buf, (int)n, p) ? 0 : -1;
	pclose(p);
	if (ok == 0)
		buf[strcspn(buf, "\n")] = 0;
	return ok;
}

/* encode one codepoint to UTF-8; returns byte count, buf NUL-terminated */
static int utf8_enc(uint32_t cp, char *buf)
{
	if (cp < 0x80) { buf[0] = (char)cp; buf[1] = 0; return 1; }
	if (cp < 0x800) {
		buf[0] = (char)(0xC0 | (cp >> 6));
		buf[1] = (char)(0x80 | (cp & 0x3F));
		buf[2] = 0; return 2;
	}
	if (cp < 0x10000) {
		buf[0] = (char)(0xE0 | (cp >> 12));
		buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		buf[2] = (char)(0x80 | (cp & 0x3F));
		buf[3] = 0; return 3;
	}
	buf[0] = (char)(0xF0 | (cp >> 18));
	buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
	buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
	buf[3] = (char)(0x80 | (cp & 0x3F));
	buf[4] = 0; return 4;
}

/* one cached label per indicator type; empty string = use static fallback */
static char g_ind_label[5][64];
static uint32_t g_ind_color[5];   /* dynamic tint per indicator (0 = default) */

static void ind_compose_battery(char *out, size_t n)
{
	int cap;
	char status[32];
	if (read_file_int(BAT_DIR "/capacity", &cap) < 0 ||
	    read_file_str(BAT_DIR "/status", status, sizeof status) < 0) {
		out[0] = 0;
		return;
	}
	uint32_t icon;
	if (!strncmp(status, "Charging", 8)) {
		icon = 0xF0084;                       /* nf-md-battery_charging */
		g_ind_color[DFR_IND_BATTERY] = 0xFF40D0FFu;         /* cyan */
	} else {
		if (cap >= 95)
			icon = 0xF0079;               /* nf-md-battery (full) */
		else if (cap >= 10)
			icon = 0xF007A + (uint32_t)(cap / 10 - 1);  /* 10..90 */
		else
			icon = 0xF008E;               /* nf-md-battery_outline */
		g_ind_color[DFR_IND_BATTERY] =
			cap <= 15 ? 0xFFEF5350u :     /* red   */
			cap <= 40 ? 0xFFFFC107u :     /* amber */
				    0xFF66BB6Au;      /* green */
	}
	char g[8];
	utf8_enc(icon, g);
	snprintf(out, n, "%s %d%%", g, cap);
}

static void ind_compose_kbdlight(char *out, size_t n)
{
	int cur, max;
	if (read_file_int(KBD_DIR "/brightness", &cur) < 0 ||
	    read_file_int(KBD_DIR "/max_brightness", &max) < 0 || max <= 0) {
		out[0] = 0;
		return;
	}
	char g[8];
	utf8_enc(0xF030C, g);                         /* nf-md-keyboard */
	g_ind_color[DFR_IND_KBDLIGHT] = 0xFFFFC107u;  /* amber */
	snprintf(out, n, "%s %d%%", g, (cur * 100 + max / 2) / max);
}

static void ind_compose_wifi(char *out, size_t n)
{
	char line[128];
	if (run_cmd_line("timeout 1 nmcli -t -f WIFI radio 2>/dev/null",
			 line, sizeof line) < 0) {
		out[0] = 0;
		return;
	}
	uint32_t icon;
	if (strcmp(line, "enabled") != 0) {
		icon = 0xF05AA;                       /* nf-md-wifi_off */
		g_ind_color[DFR_IND_WIFI] = 0xFF707070u;            /* gray (off) */
	} else if (run_cmd_line("timeout 1 nmcli -t -f TYPE,STATE device status 2>/dev/null"
				" | grep -m1 '^wifi:connected'",
				line, sizeof line) == 0) {
		icon = 0xF05A9;                       /* nf-md-wifi (connected) */
		g_ind_color[DFR_IND_WIFI] = 0xFF66BB6Au;            /* green */
	} else {
		icon = 0xF092E;                       /* nf-md-wifi_strength_outline */
		g_ind_color[DFR_IND_WIFI] = 0xFFB0BEC5u;            /* dim (on, no link) */
	}
	char g[8];
	utf8_enc(icon, g);
	snprintf(out, n, "%s", g);
}

/* bluetooth via rfkill sysfs (no CLI needed, works unprivileged) */
static void ind_compose_bt(char *out, size_t n)
{
	uint32_t icon = 0;
	for (int i = 0; i < 16; i++) {
		char p[96], type[32];
		snprintf(p, sizeof p, "/sys/class/rfkill/rfkill%d/type", i);
		if (read_file_str(p, type, sizeof type) < 0)
			continue;
		if (strcmp(type, "bluetooth") != 0)
			continue;
		int soft = 0, hard = 0;
		snprintf(p, sizeof p, "/sys/class/rfkill/rfkill%d/soft", i);
		read_file_int(p, &soft);
		snprintf(p, sizeof p, "/sys/class/rfkill/rfkill%d/hard", i);
		read_file_int(p, &hard);
		if (soft || hard) {
			icon = 0xF00B2;               /* bluetooth_off */
			g_ind_color[DFR_IND_BT] = 0xFF707070u;      /* gray */
		} else {
			icon = 0xF00AF;               /* bluetooth */
			g_ind_color[DFR_IND_BT] = 0xFF5C9DFFu;      /* blue */
		}
		break;
	}
	if (!icon) {
		out[0] = 0;
		return;
	}
	char g[8];
	utf8_enc(icon, g);
	snprintf(out, n, "%s", g);
}

/* refresh all indicator labels; returns 1 if any changed */
static int ind_refresh(void)
{
	char next[5][64];
	memset(next, 0, sizeof next);
	ind_compose_battery (next[DFR_IND_BATTERY],  sizeof next[0]);
	ind_compose_wifi    (next[DFR_IND_WIFI],     sizeof next[0]);
	ind_compose_bt      (next[DFR_IND_BT],       sizeof next[0]);
	ind_compose_kbdlight(next[DFR_IND_KBDLIGHT], sizeof next[0]);
	int changed = memcmp(next, g_ind_label, sizeof next) != 0;
	memcpy(g_ind_label, next, sizeof next);
	return changed;
}

/* live label for a key (static label if no indicator / no live value) */
static const char *key_label(const struct dfr_key *k)
{
	if (k->indicator > DFR_IND_NONE && k->indicator < 5 &&
	    g_ind_label[k->indicator][0])
		return g_ind_label[k->indicator];
	return k->label;
}

/* ------------------------------------------------------------------ */
/* Layout drawing                                                       */
/* ------------------------------------------------------------------ */
static void draw_layout(const struct dfr_layout *l)
{
	fill_phys(0, 0, g_long, g_short, COL_BG);
	for (int i = 0; i < l->nkeys; i++) {
		int x0, x1;
		dfr_key_extent(l, i, g_long, &x0, &x1);
		int bw = (x1 - x0) - 6;                  /* 3px gap each side */
		uint32_t bc = (l->keys[i].action == DFR_ACT_KEY &&
			       l->keys[i].keycode == KEY_ESC) ? COL_ESC : COL_BTN;
		fill_phys(x0 + 3, 6, bw, g_short - 12, bc);

		const struct dfr_key *k = &l->keys[i];
		const char *label = key_label(k);
		uint32_t tcol = COL_TXT;
		if (k->indicator > DFR_IND_NONE && k->indicator < 5 && g_ind_color[k->indicator])
			tcol = g_ind_color[k->indicator];
		else if (k->color)
			tcol = k->color;

		/* biggest size that fits width and bar height */
		int px = FONT_PX_MAX, asc, box, tw;
		for (;;) {
			tw = text_width_ft(label, px);
			text_vmetrics(px, &asc, &box);
			if ((tw <= bw - 14 && box <= g_short - 14) || px <= FONT_PX_MIN)
				break;
			px -= 2;
		}
		int tx = x0 + 3 + (bw - tw) / 2;
		int baseline = (g_short - box) / 2 + asc;
		draw_text_ft(tx, baseline, label, px, tcol);
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
 * root — lets the layout/font/rotation be eyeballed before a hardware run.
 * Indicators are refreshed once (battery/kbd via /sys work unprivileged;
 * wifi/bt best-effort, falling back to the static labels). */
static int do_preview(const struct dfr_layout *l, const char *path)
{
	g_short = 60;
	g_long = 2170;
	g_pitch32 = g_short;
	g_fb = calloc((size_t)g_short * g_long, 4);
	if (!g_fb)
		return 1;
	if (dfr_layout_has_indicators(l))
		ind_refresh();
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

static int64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
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
				"usage: %s [-l fn|media|ctrl|alt|meta] [-c /dev/dri/cardN]\n"
				"          [--flip-long] [--flip-short] [--preview out.ppm]\n"
				"  --flip-short : if labels render vertically mirrored\n"
				"  --flip-long  : if the row is left/right reversed vs touch\n"
				"  --preview    : no DRM — write what the bar would show as a PPM\n"
				"  DFR_FONT     : env override for the Nerd Font path\n"
				"  SIGUSR1      : cycle to next layout (send to dfr-touchd too!)\n"
				"  SIGRTMIN+i   : set layout i (dfr-fnd drives this)\n",
				argv[0]);
			return 2;
		}
	}

	if (ft_init() < 0)
		return 1;

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
	if (dfr_layout_has_indicators(layout))
		ind_refresh();
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

	int64_t next_ind = 0;   /* refresh immediately on first indicator layout */
	while (!g_stop) {
		int redraw = 0;
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
				next_ind = 0;   /* fresh values for the new layout */
				redraw = 1;
				printf("set layout '%s'\n", layout->name);
				fflush(stdout);
			}
		}
		if (dfr_layout_has_indicators(layout) && now_ms() >= next_ind) {
			if (ind_refresh())
				redraw = 1;
			next_ind = now_ms() + IND_REFRESH_MS;
		}
		if (redraw) {
			draw_layout(layout);
			drmModeDirtyFB(fd, fb_id, NULL, 0);
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
