#!/usr/bin/env python3
"""Adapt Xcode 26 generated IORPC access to the unchanged xnu-8796 ABI.

Only generated implementation text is adapted. The kernel's IORPC structure,
dispatch ABI and message layout stay identical to the working Xcode 16 build.

Xcode 26 iig adds a `kernelContent` member to the generated invocation union
and uses it instead of IORPCMessageFromMach(). xnu-8796 has no such member, so
each generated use is rewritten to the equivalent old-ABI lookup. The content
lives immediately after the Mach descriptors in the same message buffer.
"""
import os
from pathlib import Path
import re
import subprocess
import sys


def normalize(source):
    # Newer iig initializes its extra union member; old IORPC has no such field.
    source = re.sub(r",\s*\.kernelContent\s*=\s*&[A-Za-z_][A-Za-z0-9_]*\.msg\.content\.__hdr\b",
                    "", source)
    # InterruptOccurred records the outgoing content for the newer dispatcher.
    # Old dispatch derives it from the Mach message, so the assignment is unused.
    source = re.sub(r"^[ \t]*rpc\.kernelContent\s*=\s*&[A-Za-z_][A-Za-z0-9_]*->content\.__hdr;[ \t]*\r?\n",
                    "", source, flags=re.MULTILINE)
    # Dispatch tables inspect the incoming request content.
    source = re.sub(r"\bIORPCMessage \* msg = rpc\.kernelContent;",
                    "IORPCMessage * msg = IORPCMessageFromMach(rpc.message, false);",
                    source)
    # The kernel side of CheckForWork_Invoke inspects the incoming reply.
    source = re.sub(r"\breplyMessage = rpc\.rpc\.kernelContent;",
                    "replyMessage = IORPCMessageFromMach(rpc.rpc.reply, false);",
                    source)
    # CheckForWork's reply path validates the received reply content.
    source = re.sub(r"\bmessage = rpc\.kernelContent;",
                    "message = IORPCMessageFromMach(rpc.reply, false);",
                    source)

    # Invoke methods need the typed content header; old iig used the Msg's
    # trailing `content` member, which is the same address.
    def typed_content(match):
        decl, type_name = match.group(1), match.group(2)
        return (decl + "(const " + type_name + " *)"
                "IORPCMessageFromMach((IORPCMessageMach *)(uintptr_t)rpc.message, false);")

    source = re.sub(r"((?:const )?((?:struct )?[A-Za-z_][A-Za-z0-9_]*_Msg_Content) \* messageContent = )"
                    r"rpc\.kernelContent;", typed_content, source)

    if re.search(r"\.\s*kernelContent\b", source):
        raise ValueError("Unknown generated IORPC kernelContent use; refusing ABI guess")
    return source


def main(args):
    real = Path(os.environ["SWEET_REAL_IIG"]).resolve()
    if real == Path(__file__).resolve():
        raise ValueError("Recursive IIG wrapper")
    result = subprocess.run([str(real), *args])
    if result.returncode:
        return result.returncode
    if "--impl" in args:
        path = Path(args[args.index("--impl") + 1])
        original = path.read_text()
        adapted = normalize(original)
        if adapted != original:
            path.write_text(adapted)
            print("SWEET IIG: normalized generated IORPC access for xnu-8796", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
