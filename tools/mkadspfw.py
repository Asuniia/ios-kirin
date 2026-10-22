#!/usr/bin/env python3
"""Pack the ADSP firmware (adsp.mdt + adsp.bNN) into one payload blob.

  mkadspfw.py DIR OUT

DIR holds the files from the phone's modem partition (image/adsp.*; not in
the repository). OUT is what bsd/dev/sweet_adsp.c reads as ADSP-FW
(mkpayload.py --blob ADSP-FW=OUT): 'SADF', u32 segment count, u32 offset
and length of the .mdt, then per program header u32 offset and length of
its .bNN file (0, 0 when there is none), then the files, 16-byte aligned.
"""
import os
import struct
import sys


def main():
    d, out = sys.argv[1], sys.argv[2]
    mdt = open(os.path.join(d, 'adsp.mdt'), 'rb').read()
    phnum = struct.unpack_from('<H', mdt, 0x2c)[0]
    files = [mdt] + [open(os.path.join(d, 'adsp.b%02d' % i), 'rb').read()
                     if os.path.exists(os.path.join(d, 'adsp.b%02d' % i)) else b'' for i in range(phnum)]
    pos = 16 + 8 * phnum
    offs = []
    body = b''
    for f in files:
        pad = (-(pos + len(body))) % 16
        body += b'\0' * pad
        offs.append((pos + len(body), len(f)) if f else (0, 0))
        body += f
    head = b'SADF' + struct.pack('<III', phnum, *offs[0])
    head += b''.join(struct.pack('<II', *o) for o in offs[1:])
    open(out, 'wb').write(head + body)
    print(f'{out}: {phnum} segments, {len(head) + len(body)} bytes')


if __name__ == '__main__':
    main()
