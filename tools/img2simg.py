#!/usr/bin/env python3
"""Raw image -> Android sparse image (img2simg is not in Homebrew's platform-tools;
fastboot 37 on macOS fails "Failed reading from userdata" on a 16 GiB raw file).

  img2simg.py RAW.img OUT.simg

Zero blocks become FILL chunks (written as zeros, not left as old data).
"""
import struct, sys

BLK = 4096
RAW, FILL = 0xCAC1, 0xCAC2
ZERO = bytes(BLK)


def chunks(f):
    """Yield (kind, data_or_None, nblocks) runs."""
    kind, run, n = None, [], 0
    while True:
        buf = f.read(BLK * 1024)
        if not buf:
            break
        assert len(buf) % BLK == 0, 'image size must be a multiple of 4096'
        for i in range(0, len(buf), BLK):
            b = buf[i:i + BLK]
            k = FILL if b == ZERO else RAW
            # Keep RAW chunks under 64 MiB so fastboot can split them under max-download-size.
            if k != kind or (k == RAW and n == 16384):
                if kind:
                    yield kind, b''.join(run), n
                kind, run, n = k, [], 0
            if k == RAW:
                run.append(b)
            n += 1
    if kind:
        yield kind, b''.join(run), n


def main(src, dst):
    total, count = 0, 0
    with open(src, 'rb') as f, open(dst, 'wb') as o:
        o.write(bytes(28))
        for kind, data, n in chunks(f):
            if kind == RAW:
                o.write(struct.pack('<HHII', RAW, 0, n, 12 + len(data)) + data)
            else:
                o.write(struct.pack('<HHIII', FILL, 0, n, 16, 0))
            total += n
            count += 1
        o.seek(0)
        o.write(struct.pack('<IHHHHIIII', 0xED26FF3A, 1, 0, 28, 12, BLK, total, count, 0))
    print(f'{dst}: {total} blocks, {count} chunks')


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(*sys.argv[1:])
