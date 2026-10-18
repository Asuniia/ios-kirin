/*
 * Loader test payload (tools/mkstub-payload.py): stands in for xnu at the
 * payload entry point, appends what it received to the pstore log and
 * resets. MMU off, x0 = boot_args (physical), like xnu's start.
 */
#include <stdint.h>
#include "pstore.h"

__asm__(".section .text.head, \"ax\"\n"
	".globl _head\n"
	"_head:\n"
	"	adr	x1, _stack_top\n"
	"	mov	sp, x1\n"
	"	b	stub_main\n");

struct boot_args {	/* loader.c, Revision/Version 2 */
	uint16_t Revision, Version;
	uint64_t virtBase, physBase, memSize, topOfKernelData;
	uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char CommandLine[608];
};

#define FILL_OFF	0x100000UL	/* filler segment: virtBase + this */

void __attribute__((noreturn)) stub_main(volatile struct boot_args *ba)
{
	plog_append();
	plog_str("STUB: reached, boot_args at\n");
	plog_hex("  ba", (uintptr_t)ba);
	plog_hex("  rev", ba->Revision << 16 | ba->Version);
	plog_hex("  physBase", ba->physBase);
	plog_hex("  memSize", ba->memSize);
	plog_hex("  top", ba->topOfKernelData);
	plog_hex("  adtVA", ba->deviceTreeP);
	plog_hex("  adtLen", ba->deviceTreeLength);
	plog_hex("  fb", ba->v_baseAddr);
	plog_hex("  depth", ba->v_depth);

	/* cmdline "stub sum=<hex> len=<hex>": the filler's expected checksum */
	char line[64];
	int i;
	for (i = 0; i < 63 && ba->CommandLine[i]; i++)
		line[i] = ba->CommandLine[i];
	line[i] = 0;
	plog_str("  cmdline ");
	plog_str(line);
	plog_str("\n");

	uint64_t len = 0;
	for (const char *p = line; *p; p++)
		if (p[0] == 'l' && p[1] == 'e' && p[2] == 'n' && p[3] == '=')
			for (p += 4; *p && *p != ' '; p++)
				len = len << 4 | (*p <= '9' ? *p - '0' : *p - 'a' + 10);
	uint64_t sum = 0;
	for (uint64_t off = 0; off < len; off += 8)
		sum = sum * 31 + *(volatile uint64_t *)(ba->physBase + FILL_OFF + off);
	plog_hex("  got sum", sum);

	plog_str("STUB: done, PSCI reset\n");
	register uint64_t x0 __asm__("x0") = 0x84000009UL;	/* SYSTEM_RESET */
	__asm__ volatile("dsb sy; smc #0" : "+r"(x0) : : "memory");
	for (;;)
		__asm__ volatile("wfe");
}
