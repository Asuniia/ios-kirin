; sample.asm on 4 pixels per invocation (sweet_gpu_quad): the 4 pixels
; X..X+3 of row Y, X = xa + 4 * invocation (xa = x0 rounded down to 4), one
; 128-bit store. Per pixel: (u, v) = (a b; c d)^T (x+.5, y+.5) + (tx, ty),
; s = sample of t#0 (s#0) * color * w, d = t#1 (the dst image, s#1
; NEAREST) at the pixel, out = s + d * (1 - w * (1 - k1) - s.a * k0),
; clamped, RGBA8 (byte 0 = x). w = 1 inside [x0, x1) and on the inner side
; of the 4 edges (a x + b y + c >= 0 at the pixel center, as a steep ramp
; clamp(K (a x + b y + c) + 1)), else 0: pixels outside keep d.
; k0, k1 = 1, 1 source-over, 0, 1 add, 0, 0 copy.
; Constants:
;   c0 = a, b, c, d          c1 = tx, ty, opacity, 1.0
;   c2 = 255.0, 0.5, groups - 1 (u32), dst pitch in pixels (u32)
;   c3 = xa (u32), y0 (u32), k0, k1 - 1
;   c4 = x0 (u32), x1 (u32), -1.0, 0.0
;   c5 = color (per channel, memory order)
;   c6-c9 = edges a, b, c, 0 (unused: 0, 0, 1)    c10 = K, 0, 0, 0
; Grid: (ceil(groups / 64), h); groups past the last are clamped (same
; pixels twice, same values). Registers: r3.xy r3.zw r4.xy r4.zw pixel
; centers, r5.xy r5.zw r6.xy r6.zw (u, v), r15 x per pixel, r16-r19 s,
; r20-r23 d, r8 w, r9 edge term, r7 d factor, r24 out, r25 color,
; r26-r29 edges, r30 b y + c per edge, r31.x K.
@localsize 64, 1, 1
@constlen 12
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
mov.u32u32 r14.x, c4.x
mov.u32u32 r14.y, c4.y
mov.u32u32 r14.z, c4.z
mov.u32u32 r14.w, c4.w
mov.u32u32 r25.x, c5.x
mov.u32u32 r25.y, c5.y
mov.u32u32 r25.z, c5.z
mov.u32u32 r25.w, c5.w
mov.u32u32 r26.x, c6.x
mov.u32u32 r26.y, c6.y
mov.u32u32 r26.z, c6.z
mov.u32u32 r27.x, c7.x
mov.u32u32 r27.y, c7.y
mov.u32u32 r27.z, c7.z
mov.u32u32 r28.x, c8.x
mov.u32u32 r28.y, c8.y
mov.u32u32 r28.z, c8.z
mov.u32u32 r29.x, c9.x
mov.u32u32 r29.y, c9.y
mov.u32u32 r29.z, c9.z
mov.u32u32 r31.x, c10.x
mov.u32u32 r2.y, 0
; group, row, first pixel, dword offset
shl.b r1.x, r48.x, 6
(rpt2)nop
add.u r1.x, r1.x, r0.x
add.u r1.y, r48.y, r13.y
(rpt1)nop
min.u r1.x, r1.x, r12.z
(rpt2)nop
shl.b r1.x, r1.x, 2
(rpt2)nop
add.u r1.z, r1.x, r13.x
(rpt2)nop
mad.u24 r1.w, r1.y, r12.w, r1.z
cov.u32f32 r3.x, r1.z
cov.u32f32 r3.y, r1.y
cov.u32f32 r14.x, r14.x
cov.u32f32 r14.y, r14.y
(rpt1)nop
shl.b r2.x, r1.w, 2
add.f r3.x, r3.x, r12.y
add.f r3.y, r3.y, r12.y
; c4.x -> 0.5 - x0, c4.y -> x1 + 0.5
add.f r14.x, r12.y, (neg)r14.x
add.f r14.y, r14.y, r12.y
(rpt1)nop
; pixel centers
add.f r3.z, r3.x, r11.w
mov.f32f32 r3.w, r3.y
mov.f32f32 r4.y, r3.y
mov.f32f32 r4.w, r3.y
(rpt1)nop
add.f r4.x, r3.z, r11.w
(rpt2)nop
add.f r4.z, r4.x, r11.w
; edges: b y + c per edge (this row)
mad.f32 r30.x, r26.y, r3.y, r26.z
mad.f32 r30.y, r27.y, r3.y, r27.z
mad.f32 r30.z, r28.y, r3.y, r28.z
mad.f32 r30.w, r29.y, r3.y, r29.z
; u, v: base (c y + tx, d y + ty), then + (a, b) x
mad.f32 r7.x, r10.z, r3.y, r11.x
mad.f32 r7.y, r10.w, r3.y, r11.y
(rpt2)nop
mad.f32 r5.x, r10.x, r3.x, r7.x
mad.f32 r5.y, r10.y, r3.x, r7.y
mad.f32 r5.z, r10.x, r3.z, r7.x
mad.f32 r5.w, r10.y, r3.z, r7.y
mad.f32 r6.x, r10.x, r4.x, r7.x
mad.f32 r6.y, r10.y, r4.x, r7.y
mad.f32 r6.z, r10.x, r4.z, r7.x
mad.f32 r6.w, r10.y, r4.z, r7.y
mov.f32f32 r15.x, r3.x
mov.f32f32 r15.y, r3.z
mov.f32f32 r15.z, r4.x
mov.f32f32 r15.w, r4.z
(rpt2)nop
sam (f32)(xyzw)r16.x, r5.x, s#0, t#0
sam (f32)(xyzw)r17.x, r5.z, s#0, t#0
sam (f32)(xyzw)r18.x, r6.x, s#0, t#0
sam (f32)(xyzw)r19.x, r6.z, s#0, t#0
sam (f32)(xyzw)r20.x, r3.x, s#1, t#1
sam (f32)(xyzw)r21.x, r3.z, s#1, t#1
sam (f32)(xyzw)r22.x, r4.x, s#1, t#1
sam (f32)(xyzw)r23.x, r4.z, s#1, t#1
; coverage while the samples run: w = clamp(x + 0.5 - x0) * clamp(x1 + 0.5 - x)
(rpt3)add.f r8.x, (r)r15.x, r14.x
(rpt3)mad.f32 r9.x, (r)r15.x, r14.z, r14.y
(rpt3)max.f r8.x, (r)r8.x, r14.w
(rpt3)max.f r9.x, (r)r9.x, r14.w
(rpt3)min.f r8.x, (r)r8.x, r11.w
(rpt3)min.f r9.x, (r)r9.x, r11.w
(rpt3)mul.f r8.x, (r)r8.x, (r)r9.x
; times each edge's clamp(K (a x + b y + c) + 1)
(rpt3)mad.f32 r9.x, (r)r15.x, r26.x, r30.x
(rpt2)nop
(rpt3)mad.f32 r9.x, (r)r9.x, r31.x, r11.w
(rpt3)max.f r9.x, (r)r9.x, r14.w
(rpt3)min.f r9.x, (r)r9.x, r11.w
(rpt3)mul.f r8.x, (r)r8.x, (r)r9.x
(rpt3)mad.f32 r9.x, (r)r15.x, r27.x, r30.y
(rpt2)nop
(rpt3)mad.f32 r9.x, (r)r9.x, r31.x, r11.w
(rpt3)max.f r9.x, (r)r9.x, r14.w
(rpt3)min.f r9.x, (r)r9.x, r11.w
(rpt3)mul.f r8.x, (r)r8.x, (r)r9.x
(rpt3)mad.f32 r9.x, (r)r15.x, r28.x, r30.z
(rpt2)nop
(rpt3)mad.f32 r9.x, (r)r9.x, r31.x, r11.w
(rpt3)max.f r9.x, (r)r9.x, r14.w
(rpt3)min.f r9.x, (r)r9.x, r11.w
(rpt3)mul.f r8.x, (r)r8.x, (r)r9.x
(rpt3)mad.f32 r9.x, (r)r15.x, r29.x, r30.w
(rpt2)nop
(rpt3)mad.f32 r9.x, (r)r9.x, r31.x, r11.w
(rpt3)max.f r9.x, (r)r9.x, r14.w
(rpt3)min.f r9.x, (r)r9.x, r11.w
(rpt3)mul.f r8.x, (r)r8.x, (r)r9.x
(rpt2)nop
(rpt3)mad.f32 r7.x, (r)r8.x, r13.w, r11.w
; s *= color, then * w
(sy)(rpt3)mul.f r16.x, (r)r16.x, (r)r25.x
(rpt3)mul.f r17.x, (r)r17.x, (r)r25.x
(rpt3)mul.f r18.x, (r)r18.x, (r)r25.x
(rpt3)mul.f r19.x, (r)r19.x, (r)r25.x
(rpt3)mul.f r16.x, (r)r16.x, r8.x
(rpt3)mul.f r17.x, (r)r17.x, r8.y
(rpt3)mul.f r18.x, (r)r18.x, r8.z
(rpt3)mul.f r19.x, (r)r19.x, r8.w
(rpt2)nop
; d factor -= s.a * k0
mad.f32 r7.x, (neg)r16.w, r13.z, r7.x
mad.f32 r7.y, (neg)r17.w, r13.z, r7.y
mad.f32 r7.z, (neg)r18.w, r13.z, r7.z
mad.f32 r7.w, (neg)r19.w, r13.z, r7.w
(rpt2)nop
; out = d * factor + s, * 255 + .5, <= 255, to u32
(rpt3)mad.f32 r20.x, (r)r20.x, r7.x, (r)r16.x
(rpt3)mad.f32 r21.x, (r)r21.x, r7.y, (r)r17.x
(rpt3)mad.f32 r22.x, (r)r22.x, r7.z, (r)r18.x
(rpt3)mad.f32 r23.x, (r)r23.x, r7.w, (r)r19.x
(rpt3)mad.f32 r20.x, (r)r20.x, r12.x, r12.y
(rpt3)mad.f32 r21.x, (r)r21.x, r12.x, r12.y
(rpt3)mad.f32 r22.x, (r)r22.x, r12.x, r12.y
(rpt3)mad.f32 r23.x, (r)r23.x, r12.x, r12.y
(rpt3)min.f r20.x, (r)r20.x, r12.x
(rpt3)min.f r21.x, (r)r21.x, r12.x
(rpt3)min.f r22.x, (r)r22.x, r12.x
(rpt3)min.f r23.x, (r)r23.x, r12.x
(rpt3)cov.f32u32 r20.x, (r)r20.x
(rpt3)cov.f32u32 r21.x, (r)r21.x
(rpt3)cov.f32u32 r22.x, (r)r22.x
(rpt3)cov.f32u32 r23.x, (r)r23.x
(rpt2)nop
; pack: byte c of pixel i = channel c
shl.b r20.y, r20.y, 8
shl.b r21.y, r21.y, 8
shl.b r22.y, r22.y, 8
shl.b r23.y, r23.y, 8
shl.b r20.z, r20.z, 16
shl.b r21.z, r21.z, 16
shl.b r22.z, r22.z, 16
shl.b r23.z, r23.z, 16
shl.b r20.w, r20.w, 24
shl.b r21.w, r21.w, 24
shl.b r22.w, r22.w, 24
shl.b r23.w, r23.w, 24
or.b r20.x, r20.x, r20.y
or.b r21.x, r21.x, r21.y
or.b r22.x, r22.x, r22.y
or.b r23.x, r23.x, r23.y
or.b r20.z, r20.z, r20.w
or.b r21.z, r21.z, r21.w
or.b r22.z, r22.z, r22.w
or.b r23.z, r23.z, r23.w
(rpt2)nop
or.b r24.x, r20.x, r20.z
or.b r24.y, r21.x, r21.z
or.b r24.z, r22.x, r22.z
or.b r24.w, r23.x, r23.z
(rpt5)nop
stgb.untyped.4d.u32.4 g[0], r24.x, r1.w, r2.x
end
