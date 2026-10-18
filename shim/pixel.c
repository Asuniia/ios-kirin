/*
 * Pixel 1 (sailfish, MSM8996) shim. Screen: the bootloader's splash setup
 * (MDP VIG0 -> LM0 -> CTL0 -> INTF1/DSI0, command-mode panel), see console.c.
 * Progress also goes to the pstore console zone; after the PSCI reset read it
 * from Android: adb root; adb shell cat /sys/fs/pstore/console-ramoops-0
 */
#include <stdint.h>
#include "console.h"
#include "pstore.h"

#define PSCI_SYSTEM_RESET	0x84000009UL
#define APSS_WDT		0x9830000UL	/* qcom,wdt@9830000, 32768 Hz ticks */
#define WDT_EN			0x08
#define WDT_STS			0x0c
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

/* Full-screen colour named on screen: swapped R/B shows as the wrong one. */
static void colour_test(uint32_t c, const char *name)
{
	plog_str(name);
	con_clear(c);
	con_str("SHIM", "pixel1 colour test");
	con_str("SHOULD", name);
	con_flush();
	delay_ms(8000);
}

void shim_main(uint64_t dtb)
{
	plog_init();
	plog_str("PIXEL1 SHIM: console test\n");
	/* The bootloader leaves the APSS watchdog running (reset ~27 s after entry). */
	plog_hex("WDT_EN", rd32(APSS_WDT + WDT_EN));
	plog_hex("WDT_STS", rd32(APSS_WDT + WDT_STS));
	plog_hex("WDT_BARK", rd32(APSS_WDT + WDT_BARK));
	plog_hex("WDT_BITE", rd32(APSS_WDT + WDT_BITE));
	plog_hex("CNTPCT", rsr("cntpct_el0"));
	wr32(APSS_WDT + WDT_EN, 0);
	__asm__ volatile("dsb sy" ::: "memory");
	plog_hex("WDT_EN off", rd32(APSS_WDT + WDT_EN));
	colour_test(RED, "RED\n");
	colour_test(BLUE, "BLUE\n");

	con_clear(0);
	con_str("SHIM", "pixel1 (sailfish) console");
	con_skip();
	con_hex("EL", rsr("CurrentEL") >> 2);
	con_hex("MIDR", rsr("midr_el1"));
	con_hex("MMFR0", rsr("id_aa64mmfr0_el1"));
	con_hex("ISAR0", rsr("id_aa64isar0_el1"));
	con_hex("DTB", dtb);
	con_skip();
	con_str("RESET", "in 90s");
	con_flush();
	plog_str("text shown\n");
	/* heartbeat: the last value tells when an unexpected reset (watchdog) hit */
	for (int t = 1; t <= 90; t++) {
		delay_ms(1000);
		plog_hex("t", t);
	}
	plog_str("PIXEL1 SHIM: done, PSCI reset\n");
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
