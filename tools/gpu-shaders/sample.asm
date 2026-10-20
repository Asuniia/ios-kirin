; Textured-quad composite (CA image_sampler + source-over): for each pixel
; (X, Y) of the dst rect, (u, v) = (a b; c d)^T (X+.5, Y+.5) + (tx, ty) in
; unnormalized texel coords, s = bilinear sample of t#0 (s#0: LINEAR,
; clamp-to-edge) * opacity, d = t#1 at the pixel (s#1: NEAREST; t#1 is the
; dst image itself), out = s + d * (k1 - s.a * k0), clamped, stored as RGBA8
; (byte 0 = x): k0, k1 = 1, 1 source-over, 0, 1 add, 0, 0 copy.
; Constants (uploaded per dispatch):
;   c0 = a, b, c, d          c1 = tx, ty, opacity, 1.0
;   c2 = 255.0, 0.5, w - 1 (u32), dst pitch in pixels (u32)
;   c3 = x0 (u32), y0 (u32), k0, k1
; Grid: groups (ceil(w / 64), h); x past w - 1 is clamped (same pixel twice).
; cat2 may read one const, cat3 none in src2: the constants go to r10-r13.
@localsize 64, 1, 1
@constlen 4
@buf 4
@invocationid(r0.x)
@wgid(r48.x)
mov.u32u32 r10.x, c0.x
mov.u32u32 r10.y, c0.y
mov.u32u32 r10.z, c0.z
mov.u32u32 r10.w, c0.w
mov.u32u32 r11.x, c1.x
mov.u32u32 r11.y, c1.y
mov.u32u32 r11.z, c1.z
mov.u32u32 r11.w, c1.w
mov.u32u32 r12.x, c2.x
mov.u32u32 r12.y, c2.y
mov.u32u32 r12.z, c2.z
mov.u32u32 r12.w, c2.w
mov.u32u32 r13.x, c3.x
mov.u32u32 r13.y, c3.y
mov.u32u32 r13.z, c3.z
mov.u32u32 r13.w, c3.w
mov.u32u32 r2.y, 0
shl.b r1.x, r48.x, 6
(rpt2)nop
add.u r1.x, r1.x, r0.x
add.u r1.y, r48.y, r13.y
(rpt1)nop
min.u r1.x, r1.x, r12.z
(rpt2)nop
add.u r1.x, r1.x, r13.x
(rpt2)nop
mad.u24 r1.z, r1.y, r12.w, r1.x
cov.u32f32 r3.x, r1.x
cov.u32f32 r3.y, r1.y
(rpt1)nop
add.f r3.x, r3.x, r12.y
add.f r3.y, r3.y, r12.y
shl.b r2.x, r1.z, 2
(rpt1)nop
mad.f32 r4.x, r10.x, r3.x, r11.x
mad.f32 r4.y, r10.y, r3.x, r11.y
(rpt2)nop
mad.f32 r4.x, r10.z, r3.y, r4.x
mad.f32 r4.y, r10.w, r3.y, r4.y
(rpt2)nop
sam (f32)(xyzw)r5.x, r4.x, s#0, t#0
sam (f32)(xyzw)r6.x, r3.x, s#1, t#1
(sy)(rpt3)mul.f r5.x, (r)r5.x, r11.z
(rpt2)nop
mad.f32 r7.x, (neg)r5.w, r13.z, r13.w
(rpt2)nop
(rpt3)mad.f32 r8.x, (r)r6.x, r7.x, (r)r5.x
(rpt3)mad.f32 r8.x, (r)r8.x, r12.x, r12.y
(rpt3)min.f r8.x, (r)r8.x, r12.x
(rpt3)cov.f32u32 r9.x, (r)r8.x
(rpt2)nop
shl.b r9.y, r9.y, 8
shl.b r9.z, r9.z, 16
shl.b r9.w, r9.w, 24
(rpt2)nop
or.b r9.x, r9.x, r9.y
or.b r9.z, r9.z, r9.w
(rpt2)nop
or.b r9.x, r9.x, r9.z
(rpt5)nop
stgb.untyped.4d.u32.1 g[0], r9.x, r1.z, r2.x
end
