#!/bin/bash
# Build tools/granule-probe.S into a sailfish boot image (tools/mkboot-v0.sh).
# Usage: tools/granule-probe.sh STOCK_BOOT.img OUT.img
set -euo pipefail
tmp=$(mktemp -d)
aarch64-elf-gcc -nostdlib -static -Wl,-Ttext=0 -Wl,--build-id=none \
	"$(dirname "$0")/granule-probe.S" -o "$tmp/probe.elf"
aarch64-elf-objcopy -O binary "$tmp/probe.elf" "$tmp/Image"
"$(dirname "$0")/mkboot-v0.sh" "$1" "$tmp/Image" "$2"
rm -rf "$tmp"
