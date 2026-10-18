/*
 * Pixel 1 (sailfish, MSM8996) shim. Screen: the bootloader's splash setup
 * (MDP VIG0 -> LM0 -> CTL0 -> INTF1/DSI0, command-mode panel), see console.c.
 * Progress also goes to the pstore console zone; after the PSCI reset read it
 * from Android: adb root; adb shell cat /sys/fs/pstore/console-ramoops-0
 */
#include <stdint.h>
#include "console.h"
#include "loader.h"
#include "pstore.h"

#define PSCI_SYSTEM_RESET	0x84000009UL
#define APSS_WDT		0x9830000UL	/* qcom,wdt@9830000, 32768 Hz ticks */
#define WDT_RST			0x04
#define WDT_EN			0x08
#define WDT_BARK		0x10
#define WDT_BITE		0x14

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

static void __attribute__((noreturn)) psci_reset(void)
{
	register uint64_t x0 __asm__("x0") = PSCI_SYSTEM_RESET;
	__asm__ volatile("dsb sy; smc #0" : "+r"(x0) : : "memory");
	for (;;)
		__asm__ volatile("wfe");
}

static void delay_ms(unsigned ms)
{
	uint64_t end = rsr("cntpct_el0") + rsr("cntfrq_el0") / 1000 * ms;
	while (rsr("cntpct_el0") < end)
		;
}

/*
 * The payload (tools/mkpayload.py) is appended to this Image at _end by
 * build-pixel.sh: LK decompresses the kernel to 0x80080000 and the room up to
 * the DTB at 0x82600000 is ~37 MB. (The ramdisk slot is no good: LK forces it
 * to 0x82800000, refused a 24 MiB one, and it runs into ramoops at 13 MiB.)
 */
#define TAGS_ADDR	0x82600000UL	/* where LK puts the DTB */

extern char _end[];

void shim_main(uint64_t dtb)
{
	/* The bootloader leaves the APSS watchdog running (bite 30 s after entry). */
	uint32_t wdt_en = rd32(APSS_WDT + WDT_EN), wdt_bite = rd32(APSS_WDT + WDT_BITE);
	wr32(APSS_WDT + WDT_EN, 0);
	__asm__ volatile("dsb sy" ::: "memory");

	plog_init();
	plog_str("PIXEL1 SHIM: loader\n");
	plog_hex("WDT_EN was", wdt_en);
	plog_hex("WDT_BITE", wdt_bite);
	plog_hex("DTB", dtb);
	plog_hex("PAYLOAD", (uintptr_t)_end);

	con_clear(0);
	con_str("SHIM", "pixel1 (sailfish) loader");
	con_skip();
	/*
	 * Re-arm the watchdog for the kernel (bite WDT_SECS after this): a hang
	 * then ends in a warm reset that keeps RAM, so the kernel's early log
	 * in the pstore zone (xnu sweet_early.c) can be read from Android.
	 * WDT_SECS=0 (iOS root boots, log in the UFS ring): leave it off.
	 */
#if WDT_SECS
	plog_hex("watchdog re-armed, secs", WDT_SECS);
	wr32(APSS_WDT + WDT_BARK, WDT_SECS * 32768);
	wr32(APSS_WDT + WDT_BITE, WDT_SECS * 32768);
	wr32(APSS_WDT + WDT_RST, 1);
	wr32(APSS_WDT + WDT_EN, 1);
#endif
	const char *why = loader_boot_rd((uintptr_t)_end, TAGS_ADDR);
	plog_str("LOADER: ");
	plog_str(why);
	plog_str("\n");
	con_str("LOADER", why);
	con_str("RESET", "in 15s");
	con_flush();
	delay_ms(15000);
	psci_reset();
}

void secondary_main(uint64_t idx)
{
	(void)idx;
}

void exc_handler(uint64_t slot, uint64_t *regs)
{
	(void)regs;
	plog_hex("EXC slot", slot);
	plog_hex("ESR", rsr("esr_el1"));
	plog_hex("FAR", rsr("far_el1"));
	plog_hex("ELR", rsr("elr_el1"));
	psci_reset();
}
