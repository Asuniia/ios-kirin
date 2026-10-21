#!/bin/bash
# Write the iOS root (tools/mkiosroot-kirin.sh) to the Xperia 10's userdata
# partition. THIS ERASES ANDROID'S DATA. Nothing else is touched: no boot,
# bootloader, modem or persist partition is ever written here.
#
#   flash-userdata-kirin.sh ROOT.img
#
# Checks first: an HFS+ volume header in the image, the phone in fastboot
# mode, the image no larger than userdata. Then converts it to a sparse
# image (tools/img2simg.py, next to ROOT.img) and asks for "ERASE" to be
# typed before flashing. Back to Android afterwards: fastboot erase userdata,
# then boot normally (LineageOS formats an empty userdata on first boot).
set -euo pipefail

[[ $# -eq 1 ]] || { sed -n '2,13p' "$0"; exit 2; }
img=$1
tools=$(dirname "$(realpath "$0")")

sig=$(dd if="$img" bs=1 skip=1024 count=2 2>/dev/null)
[[ $sig == "H+" || $sig == "HX" ]] || { echo "$img: no HFS+ volume header" >&2; exit 1; }

fastboot devices | grep -q fastboot || { echo "phone not in fastboot mode" >&2; exit 1; }
part=$(fastboot getvar partition-size:userdata 2>&1 | awk '/partition-size:userdata:/{print $NF}')
[[ -n $part ]] || { echo "could not read the userdata size" >&2; exit 1; }
size=$(stat -c %s "$img")
(( size <= part )) || { echo "image ($size bytes) larger than userdata ($((part)) bytes)" >&2; exit 1; }
echo "userdata: $((part >> 20)) MiB, image: $((size >> 20)) MiB"

simg=${img%.img}.simg
if [[ ! -e $simg || $simg -ot $img ]]; then
	python3 -I "$tools/img2simg.py" "$img" "$simg"
fi

echo "This erases everything in Android's userdata (apps, photos, settings)."
read -r -p 'Type ERASE to flash: ' answer
[[ $answer == ERASE ]] || { echo "not flashed"; exit 1; }
fastboot flash userdata "$simg"
