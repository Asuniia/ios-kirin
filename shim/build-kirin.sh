#!/bin/bash
# Build the Xperia 10 (kirin) shim: start.S + kirin.c + console.c + emmc.c + fdt.c + loader.c,
# wrapped in a stock-style v0 boot.img by tools/mkboot-v0.sh.
# A PAYLOAD (tools/mkpayload.py) is appended to the Image at _end (see kirin.c).
# Usage: [EMMC_PROBE=1] shim/build-kirin.sh STOCK_BOOT.img [PAYLOAD]   -> shim/out/kirin-boot.img
# EMMC_PROBE=1: show the eMMC host state and the GPT instead of booting (read-only, emmc.c).
# Uses an aarch64 GNU toolchain if there is one, else clang + lld.
set -euo pipefail
stock=$(realpath "$1")
payload=${2:+$(realpath "$2")}
cd "$(dirname "$0")"
FLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra -DKIRIN${EMMC_PROBE:+ -DKIRIN_EMMC_PROBE}"
if command -v aarch64-linux-gnu-gcc >/dev/null; then
	CC="aarch64-linux-gnu-gcc" LDFLAGS="-Wl,--no-warn-rwx-segments" X=aarch64-linux-gnu-
elif command -v aarch64-elf-gcc >/dev/null; then	# macOS Homebrew
	CC="aarch64-elf-gcc" LDFLAGS="-Wl,--no-warn-rwx-segments" X=aarch64-elf-
else
	CC="clang --target=aarch64-none-elf" LDFLAGS="-fuse-ld=lld -Wl,--image-base=0" X=llvm-
fi
mkdir -p out/kirin
for f in start.S kirin.c console.c emmc.c fdt.c loader.c; do
	$CC $FLAGS -c $f -o out/kirin/${f%.*}.o
done
# No relocation pass at runtime: only start.o's image_size may be absolute.
abs=$(${X}readelf -r out/kirin/kirin.o out/kirin/console.o out/kirin/emmc.o out/kirin/fdt.o out/kirin/loader.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
$CC $FLAGS -static -T shim.ld $LDFLAGS -Wl,--build-id=none -Wl,-N \
	out/kirin/start.o out/kirin/kirin.o out/kirin/console.o out/kirin/emmc.o out/kirin/fdt.o out/kirin/loader.o \
	-o out/kirin/shim.elf
${X}objcopy -O binary out/kirin/shim.elf out/kirin/Image
size=$(${X}nm out/kirin/shim.elf | awk '$3=="_end"{print $1}')
truncate -s $((16#$size)) out/kirin/Image
[ -z "$payload" ] || cat "$payload" >> out/kirin/Image
../tools/mkboot-v0.sh "$stock" out/kirin/Image "out/kirin${EMMC_PROBE:+-emmc}-boot.img"
