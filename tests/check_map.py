#!/usr/bin/env python3
"""Check the Arduino GNU ld map. Usage: check_map.py <package-manifest.json> <map>

[--allow-group <group>]... Fail with status 1 if an unapproved manifest link group is
included, _malloc_r does not come from the SDK TLSF libc port in libforce_base.a, or
main does not come from Core main.cpp. Start parsing at the first LOAD line rather than
depending on linker output language.
"""

import fnmatch
import json
import os
import re
import sys

IGNORED_SECTIONS = (".debug", ".comment", ".riscv.attributes", ".note", ".gnu.attributes")
INPUT_LINE = re.compile(r"^ (\S+)?\s+0x[0-9a-f]+\s+0x([0-9a-f]+)\s+(\S+)$")
NAME_LINE = re.compile(r"^ (\S+)$")


def input_sections(text):
    """Return (section, size, input-file) tuples for nonzero, non-debug input sections in

    the memory map.
    """
    start = text.find("\nLOAD ")
    if start < 0:
        raise ValueError("not a GNU ld map file: no LOAD line")
    out = []
    section = None
    for line in text[start:].splitlines():
        m = NAME_LINE.match(line)
        if m:
            section = m.group(1)  # Long section names put address and size on the next line.
            continue
        m = INPUT_LINE.match(line)
        name, section = (m.group(1) or section, None) if m else (None, None)
        if not m or name is None or name.startswith(IGNORED_SECTIONS) or int(m.group(2), 16) == 0:
            continue
        out.append((name, int(m.group(2), 16), m.group(3)))
    return out


def archive_member(path):
    """Split '/x/libfoo.a(bar.o)' into ('libfoo.a', 'bar.o'); restore original archive

    names for libforce_* members.
    """
    m = re.match(r"(.*\.a)\(([^)]+)\)$", path)
    if not m:
        return None
    archive, member = os.path.basename(m.group(1)), m.group(2)
    if archive.startswith("libforce_") and "@" in member:
        archive, member = member.split("@", 1)
    return archive, member


def main(argv):
    """Parse options and run the check map validation or packaging command."""
    allowed = []
    args = []
    i = 0
    while i < len(argv):
        if argv[i] == "--allow-group" and i + 1 < len(argv):
            allowed.append(argv[i + 1])
            i += 2
        else:
            args.append(argv[i])
            i += 1
    if len(args) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    with open(args[0]) as f:
        groups = json.load(f)["link"]["groups"]
    with open(args[1], errors="replace") as f:
        sections = input_sections(f.read())

    ok = True
    for group, spec in groups.items():
        if group in allowed:
            continue
        bad = sorted({am for _, _, src in sections if (am := archive_member(src))
                      and any(fnmatch.fnmatch(am[0], p) for p in spec["archives"])})
        print("%s 链接组未链入: %s" % (group, "PASS" if not bad else "FAIL"))
        for archive, member in bad:
            print("  %s(%s)" % (archive, member))
        ok = ok and not bad

    def origin(section_name):
        """Classify a map section's owning component for duplicate-owner verification."""
        return [src for name, _, src in sections if name == section_name]

    malloc_src = origin(".text._malloc_r")
    good = len(malloc_src) == 1 and archive_member(malloc_src[0]) == ("liblibc.a", "port_memory.c.obj")
    print("_malloc_r 来自 SDK libc 移植层: %s %s" % ("PASS" if good else "FAIL", malloc_src))
    ok = ok and good

    main_src = origin(".text.startup.main") + origin(".text.main")
    good = len(main_src) == 1 and main_src[0].endswith("(main.cpp.o)")
    print("main 来自 Core main.cpp: %s %s" % ("PASS" if good else "FAIL", main_src))
    return 0 if ok and good else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
