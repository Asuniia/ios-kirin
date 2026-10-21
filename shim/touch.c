/*
 * Touch probe for kirin: Novatek NT36672A (TDDI, powered with the panel) at
 * I2C 0x01 (firmware address; the DT's 0x62 is the hardware one) on BLSP1
 * QUP1 (0xc175000), polled. For TOUCH_SECS it draws a square where each
 * finger is and shows the first finger's coordinates.
 *   clocks   GCC BLSP1 AHB vote (0x152004 bit 17) + QUP1 I2C branch
 *            (0x119008), source XO 19.2 MHz (Linux gcc-sdm660.c)
 *   pins     TLMM south tile (0x3100000 + 0x1000 * n): 2/3 SDA/SCL
 *            function 3 (blsp_i2c1), 2 mA, no pull (DT i2c_1_active)
 *   I2C      QUP FIFO mode with v1 tags, 400 kHz, as bsd/dev/sweet_touch.c
 *   Novatek  page = event buffer (write FF 02 1C: 0x21C00, NT36672A), then
 *            register 0 holds 10 x 6 bytes: id << 3 | status (1 down,
 *            2 move), x[11:4], y[11:4], x[3:0] << 4 | y[3:0], width,
 *            pressure (nvt_nt36672a/nt36xxx.c nvt_ts_work_func)
 * The only write to the IC selects the page.
 */
#include <stdint.h>
#include "console.h"
#include "touch.h"

#define GCC_VOTE	0x00152004UL	/* APCS_CLOCK_BRANCH_ENA_VOTE */
#define GCC_QUP1_I2C	0x00119008UL	/* CBCR */
#define QUP		0x0c175000UL
#define TLMM_SOUTH	0x03100000UL
#define TOUCH_SDA	2u
#define TOUCH_SCL	3u
#define NVT_ADDR	0x01u
#define NVT_EVENT_BUF	0x21c00u
#define TOUCH_MAX	10u
#define TOUCH_SECS	30u
#define POINT_LEN	65u		/* bytes after the register byte */

#define QUP_CONFIG        0x000u
#define QUP_STATE         0x004u
#define QUP_IO_MODE       0x008u
#define QUP_SW_RESET      0x00cu
#define QUP_OPERATIONAL   0x018u
#define QUP_ERROR_FLAGS   0x01cu
#define QUP_HW_VERSION    0x030u
#define QUP_MX_OUTPUT_CNT 0x100u
#define QUP_OUT_FIFO      0x110u
#define QUP_MX_WRITE_CNT  0x150u
#define QUP_MX_INPUT_CNT  0x200u
#define QUP_MX_READ_CNT   0x208u
#define QUP_IN_FIFO       0x218u
#define QUP_I2C_CLK_CTL   0x400u
#define QUP_I2C_STATUS    0x404u
#define QUP_I2C_MASTER_GEN 0x408u
#define QUP_STATE_RESET   0u
#define QUP_STATE_RUN     1u
#define QUP_STATE_PAUSE   3u
#define QUP_STATE_VALID   (1u << 2)
#define QUP_STATE_MASTER  (1u << 4)
#define QUP_IN_NOT_EMPTY  (1u << 5)
#define QUP_OUT_SVC       (1u << 8)
#define QUP_IN_SVC        (1u << 9)
#define QUP_MX_OUTPUT_DONE (1u << 10)
#define QUP_NO_INPUT      (1u << 7)
#define QUP_MINI_CORE     (2u << 8)
#define QUP_N_VAL         15u
#define QUP_REPACK_EN     (3u << 14)
#define QUP_TAG_START     (1u << 8)
#define QUP_TAG_DATA      (2u << 8)
#define QUP_TAG_STOP      (3u << 8)
#define QUP_TAG_REC       (4u << 8)
#define QUP_BUS_ACTIVE    (1u << 8)
#define QUP_BUS_ERRORS    0x38000fcu
#define QUP_ERRORS        0x7cu
#define QUP_CLK_400K      ((14u << 16) | (3u << 8) | 28u)

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

static void delay_us(unsigned us)
{
	uint64_t end = rsr("cntpct_el0") + rsr("cntfrq_el0") / 1000000 * us;
	while (rsr("cntpct_el0") < end)
		;
}

static unsigned rx_max;	/* data bytes per read that fit the input FIFO */
static uint32_t last_bus, last_err;

static int qup_state(uint32_t state)
{
	for (unsigned i = 0; i < 100; i++) {
		uint32_t s = rd(QUP + QUP_STATE);
		if ((s & QUP_STATE_VALID) && (s & 3u) == state)
			return 1;
		if (s & QUP_STATE_VALID)
			wr(QUP + QUP_STATE, state);
		delay_us(1);
	}
	return 0;
}

/* One message to NVT_ADDR: write len bytes (STOP after the last) or read len bytes. 0 = ok. */
static int qup_msg(int read, uint8_t *buf, unsigned len)
{
	unsigned tx = read ? 2u : len + 1u;
	uint32_t a8 = (NVT_ADDR << 1) | (read ? 1u : 0u);
	if (!qup_state(QUP_STATE_RESET) || !(rd(QUP + QUP_STATE) & QUP_STATE_MASTER))
		return -1;
	wr(QUP + QUP_MX_OUTPUT_CNT, 0);
	wr(QUP + QUP_MX_WRITE_CNT, tx);
	if (read) {
		wr(QUP + QUP_MX_INPUT_CNT, 0);
		wr(QUP + QUP_MX_READ_CNT, len);
	}
	wr(QUP + QUP_CONFIG, QUP_MINI_CORE | QUP_N_VAL | (read ? 0u : QUP_NO_INPUT));
	wr(QUP + QUP_IO_MODE, QUP_REPACK_EN);
	if (!qup_state(QUP_STATE_RUN))
		return -2;
	wr(QUP + QUP_I2C_CLK_CTL, QUP_CLK_400K);
	if (!qup_state(QUP_STATE_PAUSE))
		return -3;
	if (read) {
		wr(QUP + QUP_OUT_FIFO, ((QUP_TAG_REC | len) << 16) | QUP_TAG_START | a8);
	} else {
		uint32_t w = QUP_TAG_START | a8;
		for (unsigned i = 0; i < len; i++) {
			uint32_t tag = ((i == len - 1) ? QUP_TAG_STOP : QUP_TAG_DATA) | buf[i];
			if ((i & 1u) == 0) {
				wr(QUP + QUP_OUT_FIFO, w | (tag << 16));
				w = 0;
			} else {
				w = tag;
			}
		}
		if (!(len & 1u))
			wr(QUP + QUP_OUT_FIFO, w);
	}
	if (!qup_state(QUP_STATE_RUN))
		return -4;
	unsigned pos = 0;
	int err = -5;
	for (unsigned t = 0; t < 20000; t++) {
		uint32_t bus = rd(QUP + QUP_I2C_STATUS) & QUP_BUS_ERRORS;
		uint32_t qerr = rd(QUP + QUP_ERROR_FLAGS) & QUP_ERRORS;
		uint32_t op = rd(QUP + QUP_OPERATIONAL);
		if (bus || qerr) {
			last_bus = bus;
			last_err = qerr;
			wr(QUP + QUP_ERROR_FLAGS, qerr);
			wr(QUP + QUP_I2C_STATUS, bus);
			err = (bus & (1u << 3)) ? -6 : -7;	/* -6: NACK */
			break;
		}
		if (op & (QUP_OUT_SVC | QUP_IN_SVC))
			wr(QUP + QUP_OPERATIONAL, op & (QUP_OUT_SVC | QUP_IN_SVC));
		if (read) {
			while (pos < len && (rd(QUP + QUP_OPERATIONAL) & QUP_IN_NOT_EMPTY)) {
				uint32_t v = rd(QUP + QUP_IN_FIFO);
				buf[pos++] = (uint8_t)v;
				if (pos < len)
					buf[pos++] = (uint8_t)(v >> 16);
			}
			if (pos == len) {
				err = 0;
				break;
			}
		} else if (op & QUP_MX_OUTPUT_DONE) {
			err = 0;
			break;
		}
		delay_us(1);
	}
	for (unsigned t = 0; t < 1000 && (rd(QUP + QUP_I2C_STATUS) & QUP_BUS_ACTIVE); t++)
		delay_us(1);
	wr(QUP + QUP_STATE, QUP_STATE_RESET);
	return err;
}

/* Register reg of the current page, n bytes, in FIFO-sized pieces. */
static int nvt_read(uint8_t reg, uint8_t *buf, unsigned n)
{
	for (unsigned done = 0; done < n;) {
		unsigned chunk = n - done < rx_max ? n - done : rx_max;
		uint8_t r = (uint8_t)(reg + done);
		int err = qup_msg(0, &r, 1);
		if (!err)
			err = qup_msg(1, buf + done, chunk);
		if (err)
			return err;
		done += chunk;
	}
	return 0;
}

static void pin_i2c(unsigned gpio)
{
	/* function 3 (blsp_i2c1), no pull, 2 mA: (2 / 2 - 1) << 6 = 0 */
	wr(TLMM_SOUTH + 0x1000u * gpio, 3u << 2);
}

void touch_probe(void)
{
	wr(GCC_VOTE, rd(GCC_VOTE) | (1u << 17));		/* BLSP1 AHB */
	wr(GCC_QUP1_I2C, rd(GCC_QUP1_I2C) | 1u);		/* QUP1 I2C */
	for (unsigned i = 0; i < 1000 && (rd(GCC_QUP1_I2C) & (1u << 31)); i++)
		delay_us(1);
	con_hex2("CLK", "vote=", rd(GCC_VOTE), "cbcr=", rd(GCC_QUP1_I2C));
	if (rd(GCC_QUP1_I2C) & (1u << 31)) {
		con_str("CLK", "QUP1 I2C clock stays off");
		return;
	}
	pin_i2c(TOUCH_SDA);
	pin_i2c(TOUCH_SCL);

	wr(QUP + QUP_SW_RESET, 1);
	delay_us(10);
	if (!qup_state(QUP_STATE_RESET)) {
		con_hex("QUP", rd(QUP + QUP_STATE));
		return;
	}
	wr(QUP + QUP_CONFIG, QUP_MINI_CORE | QUP_N_VAL);
	wr(QUP + QUP_I2C_MASTER_GEN, 0);			/* v1 tags */
	uint32_t io = rd(QUP + QUP_IO_MODE);
	static const unsigned blk[4] = {4, 16, 32, 0};
	rx_max = blk[(io >> 5) & 3u] * (2u << ((io >> 7) & 7u)) / 2u;
	con_hex2("QUP", "hw=", rd(QUP + QUP_HW_VERSION), "rx=", rx_max);
	if (rx_max < 8)
		return;

	uint8_t page[3] = {0xff, (NVT_EVENT_BUF >> 16) & 0xff, (NVT_EVENT_BUF >> 8) & 0xff};
	int err = qup_msg(0, page, 3);
	if (err) {
		con_hex2("NVT", "err=", (uint32_t)-err, "bus=", last_bus);
		return;
	}
	con_str("NVT", "page set, touch the screen");

	static const uint32_t colour[TOUCH_MAX] = {
		0xffff0000, 0xff00ff00, 0xff0080ff, 0xffffff00, 0xffff00ff,
		0xff00ffff, 0xffff8000, 0xff8000ff, 0xffffffff, 0xff808080,
	};
	int text_row = con_row(-1);
	uint8_t p[POINT_LEN];
	unsigned reads = 0, errors = 0;
	uint64_t end = rsr("cntpct_el0") + rsr("cntfrq_el0") * TOUCH_SECS;
	while (rsr("cntpct_el0") < end) {
		if (nvt_read(0, p, POINT_LEN)) {
			errors++;
		} else {
			reads++;
			int shown = 0;
			for (unsigned i = 0; i < TOUCH_MAX; i++) {
				const uint8_t *e = p + 6 * i;
				unsigned id = e[0] >> 3, status = e[0] & 7u;
				if (id == 0 || id > TOUCH_MAX || (status != 1 && status != 2))
					continue;
				unsigned x = (unsigned)e[1] << 4 | e[3] >> 4;
				unsigned y = (unsigned)e[2] << 4 | (e[3] & 0xfu);
				con_box((int)x - 12, (int)y - 12, 24, colour[id - 1]);
				if (!shown++) {
					con_row(text_row);
					con_hex2("FINGER", "x=", x, "y=", y);
				}
			}
		}
		con_row(text_row + 1);
		con_hex2("READS", "ok=", reads, "err=", errors);
		delay_us(10000);
	}
	con_row(text_row + 2);
}
