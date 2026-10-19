; Premultiplied source-over, BGRA8 (CoreAnimation's
; vAlpha_Blend_Premultiplied_BGRA_8888): dst = src + dst * (255 - src.a) / 255
; per channel, rounded like (t + 128 + (t >> 8)) >> 8, two channels per 32-bit
; lane pair (0x00ff00ff masks; 255 * 255 + 382 < 65536, no carry between
; lanes). g[0] = src, g[1] = dst (in place), one pixel per invocation.
; Delays by hand: >= 3 slots between dependent ALU ops, (sy) after loads.
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
shl.b r2.x, r1.x, 2
(rpt5)nop
ldgb.untyped.4d.u32.1 r3.x, g[0], r2.x, r1.x
ldgb.untyped.4d.u32.1 r3.y, g[1], r2.x, r1.x
(sy)shr.b r4.x, r3.x, 24
shr.b r4.z, r3.y, 8
and.b r4.y, r3.y, r5.x
(rpt1)nop
sub.u r4.x, r5.z, r4.x
and.b r4.z, r4.z, r5.x
(rpt2)nop
mul.u24 r4.y, r4.y, r4.x
mul.u24 r4.z, r4.z, r4.x
(rpt2)nop
shr.b r4.w, r4.y, 8
shr.b r6.x, r4.z, 8
(rpt2)nop
and.b r4.w, r4.w, r5.x
and.b r6.x, r6.x, r5.x
(rpt2)nop
add.u r4.y, r4.y, r4.w
add.u r4.z, r4.z, r6.x
(rpt2)nop
add.u r4.y, r4.y, r5.y
add.u r4.z, r4.z, r5.y
(rpt2)nop
shr.b r4.y, r4.y, 8
shr.b r4.z, r4.z, 8
(rpt2)nop
and.b r4.y, r4.y, r5.x
and.b r4.z, r4.z, r5.x
(rpt2)nop
shl.b r4.z, r4.z, 8
(rpt2)nop
or.b r4.y, r4.y, r4.z
(rpt2)nop
add.u r3.x, r3.x, r4.y
(rpt5)nop
stgb.untyped.4d.u32.1 g[1], r3.x, r1.x, r2.x
end
