/*
 * Xperia 10 (kirin, SDM630) shim. Screen: the bootloader splash keeps being
 * scanned out after fastboot boot (pipe -> LM0 -> CTL0 -> INTF, video-mode
 * panel), so drawing into it shows up at once, see console.c.
 * No pstore here: the PSCI resets we get lose it, so everything goes to the
 * screen, and errors stay up for ERR_SECS before the phone powers off.
 */
#include <stdint.h>
#include "console.h"
#include "emmc.h"
#include "fdt.h"
#include "loader.h"
#include "touch.h"

#define PSCI_SYSTEM_OFF		0x84000008UL
#define APSS_WDT		0x17817000UL	/* qcom,wdt@17817000, 32768 Hz ticks */
#define WDT_EN			0x08
#define WDT_BITE		0x14
#define ERR_SECS		60

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

static void __attribute__((noreturn)) psci_off(void)
{
	register uint64_t x0 __asm__("x0") = PSCI_SYSTEM_OFF;
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

static void __attribute__((noreturn)) fail(const char *label, const char *why)
{
	con_str(label, why);
	con_str("OFF", "in 60s");
	delay_ms(ERR_SECS * 1000);
	psci_off();
}

/*
 * The payload (tools/mkpayload.py) is appended to this Image at _end by
 * build-kirin.sh. Where the bootloader puts the DTB and the ramdisk is
 * shown on screen: the payload must end below both.
 */
extern char _head[], _end[];

/* Bytes used by the payload at p (layout in tools/mkpayload.py), 0 if none. */
static uint64_t payload_size(uintptr_t p)
{
	static const char magic[] = "SWEETXNU";
	for (int i = 0; i < 8; i++)
		if (((volatile const char *)p)[i] != magic[i])
			return 0;
	uint32_t nsegs = *(volatile const uint32_t *)(p + 0x0c);
	uint64_t end = *(volatile const uint64_t *)(p + 0x20) +	/* adt_off */
		       *(volatile const uint64_t *)(p + 0x28);		/* adt_size */
	for (uint32_t i = 0; i < nsegs; i++) {
		volatile const uint64_t *s = (volatile const uint64_t *)(p + 0x2a8 + 32 * i);
		if (s[2] + s[3] > end)		/* file_off + file_size */
			end = s[2] + s[3];
	}
	return end;
}

void shim_main(uint64_t dtb)
{
	/* Stop the APSS watchdog in case the bootloader left it running. */
	uint32_t wdt_en = rd32(APSS_WDT + WDT_EN), wdt_bite = rd32(APSS_WDT + WDT_BITE);
	wr32(APSS_WDT + WDT_EN, 0);
	__asm__ volatile("dsb sy" ::: "memory");

	con_clear(0);
	con_str("SHIM", "kirin (xperia 10) loader");
	con_skip();
	con_hex2("WDT", "en=", wdt_en, "bite=", wdt_bite);
	con_hex("MPIDR", rsr("mpidr_el1") & 0xffffff);	/* mkadt.py --boot-mpidr */
	con_hex("HEAD", (uintptr_t)_head);
	con_hex("PAYLOAD", (uintptr_t)_end);
	con_hex("DTB", dtb);

	uint64_t rd_start = 0, rd_end = 0;
	if (fdt_chosen_u64(dtb, "linux,initrd-start", &rd_start) &&
	    fdt_chosen_u64(dtb, "linux,initrd-end", &rd_end))
		con_hex2("RAMDISK", "lo=", rd_start, "hi=", rd_end);
	else
		con_str("RAMDISK", "none in /chosen");

	/* The payload runs from _end up to the first thing the bootloader placed above it. */
	uint64_t limit = UINT64_MAX;
	if (dtb > (uintptr_t)_end)
		limit = dtb;
	if (rd_start > (uintptr_t)_end && rd_start < limit)
		limit = rd_start;
	con_hex("LIMIT", limit);
#ifdef KIRIN_EMMC_PROBE
	con_skip();
	emmc_probe();
	fail("EMMC", "probe done, nothing written");
#endif
#ifdef KIRIN_TOUCH_PROBE
	con_skip();
	touch_probe();
	fail("TOUCH", "probe done");
#endif

	uint64_t size = payload_size((uintptr_t)_end);
	if (!size)
		fail("PAYLOAD", "none (no SWEETXNU magic)");
	con_hex("PLD END", (uintptr_t)_end + size);
	if ((uintptr_t)_end + size > limit)
		fail("PAYLOAD", "runs into the DTB or ramdisk");
	con_skip();

	const char *why = loader_boot_rd((uintptr_t)_end, (uintptr_t)_end + size);
	fail("LOADER", why);
}

void secondary_main(uint64_t idx)
{
	(void)idx;
}

void exc_handler(uint64_t slot, uint64_t *regs)
{
	(void)regs;
	con_hex("EXC", slot);
	con_hex("ESR", rsr("esr_el1"));
	con_hex("FAR", rsr("far_el1"));
	con_hex("ELR", rsr("elr_el1"));
	fail("EXC", "shim exception");
}
