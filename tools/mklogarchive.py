#!/usr/bin/env python3
"""Turn a pulled iOS root's unified logs into a .logarchive that macOS reads.

  mklogarchive.py MOUNTED_ROOT OUT.logarchive
  /usr/bin/log show --archive OUT.logarchive --info --style compact > out.log

Copies private/var/db/diagnostics and private/var/db/uuidtext, then writes the
Info.plist `log collect` would (without it: "partial or missing metadata" and
no output). The boot UUIDs come from the timesync files. The format strings
in uuidtext are only there once the system has been up for a while (they were
empty in the 2026-10-12 pulls).
"""
import glob, os, plistlib, shutil, struct, sys, uuid


def boots(paths):
    """Boot UUIDs in timesync order (boot records: 0xbbb0, 48 bytes)."""
    out = []
    for p in sorted(paths):
        d, i = open(p, 'rb').read(), 0
        while i + 24 <= len(d):
            sig = struct.unpack_from('<H', d, i)[0]
            if sig == 0xbbb0:
                out.append(str(uuid.UUID(bytes=d[i + 8:i + 24])).upper())
                i += 48
            elif sig == 0x7354:  # 'Ts' sync record
                i += 32
            else:
                i += 1
    return out


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    root, out = sys.argv[1:]
    db = os.path.join(root, 'private/var/db')
    shutil.copytree(os.path.join(db, 'diagnostics'), out)
    shutil.copytree(os.path.join(db, 'uuidtext'), out, dirs_exist_ok=True)
    b = boots(glob.glob(os.path.join(out, 'timesync/*.timesync')))
    if not b:
        sys.exit('no boot records in timesync')
    old = {'OldestTimeRef': {'ContinuousTime': 0, 'UUID': b[0], 'WallTime': 0}}
    plistlib.dump({
        'OSArchiveVersion': 4,
        'ArchiveIdentifier': str(uuid.uuid4()).upper(),
        'SourceIdentifier': str(uuid.uuid4()).upper(),
        'EndTimeRef': {'ContinuousTime': 10 ** 16, 'UUID': b[-1], 'WallTime': 10 ** 18},
        **{k + 'Metadata': old for k in ('Persist', 'Special', 'Signpost', 'HighVolume', 'Live')},
    }, open(os.path.join(out, 'Info.plist'), 'wb'))
    print(f'{out}: {len(b)} boots, last {b[-1]}')


if __name__ == '__main__':
    main()
