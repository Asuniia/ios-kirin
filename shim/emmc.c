/*
 * Read-only eMMC probe for kirin (SDM630 SDHC1, qcom,sdhci-msm-v5).
 *
 * xboot loaded the boot image from this eMMC, so the host is clocked and the
 * card is selected (transfer state, 8-bit HS400). Like the UFS driver on the
 * Pixel, reuse that: show the host state, ask the card for its status
 * (CMD13), PIO-read the GPT header (CMD17, LBA 1) and look up "userdata".
 * Nothing is ever written to the card. 32-bit register accesses only.
 */
#include <stdint.h>
#include "console.h"
#include "emmc.h"

#define SDHC		0x0c0c4000UL	/* hc_mem: standard SDHCI registers */
#define CQHC		0x0c0c5000UL	/* cqhci_mem */

#define BLKSZ_CNT	0x04
#define ARG		0x08
#define XFER_CMD	0x0c
#define RESP0		0x10
#define DATA		0x20
#define PRESENT		0x24
#define HOST_CTL	0x28		/* host control 1, power, block gap, wakeup */
#define CLK_CTL		0x2c		/* clock, timeout, software reset */
#define INT_STAT	0x30
#define INT_EN		0x34
#define SIG_EN		0x38
#define HOST_CTL2	0x3c		/* auto cmd error, host control 2 */
#define CAPS		0x40
#define VERSION		0xfc
#define CQ_CFG		0x08		/* in cqhci_mem: bit 0 = CQE enabled */

#define INT_CMD_DONE	(1u << 0)
#define INT_XFER_DONE	(1u << 1)
#define INT_BUF_RD	(1u << 5)
#define INT_ERR		(1u << 15)

#define CMD_RESP48	0x02
#define CMD_CRC		0x08
#define CMD_INDEX	0x10
#define CMD_DATA	0x20
#define XFER_READ	(1u << 4)

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

static inline uint32_t rd(uintptr_t off) { return *(volatile uint32_t *)(SDHC + off); }
static inline void wr(uintptr_t off, uint32_t v) { *(volatile uint32_t *)(SDHC + off) = v; }

/* Wait for any of bits in INT_STAT, 100 ms cap; returns the status (0 = timeout). */
static uint32_t wait_int(uint32_t bits)
{
	uint64_t end = rsr("cntpct_el0") + rsr("cntfrq_el0") / 10;
	while (rsr("cntpct_el0") < end) {
		uint32_t s = rd(INT_STAT);
		if (s & (bits | INT_ERR))
			return s;
	}
	return 0;
}

static int wait_idle(uint32_t inhibit)
{
	uint64_t end = rsr("cntpct_el0") + rsr("cntfrq_el0") / 10;
	while (rd(PRESENT) & inhibit)
		if (rsr("cntpct_el0") >= end)
			return 0;
	return 1;
}

/* After an error the CMD (bit 1) / DAT (bit 2) circuits stay stuck until reset. */
static void reset_lines(uint32_t mask)
{
	wr(CLK_CTL, rd(CLK_CTL) | (mask << 24));
	uint64_t end = rsr("cntpct_el0") + rsr("cntfrq_el0") / 10;
	while ((rd(CLK_CTL) >> 24) & mask)
		if (rsr("cntpct_el0") >= end)
			break;
}

/* Returns 0 on success; *st gets the last INT_STAT for the screen. */
static int send_cmd(uint32_t idx, uint32_t arg, uint32_t flags, uint32_t xfer, uint32_t *st)
{
	if (!wait_idle(flags & CMD_DATA ? 3 : 1))
		return *st = rd(PRESENT), -1;
	wr(INT_STAT, 0xffffffff);
	wr(ARG, arg);
	wr(XFER_CMD, (idx << 24) | (flags << 16) | xfer);
	uint32_t s = wait_int(INT_CMD_DONE);
	*st = s;
	if (!(s & INT_CMD_DONE) || (s & INT_ERR)) {
		reset_lines(flags & CMD_DATA ? 6 : 2);
		return -2;
	}
	wr(INT_STAT, INT_CMD_DONE);
	return 0;
}

/* One 512-byte block by PIO into buf. */
static int read_block(uint32_t lba, uint32_t *buf, uint32_t *st)
{
	wr(BLKSZ_CNT, (1u << 16) | 512);
	int r = send_cmd(17, lba, CMD_DATA | CMD_INDEX | CMD_CRC | CMD_RESP48, XFER_READ, st);
	if (r)
		return r;
	uint32_t s = wait_int(INT_BUF_RD);
	*st = s;
	if (!(s & INT_BUF_RD) || (s & INT_ERR)) {
		reset_lines(6);
		return -3;
	}
	wr(INT_STAT, INT_BUF_RD);
	for (int i = 0; i < 128; i++) {
		buf[i] = rd(DATA);
		uint64_t t = rsr("cntpct_el0") + 20;	/* ~1 us between words */
		while (rsr("cntpct_el0") < t)
			;
	}
	s = wait_int(INT_XFER_DONE);
	*st = s;
	if (!(s & INT_XFER_DONE) || (s & INT_ERR)) {
		*st = s ? s : 0x80000000u | rd(PRESENT);	/* timeout: PRESENT with bit 31 set */
		return -4;
	}
	wr(INT_STAT, INT_XFER_DONE);
	return 0;
}

/*
 * count blocks by ADMA2 (64-bit descriptors, 12 bytes: attr, len, addr lo/hi)
 * into buf; CMD18 with auto CMD12 when count > 1. MMU off: buf's address is
 * physical and nothing is cached.
 */
#define ADMA_ADDR	0x58
#define ADMA_ADDR_HI	0x5c
#define INT_ADMA_ERR	(1u << 25)
#define XFER_DMA	(1u << 0)
#define XFER_BLKCNT	(1u << 1)
#define XFER_ACMD12	(1u << 2)
#define XFER_MULTI	(1u << 5)

static uint32_t adma_desc[3] __attribute__((aligned(8)));

static int read_adma(uint32_t lba, uint32_t count, void *buf, uint32_t *st)
{
	adma_desc[0] = (512u * count) << 16 | 0x23;	/* len, ACT=tran | END | VALID */
	adma_desc[1] = (uint32_t)(uintptr_t)buf;
	adma_desc[2] = (uint32_t)((uintptr_t)buf >> 32);
	__asm__ volatile("dsb sy" ::: "memory");
	wr(ADMA_ADDR, (uint32_t)(uintptr_t)adma_desc);
	wr(ADMA_ADDR_HI, 0);
	wr(BLKSZ_CNT, (count << 16) | 512);
	uint32_t xfer = XFER_DMA | XFER_READ;
	if (count > 1)
		xfer |= XFER_BLKCNT | XFER_MULTI | XFER_ACMD12;
	int r = send_cmd(count > 1 ? 18 : 17, lba, CMD_DATA | CMD_INDEX | CMD_CRC | CMD_RESP48, xfer, st);
	if (r)
		return r;
	uint32_t s = wait_int(INT_XFER_DONE);
	*st = s;
	if (!(s & INT_XFER_DONE) || (s & INT_ERR)) {
		reset_lines(6);
		return -5;
	}
	wr(INT_STAT, INT_XFER_DONE);
	return 0;
}

static void show_sig(const char *label, const uint32_t *b)
{
	char sig[9];
	for (int i = 0; i < 8; i++)
		sig[i] = ((const char *)b)[i];
	sig[8] = 0;
	con_str(label, sig);
}

static int name_is(const uint16_t *utf16, const char *want)
{
	for (int i = 0; i < 36; i++) {
		if (utf16[i] != (uint8_t)want[i])
			return 0;
		if (!want[i])
			return 1;
	}
	return 0;
}

void emmc_probe(void)
{
	static uint32_t blk[128];
	uint32_t st;

	con_hex2("SDHC", "ver=", rd(VERSION) >> 16, "pres=", rd(PRESENT));
	con_hex2("SDHC", "hctl=", rd(HOST_CTL), "clk=", rd(CLK_CTL));
	con_hex2("SDHC", "ctl2=", rd(HOST_CTL2), "caps=", rd(CAPS));
	con_hex2("SDHC", "int=", rd(INT_STAT), "inten=", rd(INT_EN));
	con_hex2("CQHC", "cfg=", *(volatile uint32_t *)(CQHC + CQ_CFG), "sig=", rd(SIG_EN));

	/* Latch every status bit, raise no interrupt line. */
	wr(SIG_EN, 0);
	wr(INT_EN, 0xffff00ff);

	/* CMD13 SEND_STATUS: the RCA is whatever xboot assigned, try the usual ones. */
	uint32_t rca = 0;
	for (uint32_t r = 1; r <= 3 && !rca; r++) {
		if (send_cmd(13, r << 16, CMD_INDEX | CMD_CRC | CMD_RESP48, 0, &st) == 0)
			rca = r;
	}
	if (!rca) {
		con_hex2("CMD13", "fail st=", st, "pres=", rd(PRESENT));
		return;
	}
	/* R1 bits 12:9 = state, 4 = tran */
	con_hex2("CMD13", "rca=", rca, "r1=", rd(RESP0));

	int r = read_block(1, blk, &st);
	show_sig("PIO", blk);
	if (r) {
		con_hex2("PIO", "err=", (uint32_t)-r, "st=", st);
		reset_lines(6);
	}

	static uint32_t dma[32 * 128] __attribute__((aligned(64)));	/* 32 blocks */
	r = read_adma(1, 1, dma, &st);
	show_sig("ADMA", dma);
	if (r) {
		con_hex2("ADMA", "err=", (uint32_t)-r, "st=", st);
		con_hex2("ADMA", "pres=", rd(PRESENT), "aerr=", rd(0x54));
		return;
	}
	if (dma[0] != 0x20494645 || dma[1] != 0x54524150)	/* "EFI PART" */
		return;

	uint32_t ent_lba = dma[18], nent = dma[20], entsz = dma[21];	/* header +72, +80, +84 */
	con_hex2("GPT", "ents@", ent_lba, "n=", nent);
	if (entsz != 128 || nent > 128)
		return;
	r = read_adma(ent_lba, (nent + 3) / 4, dma, &st);	/* CMD18 + auto CMD12 */
	if (r) {
		con_hex2("CMD18", "err=", (uint32_t)-r, "st=", st);
		return;
	}
	for (uint32_t i = 0; i < nent; i++) {
		const uint32_t *e = dma + i * 32;
		if (name_is((const uint16_t *)(e + 14), "userdata")) {	/* entry +56: name */
			con_hex2("USERDATA", "lba=", e[8], "end=", e[10]);	/* +32 first, +40 last */
			return;
		}
	}
	con_str("USERDATA", "not found");
}
