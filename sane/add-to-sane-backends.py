#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Add the epsonv39 backend to a sane-backends source tree.

usage: add-to-sane-backends.py /path/to/sane-backends

Copies the backend, its config template, man page and description file
into the tree and registers them in the build system (configure.ac,
backend/Makefile.am, backend/dll.conf.in, doc/Makefile.am, doc/sane.man,
po/POTFILES.in). Each edit is anchored next to the epsonds backend. The
script is idempotent and stops if an anchor is missing, so it never edits
a tree half-way. Afterwards run ./autogen.sh and configure as usual.
"""
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

FILES = {
    "epsonv39.c": "backend/epsonv39.c",
    "epsonv39.conf.in": "backend/epsonv39.conf.in",
    "sane-epsonv39.man": "doc/sane-epsonv39.man",
    "epsonv39.desc": "doc/descriptions/epsonv39.desc",
}

MAKEFILE_STANZA = """
libepsonv39_la_SOURCES = epsonv39.c
libepsonv39_la_CPPFLAGS = $(AM_CPPFLAGS) -DBACKEND_NAME=epsonv39

nodist_libsane_epsonv39_la_SOURCES = epsonv39-s.c
libsane_epsonv39_la_CPPFLAGS = $(AM_CPPFLAGS) -DBACKEND_NAME=epsonv39
libsane_epsonv39_la_LDFLAGS = $(DIST_SANELIBS_LDFLAGS)
libsane_epsonv39_la_LIBADD = $(COMMON_LIBS) \\
    libepsonv39.la \\
    ../sanei/sanei_init_debug.lo \\
    ../sanei/sanei_constrain_value.lo \\
    ../sanei/sanei_config.lo \\
    sane_strstatus.lo \\
    ../sanei/sanei_usb.lo \\
    ../sanei/sanei_thread.lo \\
    $(MATH_LIB) $(USB_LIBS) $(SANEI_THREAD_LIBS) $(RESMGR_LIBS)
EXTRA_DIST += epsonv39.conf.in
"""

SANE_MAN_ENTRY = """.TP
.B epsonv39
Supports the host-based Epson Perfection V39 (GT-S650) USB flatbed
scanner. See
.BR sane\\-epsonv39 (5)
for details.
"""

# (file, anchor, text to insert after the anchor, marker meaning "already done")
EDITS = [
    ("configure.ac", " epsonds ", "epsonv39 ", " epsonv39 "),
    ("backend/Makefile.am", "FIRMWARE_DIRS = artec_eplus48u gt68xx snapscan epjitsu",
     " epsonv39", "epjitsu epsonv39"),
    ("backend/Makefile.am", "epsonds.conf ", "epsonv39.conf ", "epsonv39.conf "),
    ("backend/Makefile.am", "libepsonds.la ", "libepsonv39.la ", "libepsonv39.la "),
    ("backend/Makefile.am", "libsane-epsonds.la ", "libsane-epsonv39.la ", "libsane-epsonv39.la "),
    ("backend/Makefile.am", "EXTRA_DIST += epsonds.conf.in\n", MAKEFILE_STANZA,
     "libepsonv39_la_SOURCES"),
    ("backend/dll.conf.in", "\nepsonds\n", "epsonv39\n", "\nepsonv39\n"),
    ("doc/Makefile.am", "sane-epsonds.5 ", "sane-epsonv39.5 ", "sane-epsonv39.5"),
    ("doc/Makefile.am", "sane-epsonds.man ", "sane-epsonv39.man ", "sane-epsonv39.man"),
    ("doc/Makefile.am", "descriptions/epsonds.desc ", "descriptions/epsonv39.desc ",
     "descriptions/epsonv39.desc"),
    ("po/POTFILES.in", "backend/epsonds.h\n", "backend/epsonv39.c\n", "backend/epsonv39.c"),
]


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    tree = sys.argv[1]
    if not os.path.isfile(os.path.join(tree, "backend", "epsonds.c")):
        sys.exit("%s does not look like a sane-backends tree" % tree)

    # read everything first and check every anchor before writing anything
    contents = {}
    plan = []
    for rel, anchor, text, marker in EDITS:
        path = os.path.join(tree, rel)
        if path not in contents:
            contents[path] = open(path, encoding="utf-8").read()
        s = contents[path]
        if marker in s:
            continue
        if anchor not in s:
            sys.exit("anchor %r not found in %s; the tree has changed, edit it by hand" % (anchor, rel))
        plan.append((path, anchor, text))
    man = os.path.join(tree, "doc", "sane.man")
    contents[man] = open(man, encoding="utf-8").read()
    man_anchor = ".TP\n.B escl\n"
    if ".B epsonv39\n" not in contents[man]:
        if man_anchor not in contents[man]:
            sys.exit("anchor for the escl entry not found in doc/sane.man")

    for path, anchor, text in plan:
        s = contents[path]
        i = s.index(anchor) + len(anchor)
        contents[path] = s[:i] + text + s[i:]
    if ".B epsonv39\n" not in contents[man]:
        contents[man] = contents[man].replace(man_anchor, SANE_MAN_ENTRY + man_anchor, 1)

    for src, dst in FILES.items():
        shutil.copyfile(os.path.join(HERE, src), os.path.join(tree, dst))
        print("copied  %s" % dst)
    for path, s in contents.items():
        with open(path, "w", encoding="utf-8") as f:
            f.write(s)
    for rel in sorted({e[0] for e in EDITS} | {"doc/sane.man"}):
        print("updated %s" % rel)
    print("\nNow run ./autogen.sh && ./configure && make in %s" % tree)


if __name__ == "__main__":
    main()
