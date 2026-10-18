#!/bin/bash
# Build the Pixel 1 (sailfish) shim: start.S + pixel.c + pstore.c + console.c, wrapped in
# a stock-style v0 boot.img by tools/mkboot-v0.sh.
# Usage: shim/build-pixel.sh STOCK_BOOT.img   -> shim/out/pixel-boot.img
set -euo pipefail
stock=$(realpath "$1")
cd "$(dirname "$0")"
X=aarch64-linux-gnu-
command -v ${X}gcc >/dev/null || X=aarch64-elf-	# macOS Homebrew
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra -DPIXEL1"
mkdir -p out/pixel
for f in start.S pixel.c pstore.c console.c; do
	${X}gcc $CFLAGS -c $f -o out/pixel/${f%.*}.o
done
# No relocation pass at runtime: only start.o's image_size may be absolute.
abs=$(${X}readelf -r out/pixel/pixel.o out/pixel/pstore.o out/pixel/console.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
${X}gcc $CFLAGS -static -T shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
	out/pixel/start.o out/pixel/pixel.o out/pixel/pstore.o out/pixel/console.o -o out/pixel/shim.elf
${X}objcopy -O binary out/pixel/shim.elf out/pixel/Image
size=$(${X}nm out/pixel/shim.elf | awk '$3=="_end"{print $1}')
truncate -s $((16#$size)) out/pixel/Image
../tools/mkboot-v0.sh "$stock" out/pixel/Image out/pixel-boot.img
