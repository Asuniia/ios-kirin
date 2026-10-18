#!/bin/bash
# macOS twin of tools/mkiosroot.sh + tools/sweet-root-tweaks.py: build the
# iOS root (case-sensitive journaled HFS+, one volume, no partition map) for
# the userdata partition (rd=ufsroot0). Run as root (owners must be kept):
#
#   sudo tools/mkiosroot-mac.sh OS.dmg SYSTEMOS.dmg APPOS.dmg OUT.img [SIZE]   (SIZE default 16g)
#
# OUT.img is the raw volume (flash with `fastboot flash userdata OUT.img`).
# macOS reads the LZFSE/LZBITMAP-compressed IPSW files transparently and
# ditto --nopreserveHFSCompression writes them uncompressed (SWEET's HFS has
# no decmpfs decompressors). See mkiosroot.sh for what each step is for.
set -euo pipefail

[[ $# -ge 4 ]] || { sed -n '2,12p' "$0"; exit 2; }
[[ $(id -u) == 0 ]] || { echo "run as root (sudo)" >&2; exit 1; }
os=$1 sysos=$2 appos=$3 out=$4 size=${5:-16g}
[[ ! -e $out && ! -e $out.dmg ]] || { echo "refusing to overwrite $out" >&2; exit 1; }
here=$(cd "$(dirname "$0")" && pwd)

work=$(mktemp -d /tmp/mkiosroot.XXXXXX)
mounts=()	# bash 3.2 (macOS /bin/bash): empty "${a[@]}" trips set -u
cleanup() {
	for m in ${mounts[@]+"${mounts[@]}"}; do hdiutil detach -quiet "$m" 2>/dev/null || hdiutil detach -force -quiet "$m" || true; done
	rmdir "$work"/* "$work" 2>/dev/null || true
}
trap cleanup EXIT

attach() {	# IMAGE NAME [hdiutil options]
	mkdir "$work/$2"
	hdiutil attach -nobrowse -owners on -mountpoint "$work/$2" "${@:3}" "$1" >/dev/null
	mounts=("$work/$2" ${mounts[@]+"${mounts[@]}"})
}
attach "$os" os -readonly
# The cryptexes are raw APFS images, not UDIF.
attach "$sysos" sysos -readonly -imagekey diskimage-class=CRawDiskImage
attach "$appos" appos -readonly -imagekey diskimage-class=CRawDiskImage

hdiutil create -quiet -size "$size" -type UDIF -layout NONE \
	-fs "Case-sensitive Journaled HFS+" -volname iOSRoot "$out.dmg"
attach "$out.dmg" root
r=$work/root
mdutil -i off "$r" >/dev/null 2>&1 || true

copy() {
	echo "copying $1 -> /${2#"$r"/}"
	mkdir -p "$2"
	ditto --norsrc --noextattr --noqtn --noacl --nopreserveHFSCompression "$1" "$2"
}
copy "$work/os" "$r"
copy "$work/sysos" "$r/private/preboot/Cryptexes/OS"
copy "$work/appos" "$r/private/preboot/Cryptexes/App"
rm -rf "$r"/private/preboot/Cryptexes/*/.fseventsd

# Shared cache where dyld looks at ignition level 1: hard links.
dsc=$r/private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld
mkdir -p "$r/System/Library/Caches/com.apple.dyld"
for f in "$dsc"/*; do ln "$f" "$r/System/Library/Caches/com.apple.dyld/${f##*/}"; done

# Cryptex files without a counterpart in the OS root, linked to their root path.
for c in OS App; do
	(cd "$r/private/preboot/Cryptexes/$c" && find . ! -type d ! -path './System/Library/Caches/com.apple.dyld/*') |
	while IFS= read -r f; do
		f=${f#./}
		if [[ ! -e $r/$f && ! -L $r/$f ]]; then
			mkdir -p "$r/$(dirname "$f")"
			src=$r/private/preboot/Cryptexes/$c/$f
			ln -P "$src" "$r/$f" 2>/dev/null || cp -P "$src" "$r/$f"
			echo "linked $c:/$f"
		fi
	done
done

# logd's stores (owner _logd 272, .migrated markers).
db=$r/private/var/db
install -d -o 272 -g 272 -m 0751 "$db/diagnostics"
install -d -o 272 -g 272 -m 0755 "$db/uuidtext" "$db/diagnostics/timesync"
for d in diagnostics uuidtext diagnostics/timesync; do
	install -o 272 -g 272 -m 0644 /dev/null "$db/$d/.migrated"
done

# System keybag for the built-in AppleKeyStore (no passcode).
install -d -m 0700 -o 0 -g 0 "$r/private/var/keybags"
printf '%s\n' '<?xml version="1.0" encoding="UTF-8"?>' \
	'<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
	'<plist version="1.0"><dict><key>KeyBagKeys</key><data>U1dFRVQ=</data></dict></plist>' >"$r/private/var/keybags/systembag.kb"
chown 0:0 "$r/private/var/keybags/systembag.kb"
chmod 0600 "$r/private/var/keybags/systembag.kb"

python3 -I "$here/sweet-root-tweaks.py" "$r"
rm -rf "$r/.fseventsd" "$r/.Spotlight-V100" "$r/.Trashes"
df -h "$r"

hdiutil detach -quiet "$r"
mounts=("${mounts[@]:1}")	# root was first; bash 3.2: never empty here
dev=$(hdiutil attach -nobrowse -nomount "$out.dmg" | awk 'NR==1{print $1}')
fsck_hfs -fn "$dev"
hdiutil detach -quiet "$dev"

# hdiutil's UDRW image here is plain raw (CRawDiskImage, no UDIF trailer):
# the file is the volume itself. Reattach with
# `hdiutil attach -imagekey diskimage-class=CRawDiskImage OUT.img`.
[[ $(stat -f %z "$out.dmg") == $(( $(hdiutil imageinfo "$out.dmg" | awk '/Total Bytes/{print $3; exit}') )) ]] ||
	{ echo "$out.dmg is not a raw image" >&2; exit 1; }
mv "$out.dmg" "$out"
[[ $(xxd -s 1024 -l 2 -p "$out") == 4858 ]] || { echo "no HX signature at 1024" >&2; exit 1; }
chown "${SUDO_UID:-0}:${SUDO_GID:-0}" "$out"
ls -l "$out"
