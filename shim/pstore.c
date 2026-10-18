/*
 * Text log in the Android ramoops console zone (Pixel 1, live DT ramoops:
 * 1 MiB at 0x83500000 = 8 x 32K dump records, then the 512K console zone,
 * no ECC). After a warm reset Linux finds a valid persistent_ram_buffer
 * there and shows our text as /sys/fs/pstore/console-ramoops-0.
 * MMU off: plain device accesses straight to DRAM, byte stores only.
 */
#include "pstore.h"

#define PRZ_BASE	0x83540000UL
#define PRZ_DATA_MAX	(0x80000UL - 12)
#define PRZ_SIG		0x43474244U	/* "DBGC" */

#define PRZ_W(off)	(*(volatile uint32_t *)(PRZ_BASE + (off)))
#define PRZ_SIG_	PRZ_W(0)
#define PRZ_START	PRZ_W(4)	/* write position; == size until the ring wraps */
#define PRZ_SIZE	PRZ_W(8)

static uint32_t pos;

void plog_init(void)
{
	pos = 0;
	PRZ_START = 0;
	PRZ_SIZE = 0;
	PRZ_SIG_ = PRZ_SIG;
}

/* Keep what an earlier stage logged (valid record), else start fresh. */
void plog_append(void)
{
	if (PRZ_SIG_ == PRZ_SIG && PRZ_SIZE == PRZ_START && PRZ_SIZE <= PRZ_DATA_MAX)
		pos = PRZ_SIZE;
	else
		plog_init();
}

static void plog_putc(char c)
{
	if (pos >= PRZ_DATA_MAX)
		return;
	*(volatile uint8_t *)(PRZ_BASE + 12 + pos++) = c;
	PRZ_START = pos;
	PRZ_SIZE = pos;
}

void plog_str(const char *s)
{
	while (*s)
		plog_putc(*s++);
}

/* "label 0123456789abcdef\n"; values below 2^32 print as 8 digits */
void plog_hex(const char *label, uint64_t v)
{
	plog_str(label);
	plog_putc(' ');
	for (int sh = v >> 32 ? 60 : 28; sh >= 0; sh -= 4)
		plog_putc("0123456789abcdef"[(v >> sh) & 0xf]);
	plog_putc('\n');
}
