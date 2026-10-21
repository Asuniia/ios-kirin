#!/usr/bin/env python3
"""Linux version of wallpaper-heic2png.sh (which needs macOS sips).

  sudo PYTHON_WITH_PILLOW_HEIF -I wallpaper-heic2png.py MOUNTED_ROOT

The stock layered wallpapers' layers (/Library/Wallpaper/Collections/*/
Wallpapers/*.wallpaper/*.ca/assets/*.HEIC) need the A11's hardware HEVC
decoder, which the phone lacks. Each HEIC gets a lossless PNG twin (alpha
kept, same owner and mode) and the layer's main.caml points at it; the HEIC
files stay. Decoding uses pillow-heif (pip, bundles libheif). Idempotent.
"""
import os, sys

import pillow_heif
from PIL import Image

pillow_heif.register_heif_opener()


def main(root):
    base = os.path.join(root, 'Library/Wallpaper/Collections')
    converted = edited = 0
    for d, _, files in os.walk(base):
        for name in files:
            path = os.path.join(d, name)
            if name.endswith('.HEIC'):
                png = path[:-len('.HEIC')] + '.png'
                if not os.path.exists(png):
                    with Image.open(path) as im:
                        im.save(png, 'PNG')
                    st = os.stat(path)
                    os.chown(png, st.st_uid, st.st_gid)
                    os.chmod(png, st.st_mode & 0o7777)
                converted += 1
            elif name == 'main.caml':
                with open(path, 'rb') as f:
                    text = f.read()
                if b'.HEIC"' in text:
                    with open(path, 'r+b') as f:
                        f.write(text.replace(b'.HEIC"', b'.png"'))
                        f.truncate()
                    edited += 1
    print(f'{converted} HEIC images converted, {edited} main.caml edited')


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
