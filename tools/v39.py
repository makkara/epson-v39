#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prototype driver for the EPSON Perfection V39 / GT-S650 (04b8:013d).

Speaks the device protocol documented in PROTOCOL.md directly - no Epson
code involved.  Transport is pluggable: usbscan.sys on Windows (ctypes),
pyusb/libusb elsewhere.
"""
import struct
import sys
import time

ACK, NAK = 0x06, 0x15


# --------------------------------------------------------------------------
# transports

class UsbscanTransport:
    """Windows: \\\\.\\UsbscanN via usbscan.sys (bulk pipes = ReadFile/WriteFile)."""

    def __init__(self, path=r"\\.\Usbscan0", mutex=r"Global\Es_Mutex_0002", timeout=30):
        import ctypes
        from ctypes import wintypes
        self.ct, self.k32 = ctypes, ctypes.WinDLL("kernel32", use_last_error=True)
        k = self.k32
        k.CreateFileW.restype = wintypes.HANDLE
        k.OpenMutexW.restype = wintypes.HANDLE
        # hold the WIA driver's mutex so its 1 Hz status/button poll stays away
        self.mutex = k.OpenMutexW(0x00100000, False, mutex)
        if self.mutex:
            k.WaitForSingleObject(wintypes.HANDLE(self.mutex), 10000)
        h = k.CreateFileW(path, 0xC0000000, 0, None, 3, 0, None)
        if h in (None, wintypes.HANDLE(-1).value):
            raise OSError(ctypes.get_last_error(), "cannot open " + path)
        self.h = wintypes.HANDLE(h)
        to = (ctypes.c_ulong * 3)(timeout, timeout, timeout)
        ret = wintypes.DWORD()
        k.DeviceIoControl(self.h, 0x8000202C, to, 12, None, 0, ctypes.byref(ret), None)  # IOCTL_SET_TIMEOUT

    def write(self, data):
        n = self.ct.c_ulong()
        if not self.k32.WriteFile(self.h, bytes(data), len(data), self.ct.byref(n), None):
            raise OSError(self.ct.get_last_error(), "write")
        if n.value != len(data):
            raise IOError("short write %d/%d" % (n.value, len(data)))

    def read(self, size):
        buf = self.ct.create_string_buffer(size)
        out = b""
        while len(out) < size:
            n = self.ct.c_ulong()
            if not self.k32.ReadFile(self.h, buf, size - len(out), self.ct.byref(n), None):
                raise OSError(self.ct.get_last_error(), "read")
            if n.value == 0:
                break
            out += buf.raw[: n.value]
        return out

    def read_transfer(self, maxsize=1 << 20):
        """One bulk IN transfer (ends at the device's short packet)."""
        buf = self.ct.create_string_buffer(maxsize)
        n = self.ct.c_ulong()
        if not self.k32.ReadFile(self.h, buf, maxsize, self.ct.byref(n), None):
            raise OSError(self.ct.get_last_error(), "read")
        return buf.raw[: n.value]

    def close(self):
        self.k32.CloseHandle(self.h)
        if self.mutex:
            self.k32.ReleaseMutex(self.ct.c_void_p(self.mutex))


class PyusbTransport:
    """Linux/macOS: libusb via pyusb."""

    def __init__(self, timeout=30):
        import usb.core
        self.dev = usb.core.find(idVendor=0x04B8, idProduct=0x013D)
        if self.dev is None:
            raise OSError("V39 not found")
        try:
            if self.dev.is_kernel_driver_active(0):
                self.dev.detach_kernel_driver(0)
        except (NotImplementedError, usb.core.USBError):
            pass
        self.dev.set_configuration()
        self.timeout = timeout * 1000

    def write(self, data):
        self.dev.write(0x02, bytes(data), self.timeout)

    def read(self, size):
        out = b""
        while len(out) < size:
            chunk = bytes(self.dev.read(0x81, size - len(out), self.timeout))
            if not chunk:
                break
            out += chunk
        return out

    def read_transfer(self, maxsize=1 << 20):
        """One bulk IN transfer (ends at the device's short packet)."""
        return bytes(self.dev.read(0x81, maxsize, self.timeout))

    def close(self):
        import usb.util
        usb.util.dispose_resources(self.dev)


def open_transport():
    if sys.platform == "win32":
        return UsbscanTransport()
    return PyusbTransport()


# --------------------------------------------------------------------------
# device protocol

class ProtocolError(Exception):
    pass


class V39:
    def __init__(self, transport, log=None):
        self.t = transport
        self.log = log

    def _w(self, data):
        if self.log:
            self.log(">", data)
        self.t.write(data)

    def _r(self, n):
        d = self.t.read(n)
        if self.log:
            self.log("<", d)
        return d

    def _ack(self):
        a = self._r(1)
        if a != bytes([ACK]):
            raise ProtocolError("expected ACK, got %s" % a.hex())

    # command shapes (PROTOCOL.md section 3)
    def query(self, op, n):
        self._w(op)
        return self._r(n)

    def action(self, op):
        self._w(op)
        self._ack()

    def set(self, op, param):
        self._w(op)
        self._ack()
        self._w(param)
        self._ack()

    def setget(self, op, param, n):
        self._w(op)
        self._ack()
        self._w(param)
        return self._r(n)

    # basic commands
    def status(self):
        return self.query(b"\x1b\x03", 2)[0]

    def identify(self):
        return self.query(b"\x1b\x13", 28).decode("ascii", "replace")

    def reset(self):
        self.action(b"\x1b\x40")

    def buttons(self):
        return self.query(b"\x1e\x85", 1)[0]

    def wait_idle(self, timeout=20.0):
        t0 = time.time()
        while time.time() - t0 < timeout:
            s = self.status()
            if not s & 0x40:
                return s
            time.sleep(0.05)
        raise ProtocolError("timeout waiting for idle")

    def mem_read(self, typ, addr, length):
        return self.setget(b"\x1e\x83", struct.pack("<BBII", typ, 0, addr, length), length)

    def mem_write(self, typ, addr, data):
        self._w(b"\x1e\x84")
        self._ack()
        self._w(struct.pack("<BBII", typ, 0, addr, len(data)))
        self._w(data)
        self._ack()

    def read_reg32(self, addr):
        return struct.unpack("<I", self.setget(b"\x1b\x25", struct.pack("<I", addr), 4))[0]

    def home(self):
        self.action(b"\x1e\x05")
        return self.wait_idle()

    def move(self, mode, dist):
        self.set(b"\x1e\x01", struct.pack("<HI", mode, dist))

    def lamp(self, v=4):
        self.set(b"\x1e\x11", bytes([v]))

    def set_a2(self, v):
        self.set(b"\x1e\xa2", bytes([v]))

    def set_5a(self, v):
        self.set(b"\x1e\x5a", bytes([v]))

    def set_21(self, v):
        self.set(b"\x1e\x21", struct.pack("<I", v))

    def set_led(self, blk):
        assert len(blk) == 40
        self.set(b"\x1e\x22", blk)

    def set_afe(self, offsets, gains):
        self.set(b"\x1e\x31", struct.pack("<9H", *offsets, *gains))

    def get_afe(self):
        return struct.unpack("<9H", self.query(b"\x1e\x92", 18))

    def set_42(self, vals):
        self.set(b"\x1e\x42", struct.pack("<9H", *vals))

    def set_43(self, vals):
        self.set(b"\x1e\x43", struct.pack("<6H", *vals))

    def set_scan(self, blk):
        assert len(blk) == 22
        self.set(b"\x1e\x41", blk)

    def cancel(self):
        self._w(b"\x1e\x18")

    def read_image(self, lines, line_bytes, per_transfer):
        """Issue 1E 47 and collect all transfers; returns (data, trailers)."""
        self._w(b"\x1e\x47")
        data, trailers = [], []
        left = lines
        while left > 0:
            n = min(per_transfer, left)
            chunk = self._r(n * line_bytes + 8)
            if len(chunk) != n * line_bytes + 8:
                raise ProtocolError("short image transfer %d" % len(chunk))
            data.append(chunk[:-8])
            trailers.append(chunk[-8:])
            left -= n
        return b"".join(data), trailers

    # firmware (PROTOCOL.md 4.2)
    def upload_firmware(self, path):
        fw = open(path, "rb").read()
        if fw[:4] != b"EDLA" or len(fw) < 0x100 + 0x10000:
            raise ValueError("not an EDLA firmware file")
        image = fw[0x100:0x10100]
        self.action(b"\x1b\x06")
        self._w(fw[0x4C:0x50])
        self._w(image[:65520])
        self._w(image[65520:])
        self._w(bytes([sum(image) & 0xFF]))
        self._ack()
        self.action(b"\x1b\x16")
        self._w(b"\x80")
        self._ack()


# --------------------------------------------------------------------------
# calibration maths (bit-exact with Epson's esxi010c.dll, PROTOCOL.md 7.4)

def shading_table(dark, white):
    """Build the 1E 84 type-06 shading table from the 16-bit reference passes.

    dark, white: numpy uint16 arrays (lines, planes, W) in raw (interleaved)
    pixel order.  Returns (table bytes, 1E 42 words[9], 1E 41 byte-11 low
    bits, 1E 41 byte 15).
    """
    import numpy as np
    _, planes, W = dark.shape
    # A: per-pixel dark, averaged over all lines and planes (integer)
    A = dark.astype(np.int64).sum((0, 1)) // (dark.shape[0] * planes)
    blk = [int(A[0::2].min()), int(A[1::2].min())]          # per sensor half
    rng = max(int(A[0::2].max()) - blk[0], int(A[1::2].max()) - blk[1])
    abits_sel, sh_a = 0, 0                                  # A field: 8 - abits_sel bits
    for lim, sel in ((0x20, 3), (0x40, 2), (0x80, 1), (0x100, 0)):
        if rng < lim:
            abits_sel = sel
            break
    else:
        sh_a = next((k for k in range(1, 8) if rng < (0x100 << k)), 8)
    A = A - np.where(np.arange(W) % 2 == 0, blk[0], blk[1])
    # B: white in 4 equal groups of lines -> integer mean per group -> max
    # over groups, minus A; u16 arithmetic (dummy pixels wrap around)
    n = white.shape[0] // 4
    grp = [white[g * n:(g + 1) * n].astype(np.int64).sum(0) // n for g in range(4)]
    B = (np.maximum.reduce(grp) - A[None, :]) & 0xFFFF
    base = B.min(1)                                         # per colour plane
    rng_b = int((B.max(1) - base).max())
    sh_b = 0
    while rng_b >= (1 << (8 + abits_sel + sh_b)):
        sh_b += 1
    T = (((B - base[:, None]) >> sh_b) << (8 - abits_sel)) | (A >> sh_a)[None, :]
    bases = [int(b) for b in base] if planes == 3 else [int(base[0])] * 3
    words = bases + blk * 3
    return T.astype("<u2").tobytes(), words, abits_sel, (sh_b << 4) | sh_a


def box8_per_half(line):
    """8-pixel box averages within each sensor half (raw order: stride 2)."""
    import numpy as np
    return [np.convolve(line[h::2].astype(np.int64), np.ones(8, np.int64), "valid") // 8
            for h in (0, 1)]


AFE_WINDOW = (0x380, 0x480)          # relaxed variant: (0x300, 0x500)


def afe_measure(dark4):
    """Per-half AFE measure: min of 8-px box averages of the line mean."""
    m = dark4.astype("int64").sum(0) // dark4.shape[0]
    return [int(b.min()) for b in box8_per_half(m)]


def afe_next(offset, bit, measured):
    """SAR update for one sensor half; start with offset=0x80, bit=0x40.

    in window -> done; too low -> keep the last bit and try the next one;
    too high -> clear the last bit (bit << 1) and try the next one.
    """
    lo, hi = AFE_WINDOW
    if lo <= measured <= hi or bit == 0:
        return offset, 0, True
    if measured > hi:
        offset &= ~(bit << 1)
    return offset | bit, bit >> 1, False


def led_measure(plane4):
    """LED measure: max of 8-px box averages of the 4-line mean (8-bit)."""
    m = plane4.astype("int64").sum(0) // plane4.shape[0]
    return int(max(b.max() for b in box8_per_half(m)))


def led_targets(gain43=0x8000):
    """(low, target, high) peak levels; gain43 = that colour's 1E 43 word."""
    g = max(gain43, 0x8000)
    return 0x698000 // g, 0x6B8000 // g, 0x6D8000 // g     # 211, 215, 219


def main():
    dev = V39(open_transport())
    print("status %02x" % dev.status())
    print("id     %r" % dev.identify())
    print("afe    %s" % (dev.get_afe(),))
    print("buttons %02x" % dev.buttons())
    dev.t.close()


if __name__ == "__main__":
    main()
