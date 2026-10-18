#pragma once
#include <stdint.h>

#ifdef PIXEL1
#define FB_BASE		0xc0001000UL	/* cont_splash_mem = MDP VIG0 SRC0_ADDR */
#define FB_W		1080
#define FB_H		1920
#define FB_BPP		3		/* RGB888, YSTRIDE 0xca8 */
#else
#define FB_BASE		0x9c000000UL	/* reserved-memory cont_splash_region */
#define FB_W		1080
#define FB_H		2400
#define FB_BPP		4		/* a8r8g8b8, matches mainline simplefb */
#endif
#define FB_STRIDE	(FB_W * FB_BPP)

#define RED	0xffff0000U
#define BLUE	0xff0000a0U
#define WHITE	0xffffffffU
#define YELLOW	0xffffff00U
#define GREEN	0xff00ff00U

extern volatile uint32_t *con_fb;

void con_clear(uint32_t bg);
void con_flush(void);	/* show what was drawn (command-mode panels) */
void con_skip(void);
void con_str(const char *label, const char *s);
void con_hex(const char *label, uint64_t v);
/* label, then "a=xxxxxxxx b=xxxxxxxx" style pairs of 32-bit values */
void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb);
