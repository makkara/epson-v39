#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Pretty-print a raw USB capture made with `esitrace -r file.bin`.

Each record in the capture is: direction byte ('>' host->device, '<'
device->host), little-endian u32 length, payload.  Records are grouped into
command transactions: a new transaction starts with every 2-byte host write
whose first byte is ESC (0x1b) or 0x1e.

usage: v39dump.py capture.bin [--all] [--summary]
  --all      also show the 1b 03 status polls (hidden by default)
  --summary  only print a histogram of commands and parameter/reply sizes
"""
import struct
import sys
from collections import Counter, OrderedDict

NAMES = {
    (0x1b, 0x03): "status",
    (0x1b, 0x06): "fw-download",
    (0x1b, 0x13): "identify",
    (0x1b, 0x16): "fw-mode",
    (0x1b, 0x25): "read-reg32",
    (0x1b, 0x40): "reset",
    (0x1e, 0x01): "move",
    (0x1e, 0x05): "home",
    (0x1e, 0x08): "query",
    (0x1e, 0x11): "lamp",
    (0x1e, 0x18): "cancel",
    (0x1e, 0x21): "set-21",
    (0x1e, 0x22): "set-led",
    (0x1e, 0x31): "set-afe",
    (0x1e, 0x41): "set-scan",
    (0x1e, 0x42): "set-42",
    (0x1e, 0x43): "set-43",
    (0x1e, 0x47): "read-image",
    (0x1e, 0x53): "get-params",
    (0x1e, 0x57): "set-params",
    (0x1e, 0x5a): "set-5a",
    (0x1e, 0x65): "get-65",
    (0x1e, 0x83): "mem-read",
    (0x1e, 0x84): "mem-write",
    (0x1e, 0x85): "buttons",
    (0x1e, 0x87): "get-87",
    (0x1e, 0x92): "get-afe",
    (0x1e, 0x9f): "get-9f",
    (0x1e, 0xa2): "set-a2",
    (0x1e, 0xe4): "reg-e4",
}


def records(path):
    data = open(path, "rb").read()
    pos = 0
    while pos + 5 <= len(data):
        d = chr(data[pos])
        (n,) = struct.unpack_from("<I", data, pos + 1)
        p = data[pos + 5 : pos + 5 + n]
        pos += 5 + n
        if d == "#":    # stripped capture (v39seq.py): device payload, length only
            yield "<", bytes(struct.unpack("<I", p)[0])
        else:
            yield d, p


def transactions(path):
    cur = None
    for d, p in records(path):
        if d == ">" and len(p) == 2 and p[0] in (0x1B, 0x1E):
            if cur:
                yield cur
            cur = [(d, p)]
        elif cur is not None:
            cur.append((d, p))
        else:
            cur = [(d, p)]
    if cur:
        yield cur


def hexs(b, maxlen=48):
    s = " ".join("%02x" % x for x in b[:maxlen])
    if len(b) > maxlen:
        s += " ... (%d bytes)" % len(b)
    return s


def main():
    path = sys.argv[1]
    show_all = "--all" in sys.argv
    summary = "--summary" in sys.argv
    hist = Counter()
    shapes = OrderedDict()
    for t in transactions(path):
        cmd = t[0][1]
        key = (cmd[0], cmd[1]) if len(cmd) == 2 else None
        name = NAMES.get(key, "?")
        shape = " ".join("%s%d" % (d, len(p)) for d, p in t[1:])
        if key:
            hist[(key, name)] += 1
            shapes.setdefault((key, name), Counter())[shape] += 1
        if summary or (key == (0x1B, 0x03) and not show_all):
            continue
        print("%-11s %s" % (name, hexs(cmd)))
        for d, p in t[1:]:
            print("    %s %s" % (d, hexs(p)))
    if summary:
        for (key, name), n in sorted(hist.items()):
            print("%02x %02x %-11s x%-4d %s" % (key[0], key[1], name, n,
                  "; ".join("%s (x%d)" % (s or "-", c)
                            for s, c in shapes[(key, name)].most_common(4))))


if __name__ == "__main__":
    main()
