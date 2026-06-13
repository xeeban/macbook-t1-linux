/* SPDX-License-Identifier: GPL-2.0 */
/*
 * dfr-layout.h — SHARED button-layout definition for the T1 Touch Bar
 * userspace stack. Compiled into BOTH dfr-render (draws the zones) and
 * dfr-touchd (maps touch X to the zones). Keeping the data + zone math in
 * one header is what guarantees the drawn buttons and the touch targets
 * agree.
 *
 * Geometry recap (proven on hardware, see DFR-CUSTOM-RENDERING-FEASIBILITY.md):
 *  - physical panel: 2170 x 60 landscape; DRM mode is 60 x 2170 PORTRAIT
 *  - touch X arrives as float32 in [0.5, 1.0]; nx = (x - 0.5) * 2 in [0, 1)
 *  - nx = 0 is the physical LEFT edge (validated by the dfr-switch
 *    "buttons" demo: zone colors and taps lined up left-to-right)
 *
 * Layout switching: both daemons start with the same `-l <name>` and both
 * cycle to the next layout on SIGUSR1, so an external app-aware daemon can
 * `kill -USR1` both PIDs to swap layouts in lock-step. dfr-fnd sets layouts
 * DETERMINISTICALLY via SIGRTMIN+<index>.
 *
 * STEP-2 extensions (Nerd Font + actions + indicators):
 *  - labels are UTF-8 and may contain Nerd Font glyphs (JetBrainsMono Nerd
 *    Font, Material Design icons live in plane-15 PUA U+F0001..U+F1AF0).
 *    \U000F.... escapes below are encoded to UTF-8 by the compiler.
 *  - struct dfr_key grew `action` / `cmd` / `indicator` tail fields with
 *    zero defaults, so the original positional 3-field initializers
 *    ("fn"/"media") are source- and behavior-compatible.
 *  - DFR_ACT_CMD keys run `cmd` in the desktop user's session (dfr-touchd)
 *    instead of injecting a keycode.
 *  - DFR_IND_* keys get their label computed live by dfr-render (icon +
 *    value); the static `label` is the fallback when the value can't be
 *    read (and documents what the button is).
 */
#ifndef DFR_LAYOUT_H
#define DFR_LAYOUT_H

#include <linux/input-event-codes.h>

/* what a tap does */
enum dfr_action {
	DFR_ACT_KEY = 0,   /* inject `keycode` via uinput (default) */
	DFR_ACT_CMD = 1,   /* run `cmd` in the desktop user's session */
};

/* live label source (renderer recomputes icon+value at draw time) */
enum dfr_indicator {
	DFR_IND_NONE     = 0,
	DFR_IND_BATTERY  = 1,  /* /sys/class/power_supply/BAT0 capacity+status */
	DFR_IND_WIFI     = 2,  /* nmcli radio + device state */
	DFR_IND_BT       = 3,  /* /sys/class/rfkill (type bluetooth) */
	DFR_IND_KBDLIGHT = 4,  /* /sys/class/leds/spi::kbd_backlight */
};

struct dfr_key {
	const char *label;   /* UTF-8; ASCII and/or Nerd Font glyphs */
	int keycode;         /* linux KEY_* injected via uinput (DFR_ACT_KEY) */
	float weight;        /* relative width of the button */
	int action;          /* enum dfr_action (0 = DFR_ACT_KEY) */
	const char *cmd;     /* DFR_ACT_CMD: /bin/sh -c "<cmd>" as the user */
	int indicator;       /* enum dfr_indicator (0 = static label) */
};

struct dfr_layout {
	const char *name;
	const struct dfr_key *keys;
	int nkeys;
};

/* ------------------------------------------------------------------ */
/* Nerd Font glyphs used (JetBrainsMono Nerd Font v3, presence verified
 * with FT_Get_Char_Index on this machine's
 * /usr/share/fonts/TTF/JetBrainsMonoNerdFont-Regular.ttf):
 *
 *   U+F04AE nf-md-skip_previous     U+F04AD nf-md-skip_next
 *   U+F040E nf-md-play_pause        U+F04DB nf-md-stop
 *   U+F075F nf-md-volume_mute       U+F075E nf-md-volume_minus
 *   U+F075D nf-md-volume_plus
 *   U+F0084 nf-md-battery_charging  U+F0079 nf-md-battery
 *   U+F007A..U+F0082 nf-md-battery_10..90   U+F008E nf-md-battery_outline
 *   U+F05A9 nf-md-wifi              U+F05AA nf-md-wifi_off
 *   U+F092E nf-md-wifi_strength_outline (radio on, not connected)
 *   U+F00AF nf-md-bluetooth         U+F00B2 nf-md-bluetooth_off
 *   U+F030C nf-md-keyboard          (kbd backlight; % conveys the level —
 *                                    visually verified, unlike F08DC which
 *                                    renders as a file glyph in this font)
 *
 * Renderer fallback: any codepoint the loaded face lacks is drawn as
 * U+FFFD (present in this font), and indicator labels fall back to the
 * static ASCII `label` if the live value can't be read.
 */
#define DFR_GLYPH_PREV      "\U000F04AE"
#define DFR_GLYPH_PLAYPAUSE "\U000F040E"
#define DFR_GLYPH_NEXT      "\U000F04AD"
#define DFR_GLYPH_STOP      "\U000F04DB"
#define DFR_GLYPH_MUTE      "\U000F075F"
#define DFR_GLYPH_VOLDOWN   "\U000F075E"
#define DFR_GLYPH_VOLUP     "\U000F075D"

/* ------------------------------------------------------------------ */
/* Layouts (data-driven: edit here, both daemons pick it up on rebuild) */
/* ------------------------------------------------------------------ */

static const struct dfr_key dfr_keys_fn[] = {
	{ "ESC", KEY_ESC, 1.5f, 0, 0, 0 },
	{ "F1",  KEY_F1,  1.0f, 0, 0, 0 },
	{ "F2",  KEY_F2,  1.0f, 0, 0, 0 },
	{ "F3",  KEY_F3,  1.0f, 0, 0, 0 },
	{ "F4",  KEY_F4,  1.0f, 0, 0, 0 },
	{ "F5",  KEY_F5,  1.0f, 0, 0, 0 },
	{ "F6",  KEY_F6,  1.0f, 0, 0, 0 },
	{ "F7",  KEY_F7,  1.0f, 0, 0, 0 },
	{ "F8",  KEY_F8,  1.0f, 0, 0, 0 },
	{ "F9",  KEY_F9,  1.0f, 0, 0, 0 },
	{ "F10", KEY_F10, 1.0f, 0, 0, 0 },
	{ "F11", KEY_F11, 1.0f, 0, 0, 0 },
	{ "F12", KEY_F12, 1.0f, 0, 0, 0 },
};

static const struct dfr_key dfr_keys_media[] = {
	{ "ESC",  KEY_ESC,            1.4f, 0, 0, 0 },
	{ "BR-",  KEY_BRIGHTNESSDOWN, 1.0f, 0, 0, 0 },
	{ "BR+",  KEY_BRIGHTNESSUP,   1.0f, 0, 0, 0 },
	{ "<<",   KEY_PREVIOUSSONG,   1.0f, 0, 0, 0 },
	{ ">|",   KEY_PLAYPAUSE,      1.0f, 0, 0, 0 },
	{ ">>",   KEY_NEXTSONG,       1.0f, 0, 0, 0 },
	{ "MUTE", KEY_MUTE,           1.0f, 0, 0, 0 },
	{ "VOL-", KEY_VOLUMEDOWN,     1.0f, 0, 0, 0 },
	{ "VOL+", KEY_VOLUMEUP,       1.0f, 0, 0, 0 },
};

/* alt (Opt+Fn): GNOME-assignable extended function keys */
static const struct dfr_key dfr_keys_alt[] = {
	{ "F13", KEY_F13, 1.0f, 0, 0, 0 },
	{ "F14", KEY_F14, 1.0f, 0, 0, 0 },
	{ "F15", KEY_F15, 1.0f, 0, 0, 0 },
	{ "F16", KEY_F16, 1.0f, 0, 0, 0 },
	{ "F17", KEY_F17, 1.0f, 0, 0, 0 },
	{ "F18", KEY_F18, 1.0f, 0, 0, 0 },
	{ "F19", KEY_F19, 1.0f, 0, 0, 0 },
	{ "F20", KEY_F20, 1.0f, 0, 0, 0 },
	{ "F21", KEY_F21, 1.0f, 0, 0, 0 },
	{ "F22", KEY_F22, 1.0f, 0, 0, 0 },
	{ "F23", KEY_F23, 1.0f, 0, 0, 0 },
	{ "F24", KEY_F24, 1.0f, 0, 0, 0 },
};

/* meta (Cmd+Fn): media transport with Nerd Font glyphs — for when the
 * focused app overrides the default media strip */
static const struct dfr_key dfr_keys_meta[] = {
	{ DFR_GLYPH_PREV,      KEY_PREVIOUSSONG, 1.0f, 0, 0, 0 },
	{ DFR_GLYPH_PLAYPAUSE, KEY_PLAYPAUSE,    1.0f, 0, 0, 0 },
	{ DFR_GLYPH_NEXT,      KEY_NEXTSONG,     1.0f, 0, 0, 0 },
	{ DFR_GLYPH_STOP,      KEY_STOPCD,       1.0f, 0, 0, 0 },
	{ DFR_GLYPH_MUTE,      KEY_MUTE,         1.0f, 0, 0, 0 },
	{ DFR_GLYPH_VOLDOWN,   KEY_VOLUMEDOWN,   1.0f, 0, 0, 0 },
	{ DFR_GLYPH_VOLUP,     KEY_VOLUMEUP,     1.0f, 0, 0, 0 },
};

/* ctrl (Ctrl+Fn): SYSTEM row — live indicators; tap opens the matching
 * GNOME Settings panel in the user's session. Static labels are the
 * no-value fallbacks. */
static const struct dfr_key dfr_keys_ctrl[] = {
	{ "KBD",  0, 1.0f, DFR_ACT_CMD, "gnome-control-center keyboard",  DFR_IND_KBDLIGHT },
	{ "BAT",  0, 1.0f, DFR_ACT_CMD, "gnome-control-center power",     DFR_IND_BATTERY  },
	{ "WIFI", 0, 1.0f, DFR_ACT_CMD, "gnome-control-center wifi",      DFR_IND_WIFI     },
	{ "BT",   0, 1.0f, DFR_ACT_CMD, "gnome-control-center bluetooth", DFR_IND_BT       },
};

static const struct dfr_layout dfr_layouts[] = {
	{ "fn",    dfr_keys_fn,    (int)(sizeof(dfr_keys_fn)    / sizeof(dfr_keys_fn[0]))    },
	{ "media", dfr_keys_media, (int)(sizeof(dfr_keys_media) / sizeof(dfr_keys_media[0])) },
	{ "ctrl",  dfr_keys_ctrl,  (int)(sizeof(dfr_keys_ctrl)  / sizeof(dfr_keys_ctrl[0]))  },
	{ "alt",   dfr_keys_alt,   (int)(sizeof(dfr_keys_alt)   / sizeof(dfr_keys_alt[0]))   },
	{ "meta",  dfr_keys_meta,  (int)(sizeof(dfr_keys_meta)  / sizeof(dfr_keys_meta[0]))  },
};
#define DFR_NLAYOUTS ((int)(sizeof(dfr_layouts) / sizeof(dfr_layouts[0])))

/* ------------------------------------------------------------------ */
/* Shared zone math                                                    */
/* ------------------------------------------------------------------ */

static inline int dfr_layout_index(const char *name)
{
	for (int i = 0; i < DFR_NLAYOUTS; i++) {
		const char *a = dfr_layouts[i].name, *b = name;
		while (*a && *a == *b) { a++; b++; }
		if (*a == 0 && *b == 0)
			return i;
	}
	return -1;
}

static inline float dfr_total_weight(const struct dfr_layout *l)
{
	float t = 0.0f;
	for (int i = 0; i < l->nkeys; i++)
		t += l->keys[i].weight;
	return t;
}

/* nx in [0,1) (left -> right across the physical bar) -> key index */
static inline int dfr_zone_from_nx(const struct dfr_layout *l, float nx)
{
	if (nx < 0.0f) nx = 0.0f;
	if (nx > 0.99999f) nx = 0.99999f;
	float t = nx * dfr_total_weight(l), acc = 0.0f;
	for (int i = 0; i < l->nkeys; i++) {
		acc += l->keys[i].weight;
		if (t < acc)
			return i;
	}
	return l->nkeys - 1;
}

/* pixel extent [*x0, *x1) of key `idx` along the long axis of `width_px` px.
 * Must mirror dfr_zone_from_nx exactly (same cumulative-weight walk). */
static inline void dfr_key_extent(const struct dfr_layout *l, int idx,
				  int width_px, int *x0, int *x1)
{
	float total = dfr_total_weight(l), acc = 0.0f;
	*x0 = 0; *x1 = width_px;
	for (int i = 0; i <= idx && i < l->nkeys; i++) {
		float lo = acc / total;
		acc += l->keys[i].weight;
		float hi = acc / total;
		if (i == idx) {
			*x0 = (int)(lo * (float)width_px + 0.5f);
			*x1 = (int)(hi * (float)width_px + 0.5f);
		}
	}
	if (*x1 > width_px) *x1 = width_px;
	if (*x0 < 0) *x0 = 0;
}

/* does this layout need the live-indicator refresh path? */
static inline int dfr_layout_has_indicators(const struct dfr_layout *l)
{
	for (int i = 0; i < l->nkeys; i++)
		if (l->keys[i].indicator != DFR_IND_NONE)
			return 1;
	return 0;
}

#endif /* DFR_LAYOUT_H */
