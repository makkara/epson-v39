#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Replay the host->device stream of an esitrace capture with v39.py.

Status polls (1B 03) are replayed; where the capture saw the device idle
but the replay finds it busy, the poll is repeated until it is idle.
Image reads (1E 47) are read transfer by transfer using the geometry of
the 1E 41 actually sent, so patched scan parameters work.

usage: v39replay.py capture.bin [options]
  --out FILE            save the data of the last image read (the scan itself)
  --dump PREFIX         save every image read as PREFIX.<txn>.raw
  --patch-shading C     replace the shading table upload by constant C
  --shading-exact       recompute shading table, 1E 42 and 1E 41 bytes 11/15
                        from the replay's own dark/white passes (needs numpy)
  --patch I:K=V[,K=V]   patch byte K of the parameter block of transaction I
                        (repeatable; V in hex); use v39dump.py to find I
"""
import struct
import sys
import time

from v39 import V39, open_transport, shading_table
from v39dump import transactions


def opt(name, default=None):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default


def passes16(refs, W):
    """(dark, white) arrays from the remembered 16-bit reference passes."""
    import numpy as np
    arrs = [np.frombuffer(d, "<u2").reshape(n, -1, W) for d, n in refs]
    return np.concatenate(arrs[:-1]), arrs[-1]


def main():
    cap = sys.argv[1]
    out, dump = opt("--out"), opt("--dump")
    shading = opt("--patch-shading")
    exact = "--shading-exact" in sys.argv   # recompute table, 1E 42, 1E 41 b11/b15
    refs, calc = [], None
    patches = {}
    for k, a in enumerate(sys.argv):
        if a == "--patch":
            idx, kvs = sys.argv[k + 1].split(":")
            patches[int(idx)] = {int(x.split("=")[0]): int(x.split("=")[1], 16) for x in kvs.split(",")}
    ts = list(transactions(cap))
    final = max(i for i, t in enumerate(ts) if t[0][1] == b"\x1e\x47")   # last image read
    dev = V39(open_transport())
    if dev.status() & 0x40:
        dev.wait_idle()
    t0 = time.time()
    scanblk = None
    for i, t in enumerate(ts):
        op = t[0][1]
        if op == b"\x1b\x03":
            # keep the interpreter's polls; if the device is busy where the
            # capture showed it idle, keep polling until it catches up
            want = t[1][1][0] if len(t) > 1 else 0x18
            s = dev.status()
            while (s & 0x40) and not (want & 0x40):
                time.sleep(0.02)
                s = dev.status()
            continue
        dev.t.write(op)
        if op == b"\x1e\x47":
            lines, per = struct.unpack("<I", scanblk[4:8])[0], scanblk[18]
            got, seen = b"", set()
            for _ in range(-(-lines // per)):
                r = dev.t.read_transfer()
                seen.add(len(r))
                got += r[:-8]
            if i in patches or any(j < i for j in patches):
                print("tx %d read-image: %d lines, transfer sizes %s" % (i, lines, sorted(seen)))
            if dump:
                open("%s.%d.raw" % (dump, i), "wb").write(got)
            if scanblk[8] == 0 and lines >= 16:   # dark/white reference pass
                refs.append((got, lines))
            if out and i == final:
                open(out, "wb").write(got)
            continue
        first = True
        for d, p in t[1:]:
            if d == ">":
                if shading is not None and op == b"\x1e\x84" and len(p) > 1000:
                    p = (int(shading, 0) & 0xFFFF).to_bytes(2, "little") * (len(p) // 2)
                if exact and op == b"\x1e\x84" and len(p) > 1000:
                    W = struct.unpack("<H", scanblk[2:4])[0] - 351
                    calc = shading_table(*passes16(refs, W))
                    print("tx %d: table from fresh passes; 1E 42 %s, b11 sel %d, b15 %02x%s"
                          % (i, calc[1], calc[2], calc[3],
                             " (same bytes as capture)" if calc[0] == p else ""))
                    p = calc[0]
                if exact and calc and first and op == b"\x1e\x42" and any(p):
                    p = struct.pack("<9H", *calc[1])
                if exact and calc and first and op == b"\x1e\x41" and p[8] == 1:
                    p = bytearray(p)
                    p[11] = (p[11] & ~0x03) | calc[2]
                    p[15] = calc[3]
                    p = bytes(p)
                if first and i in patches:
                    p = bytearray(p)
                    for k, v in patches[i].items():
                        p[k] = v
                    p = bytes(p)
                    print("tx %d %s patched: %s" % (i, op.hex(), p.hex(" ")))
                if first and op == b"\x1e\x41":
                    scanblk = p
                first = False
                dev.t.write(p)
            else:
                try:
                    r = dev.t.read(len(p))
                except OSError as e:
                    sys.exit("tx %d %s: read of %d failed: %s" % (i, op.hex(), len(p), e))
                if p == b"\x06" and r != p:
                    sys.exit("tx %d %s: expected ACK, got %s" % (i, op.hex(), r.hex()))
        if op == b"\x1e\x05":
            dev.wait_idle()
    print("replayed %d transactions in %.1f s" % (len(ts), time.time() - t0))
    dev.t.close()


if __name__ == "__main__":
    main()
