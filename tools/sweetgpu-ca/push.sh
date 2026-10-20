#!/bin/sh
# Write out/sweetgpu-ca.dylib over /usr/lib/sweetgpu-ca.dylib on the phone
# (TWRP, adb): only the file's own 256 blocks on sda35 (extents from delta10,
# see NOTES.md; user-approved for this file), then read back after dropping
# caches. Refuses if those blocks don't start with a Mach-O header.
set -e
H=$(cd "$(dirname "$0")" && pwd)
D=$H/../../research/phone-root/delta10
O=$H/out/sweetgpu-ca.dylib
T=${TMPDIR:-/tmp}/sweetgpu-push.$$
[ "$(stat -f %z "$O")" = 1048576 ] || { echo "$O is not 1 MiB"; exit 1; }
adb push "$D/dylib-extents.txt" "$D/readruns.sh" "$D/writeruns.sh" "$O" /tmp/ >/dev/null
adb shell 'dd if=/dev/block/sda35 bs=4096 skip=2053690 count=1 2>/dev/null | head -c 4' > $T.magic
[ "$(xxd -p $T.magic)" = cffaedfe ] || { echo "extent does not start with a Mach-O header: $(xxd -p $T.magic)"; exit 1; }
adb shell 'sh /tmp/writeruns.sh /tmp/dylib-extents.txt /tmp/sweetgpu-ca.dylib && echo 3 > /proc/sys/vm/drop_caches && sh /tmp/readruns.sh /tmp/dylib-extents.txt /tmp/back'
adb pull /tmp/back $T.back >/dev/null
cmp $T.back "$O" && echo "VERIFY OK: $(shasum -a 256 "$O" | cut -c1-16)"
rm -f $T.magic $T.back
