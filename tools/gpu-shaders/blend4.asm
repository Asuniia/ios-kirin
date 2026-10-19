; blend.asm on 4 pixels per invocation: 128-bit ldgb/stgb (u32.4) and every
; ALU op as (rpt3) over the 4 pixels ((r) steps a source register, plain
; sources are constants). Back-to-back rpt3 ops are 4 slots apart per
; component, enough for the 3-slot ALU delay. g[0] = src, g[1] = dst.
@localsize 64, 1, 1
@buf 1024
@buf 1024
@invocationid(r0.x)
@wgid(r48.x)
mov.u32u32 r2.y, 0
mov.u32u32 r5.x, 0x00ff00ff
mov.u32u32 r5.y, 0x00800080
mov.u32u32 r5.z, 255
shl.b r1.x, r48.x, 6
(rpt2)nop
add.u r1.x, r1.x, r0.x
(rpt2)nop
shl.b r2.x, r1.x, 4
shl.b r1.y, r1.x, 2
(rpt5)nop
ldgb.untyped.4d.u32.4 r3.x, g[0], r2.x, r1.y
ldgb.untyped.4d.u32.4 r7.x, g[1], r2.x, r1.y
(sy)(rpt3)shr.b r8.x, (r)r3.x, 24
(rpt3)shr.b r9.x, (r)r7.x, 8
(rpt3)sub.u r8.x, r5.z, (r)r8.x
(rpt3)and.b r10.x, (r)r7.x, r5.x
(rpt3)and.b r9.x, (r)r9.x, r5.x
(rpt3)mul.u24 r10.x, (r)r10.x, (r)r8.x
(rpt3)mul.u24 r9.x, (r)r9.x, (r)r8.x
(rpt3)shr.b r11.x, (r)r10.x, 8
(rpt3)shr.b r12.x, (r)r9.x, 8
(rpt3)and.b r11.x, (r)r11.x, r5.x
(rpt3)and.b r12.x, (r)r12.x, r5.x
(rpt3)add.u r10.x, (r)r10.x, (r)r11.x
(rpt3)add.u r9.x, (r)r9.x, (r)r12.x
(rpt3)add.u r10.x, (r)r10.x, r5.y
(rpt3)add.u r9.x, (r)r9.x, r5.y
(rpt3)shr.b r10.x, (r)r10.x, 8
(rpt3)shr.b r9.x, (r)r9.x, 8
(rpt3)and.b r10.x, (r)r10.x, r5.x
(rpt3)and.b r9.x, (r)r9.x, r5.x
(rpt3)shl.b r9.x, (r)r9.x, 8
(rpt3)or.b r10.x, (r)r10.x, (r)r9.x
(rpt3)add.u r3.x, (r)r3.x, (r)r10.x
(rpt5)nop
stgb.untyped.4d.u32.4 g[1], r3.x, r1.y, r2.x
end
