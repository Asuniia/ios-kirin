; GPU step 4 test kernel: out[i] = i ^ 0x5a5a0000, i = global invocation id
; (16 workgroups of 64). Delays by hand: 3 slots between dependent ALU ops.
; stgb operands: value, dword offset, 64-bit byte offset pair (r2.xy; 0131a
; faulted with r2.y left as garbage).
@localsize 64, 1, 1
@buf 1024
@invocationid(r0.x)
@wgid(r48.x)
mov.u32u32 r3.y, 0x5a5a0000
mov.u32u32 r2.y, 0
shl.b r1.x, r48.x, 6
(rpt2)nop
add.u r1.x, r1.x, r0.x
(rpt2)nop
shl.b r2.x, r1.x, 2
xor.b r3.x, r1.x, r3.y
(rpt5)nop
stgb.untyped.4d.u32.1 g[0], r3.x, r1.x, r2.x
end
