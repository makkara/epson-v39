# epsonv39 — SANE backend for the EPSON Perfection V39 / GT-S650

A free SANE backend for USB `04b8:013d`, written from `../PROTOCOL.md`.
It contains no Epson code. It does the firmware upload, the AFE, LED and
shading calibration, and the image pipeline on the host.

## Status

All modes work on real hardware (firmware 1.42):

| mode | 75–300 dpi | 600 dpi | 1200 / 2400 / 4800 dpi |
|---|---|---|---|
| Color | works | works | works |
| Gray | works | works | works |
| Lineart | works (host threshold of gray) | works | works |

Cancelling a running scan works, and the next scan runs normally.

Resolutions below 300 dpi are produced by area-averaging a 300 dpi scan.
Gray at 4800 dpi is a colour scan averaged on the host, because no gray
motor mode was captured at 4800 dpi.

## Firmware

The scanner has no firmware in flash. The backend uploads `esfw010c.bin`
when it opens a scanner whose firmware is not loaded. The file is Epson's
and is not included here. Extract it from Epson's Windows package:

```sh
curl -LO https://ftp.epson.com/drivers/V39_EScan_3950_AM.exe
7z x V39_EScan_3950_AM.exe ModUsd.cab
7z x ModUsd.cab esfw010c.bin
# expected sha256 14986e0823fb805f97099605e381686aded9a86f58228c508675b5e42b4a6211
install -D -m 644 esfw010c.bin ~/.local/share/sane/epsonv39/esfw010c.bin
```

The backend looks for it in this order:

1. `$SANE_EPSONV39_FIRMWARE`
2. `firmware <path>` in `epsonv39.conf`
3. `$XDG_DATA_HOME/sane/epsonv39/` and `~/.local/share/sane/epsonv39/`
4. `/usr/share/sane/epsonv39/`, `/usr/local/share/sane/epsonv39/`,
   `/usr/share/iscan/`, `/usr/lib/firmware/epson/`

## Build and install

Needs the libusb-1.0 and SANE development headers.

```sh
make
sudo make install        # backend, epsonv39.conf, dll.d entry, udev rule
scanimage -L
```

To try it without installing:

```sh
mkdir -p /tmp/sanecfg && echo epsonv39 > /tmp/sanecfg/dll.conf
SANE_CONFIG_DIR=/tmp/sanecfg LD_LIBRARY_PATH=$PWD scanimage -d epsonv39 \
    --mode Color --resolution 300 -x 100 -y 50 -o scan.pnm
```

`SANE_DEBUG_EPSONV39=1…5` turns on logging. Level 2 shows calibration
results, 4 shows every image transfer and 5 shows every USB write and read.

## Options

`--mode Color|Gray|Lineart`, `--resolution 75|100|150|200|300|600|1200|2400|4800`,
`--preview`, `--threshold` (lineart), and the standard `-l -t -x -y` area in
mm (8.5" × 11.7" maximum).

## Notes

* After a cold start, the backend uploads the firmware and then waits
  about 10 s while the firmware initialises and homes the carriage.
* Never USB-reset a wedged scanner. That knocked it off the bus in
  testing, and it needed a physical replug. If it stops answering,
  unplug it.
* Every scan runs the full calibration (≈2.5 s). Epson's calibration
  cache in device RAM is not used.
* The buttons (`1E 85`) are not exposed yet.
