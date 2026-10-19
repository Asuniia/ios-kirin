#!/bin/sh
# Build sweetgpu-test for iOS 16 arm64 with the local Xcode (research/xcode15).
set -e
H=$(cd "$(dirname "$0")" && pwd)
export DEVELOPER_DIR=$H/../../research/xcode15/Xcode.app/Contents/Developer
mkdir -p "$H/out"
xcrun -sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0 -O2 -Wall \
	-framework IOKit -o "$H/out/sweetgpu-test" "$H/sweetgpu-test.c"
codesign -f -s - "$H/out/sweetgpu-test"
echo "built $H/out/sweetgpu-test"
