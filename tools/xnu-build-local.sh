#!/bin/bash
# Local replacement for .github/workflows/xnu.yml (the repo stays local):
# blacktop/darwin-xnu-build at the CI's pinned commit, our xnu/patches, the
# Xcode 15 toolchain (Xcode 16+ iig emits IORPC.kernelContent that xnu-8796
# lacks) and the KDK unpacked in research/.
# Usage: tools/xnu-build-local.sh OUTDIR     (log in OUTDIR/build.log)
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
OUT=$(mkdir -p "$1" && cd "$1" && pwd)
rm -rf "$OUT"/kernel.* "$OUT/SHA256SUMS"	# a failed build must not leave an old kernel
WORK=$REPO/research/xnu-build
DXB_REPO=https://github.com/blacktop/darwin-xnu-build
DXB_SHA=663906528d1cd5ea8fde7de6f16de73db9511549
XCODE=${XCODE:-$REPO/research/xcode15/Xcode.app}
KDK=${KDK:-$REPO/research/kdk/KDK_13.5_22G74.kdk}
[ -d "$XCODE" ] || { echo "no Xcode 15 at $XCODE" >&2; exit 1; }
[ -d "$KDK" ] || { echo "no KDK at $KDK" >&2; exit 1; }

if [ ! -d "$WORK" ]; then
	git init -q "$WORK"
	git -C "$WORK" fetch -q --depth 1 "$DXB_REPO" "$DXB_SHA"
	git -C "$WORK" checkout -q FETCH_HEAD
fi
# libfirehose_kernel is built for x86_64/arm64e with Xcode's default CPU (LSE,
# LDAPR); the SWEET kernel needs plain arm64 for the Pixel 1 Kryo (ARMv8.0).
# '$(inherited)' (single quotes in build.sh) keeps the xcconfig's -mkernel etc.
git -C "$WORK" checkout -q -- build.sh
sed -i '' 's/ARCHS="x86_64 arm64e" VALID_ARCHS="x86_64 arm64e"/ARCHS="arm64" VALID_ARCHS="arm64" OTHER_CFLAGS='"'"'$(inherited) -mcpu=kryo'"'"'/' "$WORK/build.sh"
grep -q "OTHER_CFLAGS='\$(inherited) -mcpu=kryo'" "$WORK/build.sh"
# Our patches next to blacktop's (12.5-14.3 use the top-level patches dir).
rm -f "$WORK"/patches/0[0-9][0-9][0-9]-*
cp "$REPO"/xnu/patches/*.patch "$REPO"/xnu/patches/*.sh "$WORK/patches/"
# Pristine xnu source every run: build.sh re-applies everything; the
# dependency clones, fakeroot and object dirs stay (incremental build).
if [ -d "$WORK/xnu/.git" ]; then
	git -C "$WORK/xnu" reset -q --hard
	git -C "$WORK/xnu" clean -qfdx
fi

cd "$WORK"
# build.sh skips the kernel if this exists (and we would copy a stale one):
# remove it, make still rebuilds only what changed in build/xnu.obj.
rm -rf build/xnu.obj/kernel.development.sweet* fakeroot/System/Library/Kernels/kernel.development.sweet*
env PATH="$REPO/research/bin:/opt/homebrew/bin:$PATH" \
	DEVELOPER_DIR="$XCODE/Contents/Developer" KDKROOT="$KDK" \
	MACOS_VERSION=13.5 KERNEL_CONFIG=DEVELOPMENT MACHINE_CONFIG=SWEET \
	/opt/homebrew/bin/bash build.sh 2>&1 | tee "$OUT/build.log"

# build.sh only warns when a patch does not apply: refuse a stale kernel.
if grep -E 'Skipping patch .*/0[0-9]{3}-' "$OUT/build.log"; then
	echo "error: some xnu/patches were not applied" >&2
	exit 1
fi
cp -R fakeroot/System/Library/Kernels/. "$OUT/"
(cd "$OUT" && find kernel.* -type f -maxdepth 0 -exec shasum -a 256 {} + | tee SHA256SUMS)
