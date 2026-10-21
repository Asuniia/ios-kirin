#pragma once
#include <stdint.h>

#ifdef PIXEL1
#define FB_BASE		0xc0001000UL	/* cont_splash_mem = MDP VIG0 SRC0_ADDR */
#define FB_W		1080
#define FB_H		1920
#define FB_BPP		3		/* RGB888, YSTRIDE 0xca8 */
#elif defined(KIRIN)
#define FB_BASE		0x9d400000UL	/* splash_region = SRC0_ADDR of the pipe on LM0 */
#define FB_W		1080
#define FB_H		2520
#define FB_BPP		4		/* YSTRIDE 4320, bytes B, G, R, X */
#else
#define FB_BASE		0x9c000000UL	/* reserved-memory cont_splash_region */
#define FB_W		1080
#define FB_H		2400
#define FB_BPP		4		/* a8r8g8b8, matches mainline simplefb */
#endif
#define FB_STRIDE	(FB_W * FB_BPP)

#define FB_RGB(r, g, b)	(0xff000000U | (uint32_t)(r) << 16 | (uint32_t)(g) << 8 | (uint32_t)(b))

#define RED	FB_RGB(0xff, 0x00, 0x00)
#define BLUE	FB_RGB(0x00, 0x00, 0xa0)
#define WHITE	FB_RGB(0xff, 0xff, 0xff)
#define YELLOW	FB_RGB(0xff, 0xff, 0x00)
#define GREEN	FB_RGB(0x00, 0xff, 0x00)

extern volatile uint32_t *con_fb;

void con_clear(uint32_t bg);
void con_flush(void);	/* show what was drawn (command-mode panels) */
void con_skip(void);
int con_row(int r);	/* set the text row (r >= 0), returns the previous one */
void con_box(int x, int y, int size, uint32_t c);	/* filled square, top left at x, y */
void con_str(const char *label, const char *s);
void con_hex(const char *label, uint64_t v);
/* label, then "a=xxxxxxxx b=xxxxxxxx" style pairs of 32-bit values */
void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb);
