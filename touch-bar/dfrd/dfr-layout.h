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
 * `kill -USR1` both PIDs to swap layouts in lock-step.
 */
#ifndef DFR_LAYOUT_H
#define DFR_LAYOUT_H

#include <linux/input-event-codes.h>

struct dfr_key {
	const char *label;   /* uppercase A-Z 0-9 + - < > | and space only (5x7 font) */
	int keycode;         /* linux KEY_* injected via uinput */
	float weight;        /* relative width of the button */
};

struct dfr_layout {
	const char *name;
	const struct dfr_key *keys;
	int nkeys;
};

/* ------------------------------------------------------------------ */
/* Layouts (data-driven: edit here, both daemons pick it up on rebuild) */
/* ------------------------------------------------------------------ */

static const struct dfr_key dfr_keys_fn[] = {
	{ "ESC", KEY_ESC, 1.5f },
	{ "F1",  KEY_F1,  1.0f },
	{ "F2",  KEY_F2,  1.0f },
	{ "F3",  KEY_F3,  1.0f },
	{ "F4",  KEY_F4,  1.0f },
	{ "F5",  KEY_F5,  1.0f },
	{ "F6",  KEY_F6,  1.0f },
	{ "F7",  KEY_F7,  1.0f },
	{ "F8",  KEY_F8,  1.0f },
	{ "F9",  KEY_F9,  1.0f },
	{ "F10", KEY_F10, 1.0f },
	{ "F11", KEY_F11, 1.0f },
	{ "F12", KEY_F12, 1.0f },
};

static const struct dfr_key dfr_keys_media[] = {
	{ "ESC",  KEY_ESC,            1.4f },
	{ "BR-",  KEY_BRIGHTNESSDOWN, 1.0f },
	{ "BR+",  KEY_BRIGHTNESSUP,   1.0f },
	{ "<<",   KEY_PREVIOUSSONG,   1.0f },
	{ ">|",   KEY_PLAYPAUSE,      1.0f },
	{ ">>",   KEY_NEXTSONG,       1.0f },
	{ "MUTE", KEY_MUTE,           1.0f },
	{ "VOL-", KEY_VOLUMEDOWN,     1.0f },
	{ "VOL+", KEY_VOLUMEUP,       1.0f },
};

static const struct dfr_layout dfr_layouts[] = {
	{ "fn",    dfr_keys_fn,    (int)(sizeof(dfr_keys_fn)    / sizeof(dfr_keys_fn[0]))    },
	{ "media", dfr_keys_media, (int)(sizeof(dfr_keys_media) / sizeof(dfr_keys_media[0])) },
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

#endif /* DFR_LAYOUT_H */
