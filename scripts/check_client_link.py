#!/usr/bin/env python3
"""Prove the netcode client can link on its own.

The root Makefile builds one flat list containing every net/ and server/ source,
so a symbol that only server code defines still resolves and the whole gate
passes. The real client does not link that list -- pc/CMakeLists.txt gives
`acnet_client` a strict subset, deliberately excluding the server-only
translation units -- so a call from c_api.cpp into, say, shop.cpp builds clean
here and fails at link time in build_pc.bat. That happened on 2026-08-08 with
turnip_sell_price, and nothing in `make check` could have caught it.

This links exactly the object files CMake's client target is made of, and
nothing else. Linking the objects directly rather than through an archive is
what makes it strict: an archive member is only pulled in if something already
references it, whereas a direct link must resolve every reference in every
object. An undefined symbol here is a symbol the shipped client would not find.

The source list is parsed out of pc/CMakeLists.txt rather than copied, so it
cannot drift from the target it is meant to be checking.
"""

import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CMAKE = os.path.join(ROOT, "pc", "CMakeLists.txt")


def client_sources():
    """The net/src/*.cpp files in CMake's acnet_client target."""
    with open(CMAKE, "r", encoding="utf-8") as handle:
        text = handle.read()
    match = re.search(r"add_library\(acnet_client\s+STATIC(.*?)\)", text, re.S)
    if match is None:
        sys.exit("could not find the acnet_client target in pc/CMakeLists.txt")
    sources = re.findall(r"\$\{DECOMP_ROOT\}/(\S+\.cpp)", match.group(1))
    if not sources:
        sys.exit("acnet_client lists no sources -- the parser is probably stale")
    return sources


def main():
    build_dir = sys.argv[1] if len(sys.argv) > 1 else "build/netcode"
    cxx = os.environ.get("CXX", "g++")

    objects = []
    missing_objects = []
    for source in client_sources():
        obj = os.path.join(ROOT, build_dir, source[:-4] + ".o")
        if os.path.exists(obj):
            objects.append(obj)
        else:
            missing_objects.append(source)

    if missing_objects:
        # A source the client needs that the Makefile never compiles: `make
        # check` is not merely failing to link it, it is not building it at all.
        sys.stderr.write(
            "acnet_client sources are not built by the Makefile, so nothing in "
            "`make check` compiles them:\n"
        )
        for source in missing_objects:
            sys.stderr.write("  %s\n" % source)
        sys.stderr.write("Add them to NET_SOURCES in the root Makefile.\n")
        return 1

    with tempfile.TemporaryDirectory() as tmp:
        stub = os.path.join(tmp, "client_link_stub.cpp")
        with open(stub, "w", encoding="utf-8") as handle:
            # The objects are linked directly, so every reference in them has to
            # resolve whether or not this stub mentions it. main() exists only
            # to satisfy the linker's entry point.
            handle.write('#include "acnet/c_api.h"\n')
            handle.write("int main() { return acnet_client_poll(); }\n")

        binary = os.path.join(tmp, "client_link_check")
        command = (
            [cxx, "-std=c++17", "-I" + os.path.join(ROOT, "net", "include"), stub]
            + objects
            + ["-o", binary, "-ldl"]
        )
        result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
        if result.returncode != 0:
            sys.stderr.write(
                "the netcode client does not link on its own -- build_pc.bat "
                "would fail:\n"
            )
            sys.stderr.write(result.stderr)
            sys.stderr.write(
                "\nEither move the definition into a translation unit "
                "acnet_client already links, or add its source to the "
                "acnet_client target in pc/CMakeLists.txt.\n"
            )
            return 1

    print('{"client_link":"pass","objects":%d}' % len(objects))
    return 0


if __name__ == "__main__":
    sys.exit(main())
