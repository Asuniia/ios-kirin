#!/bin/bash
# Wrap a raw arm64 Image like a stock v0 boot.img with appended DTBs (Pixel 1 /
# sailfish): the Image replaces the kernel, the stock DTBs, ramdisk and header
# are kept. The kernel goes in as gzip: the sailfish aboot `fastboot boot`
# path says "dtb not found" for LZ4 kernels, even for the unmodified stock
# Image.lz4-dtb that boots fine from flash.
# Usage: tools/mkboot-v0.sh STOCK_BOOT.img Image OUT.img
set -euo pipefail
stock=$1 image=$2 out=$3
tmp=$(mktemp -d)
gzip -9 -n -c "$image" > "$tmp/Image.gz"
python3 -I - "$stock" "$tmp/Image.gz" "$out" <<'EOF'
import hashlib, struct, sys
stock, gz, out = sys.argv[1:]
b = open(stock, 'rb').read()
assert b[:8] == b'ANDROID!'
ksize, _, rsize, _, ssize, _, _, page, hv = struct.unpack_from('<9I', b, 8)
assert hv == 0, 'only v0 headers'
up = lambda n: (n + page - 1) // page * page
kernel = b[page:page + ksize]
dtbs = kernel[kernel.index(b'\xd0\x0d\xfe\xed'):]   # stock appended DTBs
ramdisk = b[page + up(ksize):][:rsize]
second = b[page + up(ksize) + up(rsize):][:ssize]
kernel = open(gz, 'rb').read() + dtbs
h = bytearray(b[:page])
struct.pack_into('<I', h, 8, len(kernel))
sha = hashlib.sha1()
for part in (kernel, ramdisk, second):
	sha.update(part); sha.update(struct.pack('<I', len(part)))
h[576:608] = sha.digest().ljust(32, b'\0')
pad = lambda d: d + b'\0' * (up(len(d)) - len(d))
open(out, 'wb').write(bytes(h) + pad(kernel) + pad(ramdisk) + pad(second))
print(f'{out}: kernel {len(kernel)} bytes ({len(dtbs)} of stock DTBs)')
EOF
rm -rf "$tmp"
