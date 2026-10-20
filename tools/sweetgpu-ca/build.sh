#!/bin/sh
# Build sweetgpu-ca.dylib for iOS 16 arm64 with the local Xcode, ad-hoc
# signed and zero-padded to 1 MiB: the file on the phone keeps one size
# (and so one extent), so new versions can be written over it in place.
set -e
H=$(cd "$(dirname "$0")" && pwd)
export DEVELOPER_DIR=$H/../../research/xcode15/Xcode.app/Contents/Developer
O=$H/out/sweetgpu-ca.dylib
mkdir -p "$H/out"
xcrun -sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0 -O2 -Wall -dynamiclib \
	-install_name /usr/lib/sweetgpu-ca.dylib -framework IOKit -o "$O" "$H/sweetgpu-ca.c"
codesign -f -s - "$O"
SIZE=$(stat -f %z "$O")
[ "$SIZE" -le 1048576 ] || { echo "$O is $SIZE bytes, over 1 MiB"; exit 1; }
truncate -s 1048576 "$O"
echo "built $O ($SIZE bytes, padded to 1 MiB)"
