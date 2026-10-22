#!/usr/bin/env python3
"""Receive a partition the kernel sends with boot-arg sweetdump=NAME.

  sweet-dump-recv.py OUT [PORT]

Datagrams from the phone (bsd/dev/sweet_usb.c ud_dump_thread) to UDP PORT
(default 5141): 'SWDP', u32 pass, u64 byte offset, then 1024 bytes; the
partition comes twice, so a datagram lost in one pass is filled by the
other. A datagram at offset ~0 carries the size and ends it. Prints what is
still missing at the end.
"""
import socket
import struct
import sys


def main():
    out, port = sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 5141
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(('0.0.0.0', port))
    got = set()
    size = None
    with open(out, 'wb') as f:
        while True:
            d, _ = s.recvfrom(2048)
            if len(d) < 16 or d[:4] != b'SWDP':
                continue
            pas, off = struct.unpack_from('<IQ', d, 4)
            if off == 0xffffffffffffffff:
                size = struct.unpack_from('<Q', d, 16)[0]
                break
            if off not in got:
                f.seek(off)
                f.write(d[16:])
                got.add(off)
                if len(got) % 4096 == 0:
                    print(f'{len(got) * 1024 >> 20} MB (pass {pas})', flush=True)
        f.truncate(size)
    missing = [o for o in range(0, size, 1024) if o not in got]
    print(f'{out}: {size} bytes, {len(missing)} KB chunk(s) missing'
          + (f' (first at {missing[0]:#x})' if missing else ''))
    return 1 if missing else 0


if __name__ == '__main__':
    sys.exit(main())
