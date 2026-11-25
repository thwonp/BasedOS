/*
 * BasedOS's boot screen: drawn straight to the framebuffer (/dev/fb0) while no frontend owns the
 * display, so the boot shows BasedOS instead of console text.
 *
 *   basedos-splash [-r 0|90|270] [status text...]
 *   basedos-splash [-r 0|90|270] --centre text...
 *   basedos-splash --console graphics|text
 *   basedos-splash --bmp FILE|-
 *
 * Every call redraws the whole screen (about 9 MB, a few ms). The layout is a grid on the screen
 * as held (landscape):
 *
 *   - the wordmark ("> BasedOS" and a block cursor) top left, a rule under it;
 *   - the info lines from /run/basedos/splash, left-aligned under it, one per line:
 *       !text        a heading (highlighted)
 *       !> text      a menu entry, selected
 *          text      a menu entry (three spaces first)
 *       key<TAB>val  a key and its value; the values of all of them line up
 *       @btn label   a label for a button beside the screen, at that button's height on the right
 *                    edge (start and select; another button's label is listed as a line)
 *       (empty)      a half-line gap
 *       text         plain text
 *   - the status text, if any, along the bottom (no rule above it: one would move as the
 *     status wraps to more lines).
 * Every line wraps at spaces to the room it has (beside a button label, up to it), so nothing is
 * cut off; the status block grows upwards for as many lines as it needs.
 *
 * Only the wordmark is green; text is neutral, headings and the selected entry amber.
 *
 * The panel's native mode is portrait; the 350H Ultra is held landscape, so on a portrait
 * framebuffer the drawing is turned the way BasedOS's SDL turns frames (-r 270, the default
 * there): landscape pixel (x, y) goes to native (y, H-1-x). -r 90 turns the other way, -r 0
 * not at all (a landscape framebuffer is never turned).
 *
 * --centre: nothing but the text, in the middle of a black screen, at the wordmark's size (one line;
 * it wraps at spaces if it doesn't fit). frontend-session leaves this on the framebuffer when it
 * hands the display to a frontend ("Intermission..."): the panel shows the framebuffer again
 * whenever the frontend lets go of the display, until the frontend draws over it or clears it.
 *
 * --console: the text console's mode, for BasedOS's screens on stock (usr/share/basedos/stock/
 * screen.sh), where a frontend gives the display up first. Its seat manager hands the console back
 * in text mode a moment after the frontend exits, and the text console clears the screen then;
 * "graphics" waits for that (up to 5 s), then puts the console in graphics mode, where the text
 * console draws nothing (as boot splash programs do). "text" puts it back.
 *
 * --bmp: an uncompressed 24-bit BMP of the framebuffer's own size and layout (not turned), from
 * FILE or stdin, and nothing else: the bootloader's logo shown again once the kernel has taken the
 * screen (plorpOS: frontend-session's boot_logo). Exits 1 having drawn nothing for any other image.
 *
 * Font: converted from Terminus Font (tools/boot-ui/font.h, OFL-Terminus.txt).
 */
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "font.h"

#ifndef INFO_FILE
#define INFO_FILE "/run/basedos/splash"
#endif
#define MAX_LINES 24
#define LINE_LEN 256

static struct fb_var_screeninfo vi;
static struct fb_fix_screeninfo fi;
static uint8_t *fb;      /* the visible page */
static int rot;          /* 0, 90 or 270 */
static int lw, lh;       /* logical (as held) size */

/* The grid, for the 1600x1440 panel as held (scaled for another size: see main). */
static int ts;           /* text scale: font pixels per glyph pixel */
static int cw, ch;       /* a text cell */
static int pitch;        /* line to line */
static int margin;       /* left and right of the text column */

/* Where the buttons beside the screen are, in thousandths of the screen's height from its top.
 * Measured from a straight-on photo of the 350H Ultra (the screen 142..500 px of a 1280x720
 * frame; the Start pill centred at 394 px, Select at 442), both just right of the screen. */
static const struct { const char *name; int permille; } buttons[] = {
	{ "start", 704 },
	{ "select", 839 },
};

/* A button label's box: lines that share its height wrap before it. */
#define MAX_RAIL 4
static struct { int x0, y0, y1; } rail[MAX_RAIL];
static int nrail;

static uint32_t pack(uint32_t rgb)
{
	uint32_t r = rgb >> 16 & 0xff, g = rgb >> 8 & 0xff, b = rgb & 0xff;

	return (r >> (8 - vi.red.length)) << vi.red.offset |
	       (g >> (8 - vi.green.length)) << vi.green.offset |
	       (b >> (8 - vi.blue.length)) << vi.blue.offset |
	       (vi.transp.length ? ((1u << vi.transp.length) - 1) << vi.transp.offset : 0);
}

static void put(int x, int y, uint32_t px)
{
	int nx, ny;
	uint8_t *p;

	if (x < 0 || y < 0 || x >= lw || y >= lh)
		return;
	switch (rot) {
	case 270: nx = y; ny = (int)vi.yres - 1 - x; break;
	case 90: nx = (int)vi.xres - 1 - y; ny = x; break;
	default: nx = x; ny = y; break;
	}
	p = fb + (size_t)ny * fi.line_length + (size_t)nx * (vi.bits_per_pixel / 8);
	switch (vi.bits_per_pixel) {
	case 32: *(uint32_t *)p = px; break;
	case 24: p[0] = px; p[1] = px >> 8; p[2] = px >> 16; break;
	case 16: *(uint16_t *)p = px; break;
	}
}

static void fill(int x0, int y0, int w, int h, uint32_t px)
{
	for (int y = y0; y < y0 + h; y++)
		for (int x = x0; x < x0 + w; x++)
			put(x, y, px);
}

static int glyph_index(unsigned char c)
{
	return (c >= 0x20 && c < 0x7f) ? c - 0x20 : '?' - 0x20;
}

/* n characters of s at (x, y), top left, each font pixel scale x scale. */
static void text_n(int x, int y, const char *s, int n, int scale, uint32_t px)
{
	for (int i = 0; i < n && s[i]; i++, x += FONT_W * scale) {
		const unsigned char *g = font_glyphs[glyph_index((unsigned char)s[i])];

		for (int r = 0; r < FONT_H; r++)
			for (int c = 0; c < FONT_W; c++)
				if (g[r * FONT_STRIDE + c / 8] & (0x80 >> (c % 8)))
					fill(x + c * scale, y + r * scale, scale, scale, px);
	}
}

static void text(int x, int y, const char *s, int scale, uint32_t px)
{
	text_n(x, y, s, (int)strlen(s), scale, px);
}

/* The right end of the room for a line between y0 and y1: the right margin, or before a button
 * label at that height. */
static int room_right(int y0, int y1)
{
	int r = lw - margin;

	for (int i = 0; i < nrail; i++)
		if (y0 < rail[i].y1 && y1 > rail[i].y0 && rail[i].x0 < r)
			r = rail[i].x0;
	return r;
}

/* s from (x, y), wrapped at spaces to the room each line has (a word longer than a line is
 * broken), or to right if it's above 0. Draws only if draw; returns the y under the last line. */
static int wrap_to(int x, int y, const char *s, uint32_t px, int draw, int right)
{
	int n = (int)strlen(s), i = 0;

	while (i < n) {
		int max = ((right > 0 ? right : room_right(y, y + ch)) - x) / cw, len = n - i;

		if (max < 1)
			max = 1;
		if (len > max) {
			int b = i + max;

			while (b > i && s[b] != ' ')
				b--;
			len = b > i ? b - i : max;
		}
		if (draw)
			text_n(x, y, s + i, len, ts, px);
		i += len;
		while (s[i] == ' ')
			i++;
		y += pitch;
	}
	return y;
}

static int wrap(int x, int y, const char *s, uint32_t px, int draw)
{
	return wrap_to(x, y, s, px, draw, 0);
}

static int button_permille(const char *name, int len)
{
	for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++)
		if ((int)strlen(buttons[i].name) == len && !strncmp(buttons[i].name, name, len))
			return buttons[i].permille;
	return -1;
}

static int console_mode(const char *mode)
{
	int fd = open("/dev/tty0", O_RDWR | O_NOCTTY), m = -1, r;

	if (fd < 0)
		return 1;
	if (!strcmp(mode, "graphics")) {
		for (int i = 0; i < 100; i++) {
			if (ioctl(fd, KDGETMODE, &m) == 0 && m == KD_TEXT)
				break;
			usleep(50000);
		}
		r = ioctl(fd, KDSETMODE, KD_GRAPHICS);
	} else if (!strcmp(mode, "text")) {
		r = ioctl(fd, KDSETMODE, KD_TEXT);
	} else {
		r = -1;
	}
	close(fd);
	return r != 0;
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* --bmp: the whole image is read and checked before a pixel is written. */
static int draw_bmp(const char *path)
{
	FILE *f = strcmp(path, "-") ? fopen(path, "rb") : stdin;
	uint8_t h[54];
	int bpp = (int)vi.bits_per_pixel / 8;

	if (!f || fread(h, 1, sizeof(h), f) != sizeof(h) || h[0] != 'B' || h[1] != 'M')
		return 1;
	int32_t w = (int32_t)le32(h + 18), ht = (int32_t)le32(h + 22);
	int down = ht < 0;
	uint32_t off = le32(h + 10);

	if (down)
		ht = -ht;
	if ((h[28] | h[29] << 8) != 24 || le32(h + 30) != 0 || w != (int)vi.xres || ht != (int)vi.yres || off < sizeof(h))
		return 1;
	for (uint32_t k = sizeof(h); k < off; k++)
		if (fgetc(f) == EOF)
			return 1;
	size_t stride = ((size_t)w * 3 + 3) & ~(size_t)3;
	uint8_t *img = malloc(stride * (size_t)ht), *row = malloc((size_t)w * bpp);

	if (!img || !row || fread(img, 1, stride * (size_t)ht, f) != stride * (size_t)ht)
		return 1;
	for (int y = 0; y < ht; y++) {
		const uint8_t *s = img + stride * (size_t)(down ? y : ht - 1 - y);

		for (int x = 0; x < w; x++, s += 3) {
			uint32_t px = pack((uint32_t)s[2] << 16 | s[1] << 8 | s[0]);
			uint8_t *d = row + (size_t)x * bpp;

			switch (bpp) {
			case 4: memcpy(d, &px, 4); break;
			case 3: d[0] = px; d[1] = px >> 8; d[2] = px >> 16; break;
			case 2: d[0] = px; d[1] = px >> 8; break;
			}
		}
		memcpy(fb + (size_t)y * fi.line_length, row, (size_t)w * bpp);
	}
	return 0;
}

int main(int argc, char **argv)
{
	int fd, i = 1, n = 0, req = -1, centre = 0;
	static char lines[MAX_LINES][LINE_LEN];
	char status[512] = "";
	FILE *f;
	size_t size;
	uint8_t *map;

	const char *bmp = NULL;

	if (argc == 3 && strcmp(argv[1], "--console") == 0)
		return console_mode(argv[2]);
	if (argc == 3 && strcmp(argv[1], "--bmp") == 0) {
		bmp = argv[2];
		i = argc;
	}
	if (argc > 2 && strcmp(argv[1], "-r") == 0) {
		req = atoi(argv[2]);
		i = 3;
	}
	if (i < argc && strcmp(argv[i], "--centre") == 0) {
		centre = 1;
		i++;
	}
	for (; i < argc; i++) {
		if (status[0])
			strncat(status, " ", sizeof(status) - strlen(status) - 1);
		strncat(status, argv[i], sizeof(status) - strlen(status) - 1);
	}
	if ((f = fopen(INFO_FILE, "r"))) {
		while (n < MAX_LINES && fgets(lines[n], LINE_LEN, f)) {
			lines[n][strcspn(lines[n], "\r\n")] = 0;
			n++;
		}
		fclose(f);
	}

	/* BASEDOS_SPLASH_PREVIEW=<file>: draw into a 1440x1600 XRGB8888 file shaped like the panel's
	 * framebuffer instead (for checking the drawing off the device). */
	const char *preview = getenv("BASEDOS_SPLASH_PREVIEW");

	if (preview) {
		vi.xres = vi.xres_virtual = 1440;
		vi.yres = vi.yres_virtual = 1600;
		vi.bits_per_pixel = 32;
		vi.red.offset = 16; vi.red.length = 8;
		vi.green.offset = 8; vi.green.length = 8;
		vi.blue.offset = 0; vi.blue.length = 8;
		fi.line_length = 1440 * 4;
		size = (size_t)fi.line_length * vi.yres_virtual;
		map = calloc(1, size);
		fd = -1;
	} else {
		fd = open("/dev/fb0", O_RDWR);
		if (fd < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &vi) || ioctl(fd, FBIOGET_FSCREENINFO, &fi))
			return 1;
		if (vi.bits_per_pixel != 32 && vi.bits_per_pixel != 24 && vi.bits_per_pixel != 16)
			return 1;
		size = (size_t)fi.line_length * vi.yres_virtual;
		map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (map == MAP_FAILED)
			return 1;
	}
	fb = map + (size_t)vi.yoffset * fi.line_length + (size_t)vi.xoffset * (vi.bits_per_pixel / 8);
	if (bmp) {
		if (draw_bmp(bmp))
			return 1;
		goto out;
	}

	/* A portrait framebuffer is shown turned (default 270, as BasedOS's SDL); landscape never is. */
	rot = vi.yres > vi.xres ? (req == 90 || req == 0 ? req : 270) : 0;
	lw = rot ? (int)vi.yres : (int)vi.xres;
	lh = rot ? (int)vi.xres : (int)vi.yres;

	/* The grid: made for 1600x1440 (text twice the font's size), halved on a smaller screen
	 * (HDMI at 800 wide and under). */
	ts = lw >= 1200 ? 2 : 1;
	cw = FONT_W * ts;
	ch = FONT_H * ts;
	pitch = ch + ch / 8;
	margin = lw * 7 / 100;
	int logo = ts + 1;                       /* wordmark scale */
	int logo_y = lh * 5 / 100;
	int rule_y = logo_y + FONT_H * logo + ch / 2;
	int top = rule_y + ch / 2;               /* first info line */
	int bottom = lh - lh * 5 / 100;          /* under the last status line */

	uint32_t green = pack(0x39ff7a), dim = pack(0x1f7f3a),      /* the wordmark only */
		 fg = pack(0xd6d6d6), muted = pack(0x8e8e8e), bright = pack(0xf2f2f2),
		 hi = pack(0xffc857), rule = pack(0x303030);

	/* The background is black, all-zero in every pixel format: one memset per line. */
	for (int y = 0; y < (int)vi.yres; y++)
		memset(fb + (size_t)y * fi.line_length, 0, (size_t)vi.xres * (vi.bits_per_pixel / 8));

	if (centre) {
		/* At the wordmark's scale, or the text's if that doesn't fit on one line. */
		int sc = (int)strlen(status) * FONT_W * logo <= lw - 2 * margin ? logo : ts;
		int w = (int)strlen(status) * FONT_W * sc;

		if (w <= lw - 2 * margin)
			text((lw - w) / 2, (lh - FONT_H * sc) / 2, status, sc, bright);
		else
			wrap(margin, (lh - ch) / 2, status, bright, 1);
		goto out;
	}

	/* The wordmark, top left, and a rule under it. */
	text(margin, logo_y, ">", logo, dim);
	text(margin + 2 * FONT_W * logo, logo_y, "BasedOS", logo, green);
	fill(margin + 9 * FONT_W * logo + logo, logo_y + 4 * logo, (FONT_W - 2) * logo, (FONT_H - 6) * logo, green);
	fill(margin, rule_y, lw - 2 * margin, ts, rule);

	/* Button labels first: right-justified at their buttons' heights, a pointer at the edge. */
	int keyw = 0;

	for (int l = 0; l < n; l++) {
		char *s = lines[l], *tab = strchr(s, '\t');

		if (tab && (int)(tab - s) > keyw)
			keyw = (int)(tab - s);
		if (s[0] != '@')
			continue;
		char *sp = strchr(s, ' ');
		int pm = sp ? button_permille(s + 1, (int)(sp - s - 1)) : -1;

		if (pm < 0 || nrail >= MAX_RAIL)
			continue;
		const char *label = sp + 1;
		int cy = lh * pm / 1000, tw = (int)strlen(label) * cw;
		int tri = ch * 3 / 4, tx = lw - margin / 3 - tri, x0 = tx - cw / 2 - tw;

		text(x0, cy - ch / 2, label, ts, bright);
		for (int r = 0; r < tri; r++) /* a right-pointing triangle, tri wide */
			fill(tx, cy - tri / 2 + r, (r < tri / 2 ? r : tri - 1 - r) + 1, 1, hi);
		rail[nrail].x0 = x0 - cw * 2;
		rail[nrail].y0 = cy - ch / 2 - pitch / 4;
		rail[nrail].y1 = cy + ch / 2 + pitch / 4;
		nrail++;
	}

	/* The info lines. */
	int y = top, keyx = margin + (keyw + 1) * cw;

	for (int l = 0; l < n; l++) {
		char *s = lines[l];

		if (s[0] == '@') {
			char *sp = strchr(s, ' ');

			if (sp && button_permille(s + 1, (int)(sp - s - 1)) >= 0)
				continue;
			/* A button with no known place: listed as a line. */
			char buf[LINE_LEN + 8];

			snprintf(buf, sizeof(buf), "%.*s: %s", sp ? (int)(sp - s - 1) : (int)strlen(s + 1), s + 1,
				 sp ? sp + 1 : "");
			y = wrap(margin, y, buf, fg, 1);
		} else if (!s[0]) {
			y += pitch / 2;
		} else if (!strncmp(s, "!> ", 3)) {
			text(margin, y, ">", ts, hi);
			y = wrap(margin + 2 * cw, y, s + 3, hi, 1);
		} else if (!strncmp(s, "   ", 3)) {
			y = wrap(margin + 2 * cw, y, s + 3, fg, 1);
		} else if (s[0] == '!') {
			y = wrap(margin, y, s + 1, hi, 1);
		} else if (strchr(s, '\t')) {
			char *tab = strchr(s, '\t');

			text_n(margin, y, s, (int)(tab - s), ts, muted);
			y = wrap(keyx, y, tab + 1, bright, 1);
		} else {
			y = wrap(margin, y, s, fg, 1);
		}
	}

	/* The status, bottom-anchored: as many lines as it takes, all of one width (up to a button
	 * label beside any of them). */
	if (status[0]) {
		int k, sy = bottom - pitch, right = lw - margin;

		for (k = 1; k < 12; k++) {
			sy = bottom - k * pitch;
			right = room_right(sy - pitch / 2, bottom);
			if (wrap_to(margin, sy, status, bright, 0, right) <= bottom)
				break;
		}
		wrap_to(margin, sy, status, bright, 1, right);
	}

out:
	if (preview) {
		FILE *o = fopen(preview, "wb");

		if (!o || fwrite(map, 1, size, o) != size)
			return 1;
		fclose(o);
		return 0;
	}
	munmap(map, size);
	close(fd);
	return 0;
}
