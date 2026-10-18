/*
 * One-bit 16K page probe for other Qualcomm phones (fastboot boot only).
 * Reads ID_AA64MMFR0_EL1.TGran16 [23:20] (0 = no 16K granule) and answers
 * through PSCI: supported -> SYSTEM_RESET (phone reboots right away),
 * unsupported -> SYSTEM_OFF (phone powers off). Hang = probe never ran.
 */
	.text
	.globl _head
_head:
	b	_start			/* code0 */
	.long	0			/* code1 */
	.quad	0x80000			/* text_offset */
	.quad	_end - _head		/* image_size */
	.quad	0xa			/* flags: LE, 4K pages, place anywhere */
	.quad	0, 0, 0			/* res2-res4 */
	.ascii	"ARM\x64"		/* magic */
	.long	0			/* res5 */

_start:
	msr	daifset, #0xf
	mrs	x1, id_aa64mmfr0_el1
	ubfx	x1, x1, #20, #4
	mov	w0, #0x0009		/* PSCI SYSTEM_RESET 0x84000009 */
	mov	w2, #0x0008		/* PSCI SYSTEM_OFF   0x84000008 */
	cmp	x1, #0
	csel	w0, w0, w2, ne
	movk	w0, #0x8400, lsl #16
	smc	#0
1:	wfe
	b	1b
	.balign	4096
_end:
