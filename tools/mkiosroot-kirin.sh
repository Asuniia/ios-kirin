#!/bin/bash
# Build the kirin iOS root on Linux: tools/mkiosroot.sh, then the steps the
# README does by hand on a Mac, minus the GL library (no GPU driver on kirin yet):
#   - tools/sweet-root-tweaks.py (launchd jobs, Setup skipped, ...)
#   - the stock apps moved from /private/var/staged_system_apps to /Applications
#   - PNG twins of the HEIC wallpaper layers (tools/wallpaper-heic2png.py, with
#     pillow-heif from $HEIC_PYTHON, default research/heicenv/bin/python)
#
#   mkiosroot-kirin.sh OS.dmg SYSTEMOS.dmg APPOS.dmg OUT.img [SIZE]
#
# If OUT.img already exists (mkiosroot.sh done), only the steps above run.
# fsck.hfsplus -n on Linux keeps reporting "Volume header needs minor repair"
# right after -y finds the volume OK; mkiosroot.sh then exits non-zero after
# copying everything, which is why this resumes.
#
# Needs apfs-fuse in PATH, hfsplus-tools, rsync and sudo.
set -euo pipefail

[[ $# -ge 4 ]] || { sed -n '2,16p' "$0"; exit 2; }
tools=$(dirname "$(realpath "$0")")
out=$4
heic_python=${HEIC_PYTHON:-$tools/../research/heicenv/bin/python}
"$heic_python" -I -c 'import pillow_heif' || {
	echo "no pillow-heif: python3 -m venv research/heicenv && research/heicenv/bin/pip install pillow-heif" >&2
	exit 1
}

if [[ -e $out ]]; then
	echo "$out exists: skipping mkiosroot.sh"
else
	"$tools/mkiosroot.sh" "$@" || echo "mkiosroot.sh exited $? (see the fsck output above)"
fi

mnt=$(mktemp -d)
cleanup() {
	if mountpoint -q "$mnt"; then
		sudo umount "$mnt"
	fi
	rmdir "$mnt"
}
trap cleanup EXIT

sudo mount -t hfsplus -o loop,force "$out" "$mnt"
sudo python3 -I "$tools/sweet-root-tweaks.py" "$mnt"
sudo "$heic_python" -I "$tools/wallpaper-heic2png.py" "$mnt"

staged="$mnt/private/var/staged_system_apps"
if sudo test -d "$staged"; then
	for app in $(sudo find "$staged" -mindepth 1 -maxdepth 1 -name '*.app' -printf '%f\n'); do
		if sudo test -e "$mnt/Applications/$app"; then
			echo "kept /Applications/$app"
		else
			sudo mv "$staged/$app" "$mnt/Applications/$app"
			echo "moved $app"
		fi
	done
fi

df -h "$mnt"
sudo umount "$mnt"
fsck.hfsplus -f -y "$out" | grep -v 'Illegal name' || true
