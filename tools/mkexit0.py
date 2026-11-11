#!/usr/bin/env python3
"""Write the smallest arm64 iOS executable that exits 0.

    tools/mkexit0.py OUT

A static Mach-O (no dyld): __PAGEZERO, __TEXT with four instructions
(exit(0) through svc 0x80, then a branch to self), LC_UNIXTHREAD for the entry
point, LC_BUILD_VERSION for iOS. Unsigned: this port runs with AMFI's checks
off (cs_enforcement_disable=1). Used as the /usr/sbin/BlueTool stand-in:
bluetoothd runs BlueTool scripts to power the Broadcom chip of an iPhone up,
which fails on the WCN3990 ("Init failed, still in high power"); with BlueTool
exiting 0 at once, bluetoothd opens the H4 transport itself (ios-sweet did the
same, bt-install-0147). The stock BlueTool is kept in
research/fw-dump/bluetool-orig/ (from ios16-root.img) to put back with
--blob FILE:/usr/sbin/BlueTool=... if needed.
"""
import struct
import sys

MH_MAGIC_64 = 0xfeedfacf
CPU_TYPE_ARM64 = 0x0100000c
CPU_SUBTYPE_ARM64_ALL = 0
MH_EXECUTE = 2
MH_NOUNDEFS = 0x1
MH_PIE = 0x200000
LC_SEGMENT_64 = 0x19
LC_UNIXTHREAD = 0x5
LC_BUILD_VERSION = 0x32
VM_PROT_READ, VM_PROT_WRITE, VM_PROT_EXECUTE = 1, 2, 4
ARM_THREAD_STATE64 = 6
ARM_THREAD_STATE64_COUNT = 68
PLATFORM_IOS = 2

TEXT_VMADDR = 0x100000000
FILE_SIZE = 0x4000
CODE_OFF = 0x1000

CODE = struct.pack('<4I',
                   0xd2800000,   # mov x0, #0
                   0xd2800030,   # mov x16, #1 (SYS_exit)
                   0xd4001001,   # svc #0x80
                   0x14000000)   # b .


def segment(name, vmaddr, vmsize, fileoff, filesize, maxprot, initprot, sections=()):
    body = struct.pack('<16sQQQQiiII', name.encode(), vmaddr, vmsize, fileoff, filesize,
                       maxprot, initprot, len(sections), 0)
    for s in sections:
        body += s
    return struct.pack('<II', LC_SEGMENT_64, 8 + len(body)) + body


def section(sectname, segname, addr, size, offset):
    return struct.pack('<16s16sQQIIIIIIII', sectname.encode(), segname.encode(), addr, size, offset,
                       2, 0, 0, 0x80000400, 0, 0, 0)   # align 4, S_REGULAR|SOME_INSTRUCTIONS|PURE


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    text_sect = section('__text', '__TEXT', TEXT_VMADDR + CODE_OFF, len(CODE), CODE_OFF)
    cmds = segment('__PAGEZERO', 0, TEXT_VMADDR, 0, 0, 0, 0)
    cmds += segment('__TEXT', TEXT_VMADDR, FILE_SIZE, 0, FILE_SIZE,
                    VM_PROT_READ | VM_PROT_EXECUTE, VM_PROT_READ | VM_PROT_EXECUTE, [text_sect])
    state = [0] * 32 + [TEXT_VMADDR + CODE_OFF] + [0]        # x0..x28, fp, lr, sp, pc, cpsr+pad
    thread = struct.pack('<II', ARM_THREAD_STATE64, ARM_THREAD_STATE64_COUNT) + struct.pack('<34Q', *state)
    cmds += struct.pack('<II', LC_UNIXTHREAD, 8 + len(thread)) + thread
    cmds += struct.pack('<IIIIII', LC_BUILD_VERSION, 24, PLATFORM_IOS, 0x100000, 0x100000, 0)
    header = struct.pack('<IiiIIIII', MH_MAGIC_64, CPU_TYPE_ARM64, CPU_SUBTYPE_ARM64_ALL, MH_EXECUTE,
                         4, len(cmds), MH_NOUNDEFS, 0)
    image = bytearray(FILE_SIZE)
    image[:len(header) + len(cmds)] = header + cmds
    image[CODE_OFF:CODE_OFF + len(CODE)] = CODE
    with open(sys.argv[1], 'wb') as f:
        f.write(image)
    print('%s: %u bytes, entry %#x, %u load commands' % (sys.argv[1], len(image), TEXT_VMADDR + CODE_OFF, 4))


if __name__ == '__main__':
    main()
