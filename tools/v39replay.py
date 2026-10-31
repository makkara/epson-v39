#!/usr/bin/env python3
"""Replay the host->device stream of an esitrace capture with v39.py.

Status polls (1B 03) are replayed; where the capture saw the device idle
but the replay finds it busy, the poll is repeated until it is idle.
Image reads (1E 47) are read transfer by transfer using the geometry of
the 1E 41 actually sent, so patched scan parameters work.

usage: v39replay.py capture.bin [options]
  --out FILE            save the data of the final (largest) image read
  --dump PREFIX         save every image read as PREFIX.<txn>.raw
  --patch-shading C     replace the shading table upload by constant C
  --patch I:K=V[,K=V]   patch byte K of the parameter block of transaction I
                        (repeatable; V in hex); use v39dump.py to find I
"""
import struct
import sys
import time

from v39 import V39, open_transport
from v39dump import transactions


def opt(name, default=None):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default


def shading_from_white(white16, n, formula):
    """Shading table T = scale * (mean_white - base - offset), raw pixel order.

    white16 = (data, lines) of the 16-bit white-reference pass; n = table
    entries (planes * W).  Dummy pixels (white near 0) get 0xFFFF.
    """
    import numpy as np
    scale, offset = formula[0], formula[1]
    base = formula[2] if len(formula) > 2 else 0.0   # scalar or per-plane list
    data, lines = white16
    w = np.frombuffer(data, "<u2").reshape(lines, n).astype(float).mean(0)
    if isinstance(base, list):
        base = np.repeat(base, n // len(base))
    t = np.clip(np.rint(scale * (w - base - offset)), 1, 0xFFFF)
    t[w < 5000] = 0xFFFF
    return t.astype("<u2").tobytes()


def main():
    cap = sys.argv[1]
    out, dump = opt("--out"), opt("--dump")
    shading = opt("--patch-shading")
    formula = opt("--shading-formula")   # "scale,offset[,base]" see shading_from_white
    if formula:
        formula = [[float(b) for b in x.split("/")] if "/" in x else float(x) for x in formula.split(",")]
    last16 = None
    patches = {}
    for k, a in enumerate(sys.argv):
        if a == "--patch":
            idx, kvs = sys.argv[k + 1].split(":")
            patches[int(idx)] = {int(x.split("=")[0]): int(x.split("=")[1], 16) for x in kvs.split(",")}
    ts = list(transactions(cap))
    sizes = [sum(len(p) for d, p in t[1:] if d == "<") for t in ts]
    final = max((i for i, t in enumerate(ts) if t[0][1] == b"\x1e\x47"), key=lambda i: sizes[i])
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
            if scanblk[8] == 0:      # 16-bit pass: remember it for --shading-formula
                last16 = (got, lines)
            if out and i == final:
                open(out, "wb").write(got)
            continue
        first = True
        for d, p in t[1:]:
            if d == ">":
                if shading is not None and op == b"\x1e\x84" and len(p) > 1000:
                    p = (int(shading, 0) & 0xFFFF).to_bytes(2, "little") * (len(p) // 2)
                if formula and op == b"\x1e\x84" and len(p) > 1000:
                    p = shading_from_white(last16, len(p) // 2, formula)
                    print("tx %d: shading table computed from fresh white pass" % i)
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
