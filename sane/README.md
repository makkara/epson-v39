# epsonv39 backend: notes for developers

Installation and usage are in the [top-level README](../README.md). This
file describes how the backend is put together. The protocol itself is in
[PROTOCOL.md](../PROTOCOL.md).

## Files

| file | purpose |
|---|---|
| `epsonv39.c` | the whole backend, written as an in-tree sane-backends backend |
| `epsonv39.conf.in` | config template: `firmware` path and `usb` device lines |
| `sane-epsonv39.man` | man page, `sane-epsonv39(5)` |
| `epsonv39.desc` | device description for SANE's supported-device lists |
| `Makefile` | standalone build against a downloaded sane-backends release |
| `add-to-sane-backends.py` | adds the backend to a sane-backends source tree |
| `dll.d-epsonv39` | installed as `/etc/sane.d/dll.d/epsonv39` so the dll backend loads it |
| `60-epsonv39.rules` | udev rule: desktop user access (`uaccess`) |

The backend carries the same license header as the other sane-backends
backends: GPL-2.0-or-later with the SANE linking exception.

The backend uses SANE's internal libraries: `sanei_usb` for USB,
`sanei_config` for the config file, `sanei_debug` for logging,
`sanei_thread` for the reader, and `sanei_constrain_value` for option
checks. Entry points are named through `sanei_backend.h`, so they come
out as `sane_epsonv39_*`, the names the dll backend looks for.

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
6. **Reader task** (`sanei_thread`: a thread on Linux, a process
   elsewhere). It reads transfers, unscrambles the interleaved and
   phase-grouped raw lines (§7.2), and crops x. It averages the extra
   vertical lines at 300 dpi and area-averages to the output resolution
   below 300 dpi. It converts to RGB, gray or 1-bit and writes finished
   lines into a pipe. Writes don't block. Up to 64 MiB waits inside the
   reader, so a slow frontend doesn't stall the USB stream. The pipe
   gives `sane_get_select_fd` and non-blocking reads.
7. **Cancel.** `sane_cancel` closes the frontend end of the pipe, and the
   reader stops at its next write. If not every transfer was read, the
   backend sends `1E 18` and drains IN data until a 1 s time-out.
   `sane_cancel` may run from a signal handler while `sane_read` is
   blocked; `sane_read` then returns `SANE_STATUS_CANCELLED`.

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

## Building

`make` downloads sane-backends 1.4.0 into `build/` (GitLab, then
Fedora's copy of the same file; the SHA-256 is checked either way). It
runs its `configure` only to get `config.h` and the sanei Makefiles,
builds the five sanei objects, and links them into
`libsane-epsonv39.so.1`. No autotools are needed. `make SANE_SRC=...`
uses an existing configured tree instead.

For a sane-backends merge request:

```sh
git clone https://gitlab.com/sane-project/backends.git sane-backends
./add-to-sane-backends.py sane-backends
cd sane-backends && ./autogen.sh && ./configure BACKENDS=epsonv39 && make
```

The script copies the backend, config template, man page and description
file, and registers them in `configure.ac`, `backend/Makefile.am`,
`backend/dll.conf.in`, `doc/Makefile.am`, `doc/sane.man` and
`po/POTFILES.in`, next to `epsonds`. On the 1.4.0 release,
`autogen.sh`, configure and make succeed, `tools/sane-desc` accepts the
description file, and the in-tree library scans. The script also applies
cleanly to master as of commit `f4bd073` (October 2026); a full build of
master has not been run.

Test without installing by pointing the dll backend at a private config
directory:

```sh
mkdir -p /tmp/sanecfg && echo epsonv39 > /tmp/sanecfg/dll.conf
cp epsonv39.conf /tmp/sanecfg/
SANE_CONFIG_DIR=/tmp/sanecfg LD_LIBRARY_PATH=$PWD scanimage -d epsonv39 -o scan.pnm
```

`SANE_DEBUG_EPSONV39` sets the log level. 1 shows errors and scan
summaries. 2 adds calibration results and timings. 3 adds every AFE/LED
measurement and the reader's result. 4 adds every image transfer and
button reading. `SANE_DEBUG_SANEI_USB=5` logs every USB transfer.

## Open items

* 4800 dpi gray is produced from a colour scan, because no gray motor
  mode was captured at 4800 dpi.
* Custom gamma tables (`1E 84` type `03`) aren't exposed. The device
  applies identity tables.
* No 16-bit output: the 16-bit raw mode bypasses the device's shading.
