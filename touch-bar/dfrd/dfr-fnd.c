// SPDX-License-Identifier: GPL-2.0
/*
 * dfr-fnd — momentary Fn-layer switcher for the T1 Touch Bar.
 *
 * Watches the Apple SPI Keyboard and switches the rendered Touch Bar layout to
 * mimic (and extend) macOS: the `media` control strip by default, and a layer
 * chosen by which modifiers are held WHILE Fn is pressed:
 *
 *     Fn            -> "fn"      (F1-F12)
 *     Ctrl + Fn     -> "ctrl"
 *     Alt/Opt + Fn  -> "alt"
 *     Cmd/Meta + Fn -> "meta"
 *     (Fn released) -> "media"   (the default)
 *
 * Any target layer that doesn't exist in dfr-layout.h falls back to "fn".
 * It sets the layout DETERMINISTICALLY by sending SIGRTMIN+<layout index> to
 * dfr-render and dfr-touchd (a missed event self-heals on the next press).
 *
 * Run as root (reads /dev/input/event*).
 *   sudo ./dfr-fnd -v                          # TEST: print key events, send no signals
 *   sudo ./dfr-fnd <render_pid> <touchd_pid>   # drive those pids
 *   Build: gcc -O2 -Wall -o dfr-fnd dfr-fnd.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <signal.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include "dfr-layout.h"

static int find_keyboard(char *out, size_t outsz)
{
	DIR *d = opendir("/dev/input");
	if (!d) return -1;
	struct dirent *e;
	char path[300], name[256];
	int fd = -1;
	while ((e = readdir(d))) {
		if (strncmp(e->d_name, "event", 5)) continue;
		snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
		int f = open(path, O_RDONLY);
		if (f < 0) continue;
		name[0] = 0;
		if (ioctl(f, EVIOCGNAME(sizeof name), name) >= 0 &&
		    strstr(name, "Apple SPI Keyboard")) {
			snprintf(out, outsz, "%s (%s)", path, name);
			fd = f;
			break;
		}
		close(f);
	}
	closedir(d);
	return fd;
}

/* resolve a layout name to index, falling back to "fn", then to 0 */
static int layer_idx(const char *name)
{
	int i = dfr_layout_index(name);
	if (i < 0) i = dfr_layout_index("fn");
	return i < 0 ? 0 : i;
}

int main(int argc, char **argv)
{
	int verbose = 0;
	pid_t pids[8];
	int npids = 0;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-v")) verbose = 1;
		else {
			pid_t p = (pid_t)atoi(argv[i]);
			if (p > 0 && npids < 8) pids[npids++] = p;
		}
	}

	char desc[400];
	int fd = find_keyboard(desc, sizeof desc);
	if (fd < 0) {
		fprintf(stderr, "Apple SPI Keyboard not found (run as root?)\n");
		return 1;
	}
	int idx_media = dfr_layout_index("media");
	if (idx_media < 0) idx_media = 0;

	printf("keyboard: %s\n", desc);
	if (verbose)
		printf("TEST MODE: printing key events. Press Fn and modifier+Fn combos. Ctrl-C to stop.\n");
	else
		printf("driving %d pid(s): Fn->fn, Ctrl+Fn->ctrl, Alt+Fn->alt, Cmd+Fn->meta, release->media\n", npids);

	int ctrl = 0, alt = 0, meta = 0;
	struct input_event ev;
	while (read(fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
		if (ev.type != EV_KEY || ev.value == 2 /* autorepeat */) continue;

		/* track modifier state */
		switch (ev.code) {
		case KEY_LEFTCTRL:  case KEY_RIGHTCTRL: ctrl = ev.value; break;
		case KEY_LEFTALT:   case KEY_RIGHTALT:  alt  = ev.value; break;
		case KEY_LEFTMETA:  case KEY_RIGHTMETA: meta = ev.value; break;
		}

		if (verbose && ev.code == KEY_FN)
			printf("  KEY_FN %s   (ctrl=%d alt=%d meta=%d)\n",
			       ev.value ? "down" : "up", ctrl, alt, meta);
		else if (verbose)
			printf("  code=%u value=%d\n", ev.code, ev.value);

		if (ev.code == KEY_FN) {
			int idx;
			if (ev.value) {            /* Fn pressed: choose layer by modifier */
				const char *name = ctrl ? "ctrl" : alt ? "alt" : meta ? "meta" : "fn";
				idx = layer_idx(name);
			} else {                   /* Fn released: back to default */
				idx = idx_media;
			}
			for (int i = 0; i < npids; i++)
				kill(pids[i], SIGRTMIN + idx);
			if (verbose)
				printf("    -> layout '%s' (SIGRTMIN+%d)\n", dfr_layouts[idx].name, idx);
		}
	}
	return 0;
}
