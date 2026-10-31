#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Export an esitrace capture as a reference command sequence.

Writes two files next to OUT (OUT.txt and OUT.v39):

  OUT.txt  human-readable: one line per device transaction, parameter
           blocks in full, large payloads summarised, runs of 1B 03
           status polls collapsed.
  OUT.v39  the capture with every device->host payload longer than 64
           bytes (image data, reference passes) replaced by its length
           only (record type '#').  Host->device data, including the
           shading table Epson uploaded, is kept.  It contains no scanned
           content but still drives v39replay.py, which only needs the
           host->device bytes and the reply lengths.

usage: v39seq.py capture.bin capture.log OUT
"""
import re
import struct
import sys

from v39dump import NAMES, records, transactions

STRIP = 64


def strip_records(path):
    """Yield (dir, payload) with big device->host payloads reduced to '#'+len."""
    for d, p in records(path):
        if d == "<" and len(p) > STRIP:
            yield "#", struct.pack("<I", len(p))
        else:
            yield d, p


def escI_header(logpath):
    """The ESC/I FS W block the scan was requested with (from esitrace -l)."""
    for line in open(logpath, encoding="latin-1"):
        m = re.search(r"ESCI>\s+64:\s+([0-9a-f ]+)", line)
        if m:
            b = bytes.fromhex(m.group(1))
            xr, yr, x, y, w, h = struct.unpack_from("<6I", b)
            mode = {0x13: "color", 0x00: "gray"}.get(b[24], "0x%02x" % b[24])
            if b[25] == 1:
                mode = "lineart"
            return ("FS W: %d dpi %s, x=%d y=%d w=%d h=%d (pixels at %d dpi = "
                    "%.3f\" %.3f\" %.3f\" %.3f\")\nFS W block: %s"
                    % (xr, mode, x, y, w, h, xr, x / xr, y / xr, w / xr, h / xr, b.hex(" ")))
    return "FS W: unknown"


def describe(t):
    op = t[0][1]
    name = NAMES.get((op[0], op[1]), "?")
    parts = []
    first_out = True
    for d, p in t[1:]:
        if d == ">":
            if op == b"\x1e\x84" and not first_out:
                hdr = [q for dd, q in t[1:] if dd == ">"][0]
                kind = {3: "LUT", 6: "shading table (PROTOCOL.md 7.4)"}.get(hdr[0], "data")
                ident = kind == "LUT" and p == bytes(range(256))
                parts.append("> <%s%s, %d bytes>" % ("identity " if ident else "", kind, len(p)))
            elif len(p) > STRIP:
                parts.append("> <%d bytes>" % len(p))
            else:
                parts.append("> " + p.hex(" "))
            first_out = False
        else:
            if p == b"\x06":
                parts.append("<ack")
            elif len(p) > STRIP:
                parts.append("< [%d]" % len(p))
            else:
                parts.append("< " + p.hex(" "))
    return "%s %-11s %s" % (op.hex(" "), name, "  ".join(parts))


def main():
    cap, log, out = sys.argv[1:4]
    with open(out + ".v39", "wb") as f:
        for d, p in strip_records(cap):
            f.write(d.encode() + struct.pack("<I", len(p)) + p)
    lines = ["# reference sequence exported from %s" % cap, "# " + escI_header(log).replace("\n", "\n# "), ""]
    polls, last = 0, None
    scanblk = None

    def flush():
        nonlocal polls
        if polls:
            lines.append("1b 03 status      poll x%d (last reply %s)" % (polls, last))
            polls = 0

    for t in transactions(cap):
        op = t[0][1]
        if op == b"\x1b\x03":
            polls += 1
            last = t[1][1].hex(" ") if len(t) > 1 else "-"
            continue
        flush()
        if op == b"\x1e\x41":
            scanblk = [p for d, p in t[1:] if d == ">"][0]
        if op == b"\x1e\x47":
            sizes = [len(p) for d, p in t[1:] if d == "<"]
            n = struct.unpack("<I", scanblk[4:8])[0]
            total = sum(s - 8 for s in sizes)
            bits = 8 if scanblk[8] else 16
            lines.append("1e 47 read-image  %d lines, %d transfers (%s), %d B/line, %d-bit"
                         % (n, len(sizes), ", ".join("%dx%d" % (sizes.count(s), s) for s in sorted(set(sizes))),
                            total // n if n else 0, bits))
            continue
        lines.append(describe(t))
    flush()
    open(out + ".txt", "w", encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
