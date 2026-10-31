# epsonv39 backend: notes for developers

Installation and usage are in the [top-level README](../README.md). This
file describes how the backend is put together. The protocol itself is in
[PROTOCOL.md](../PROTOCOL.md).

## Files

| file | purpose |
|---|---|
| `epsonv39.c` | the whole backend (libusb, one file, no sanei dependency) |
| `Makefile` | out-of-tree build; finds the SANE backend directory itself |
| `epsonv39.conf` | optional config: `firmware /path/to/esfw010c.bin` |
| `dll.d-epsonv39` | installed as `/etc/sane.d/dll.d/epsonv39` so the dll backend loads it |
| `60-epsonv39.rules` | udev rule: desktop user access (`uaccess`) |

The entry points are exported as `sane_epsonv39_*`, the names the dll
meta-backend looks for.

## Structure of `epsonv39.c`

1. **Tables.** There is one row per hardware resolution (300…4800 dpi).
   Each row holds the sensor width, the motor modes, the `1E 21`/`1E 22`
   timing blocks and the pass sizes, collected from the reference captures
   in `../ref/` (PROTOCOL.md §7.5).
2. **Transport.** Command shapes Q/A/S/SG/W from PROTOCOL.md §3 go over
   bulk endpoints `0x02`/`0x81`. Every image transfer is read with exactly
   one bulk read of the documented size.
3. **Firmware and init.** On open, if status bit `0x10` is clear,
   `esfw010c.bin` is uploaded (§4.2). The backend then waits until the
   firmware has finished booting (status `18`, about 10 s). After that
   it runs the `ESC @` equivalent: identity gamma LUTs, `1E E4`, `1B 40`.
4. **Calibration** (§7.4), before every scan:
   * AFE offset search, LED exposure search, then a second AFE search. With
     a valid calibration-cache entry for the mode, one check read replaces
     each search.
   * A 16-bit dark pass and a white pass over the calibration strip. The
     shading table, the `1E 42` black/base words and the `1E 41` shading
     bytes are computed with Epson's exact algorithm.
5. **Positioning and the image pass** (§7.1). After the move to the scan
   start (`1E 01 0x1000`, `1E 05`), the backend waits until the move has
   started and finished. Then it sends the final setup and `1E 47`.
6. **Reader thread.** This thread reads transfers, unscrambles the
   interleaved/phase-grouped raw lines (§7.2), and crops x. It averages
   the extra vertical lines at 300 dpi and area-averages to the output
   resolution below 300 dpi. It converts to RGB, gray or 1-bit. Finished
   lines go into a 32 MiB ring buffer that `sane_read` drains.
7. **Cancel.** `1E 18`, then IN data is drained until a 1 s time-out.

## Device rules that matter

These cost real debugging time; see PROTOCOL.md §8.1.

* The device ignores every command except `1B 03`/`1B 13` while busy
  (bit `0x40`) or still booting (bit `0x02`). Wait for idle after every
  pass.
* After `1E 05`, status reads idle for about 30 ms before the move
  starts. Sending the final setup during the move stalls high-dpi scans.
* A command abandoned on time-out (no ACK) leaves the device out of sync.
  It then NAKs all OUT traffic until it is replugged. Never USB-reset it.
  That can drop it off the bus completely.

## Building without root

On a system without the -devel packages, the headers can come from
extracted packages:

```sh
dnf download sane-backends-devel libusb1-devel     # or apt-get download ...
mkdir sysroot && for r in *.rpm; do rpm2cpio $r | (cd sysroot && cpio -idm); done
PKG_CONFIG_PATH=$PWD/sysroot/usr/lib64/pkgconfig PKG_CONFIG_SYSROOT_DIR=$PWD/sysroot \
    make DEP_LIBS=/usr/lib64/libusb-1.0.so.0
```

Test without installing by pointing the dll backend at a private config
directory:

```sh
mkdir -p /tmp/sanecfg && echo epsonv39 > /tmp/sanecfg/dll.conf
SANE_CONFIG_DIR=/tmp/sanecfg LD_LIBRARY_PATH=$PWD scanimage -d epsonv39 -o scan.pnm
```

`SANE_DEBUG_EPSONV39` sets the log level. 1 shows errors and scan
summaries. 2 adds calibration results and timings. 3 adds every AFE/LED
measurement. 4 adds every image transfer and button reading. 5 adds every
USB write and read.

## Open items

* 4800 dpi gray is produced from a colour scan, because no gray motor
  mode was captured at 4800 dpi.
* Custom gamma tables (`1E 84` type `03`) aren't exposed. The device
  applies identity tables.
* No 16-bit output: the 16-bit raw mode bypasses the device's shading.
