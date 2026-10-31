# Free Linux driver for the EPSON Perfection V39

A SANE backend for the **EPSON Perfection V39** flatbed scanner (sold as
**GT-S650** in Japan, USB ID `04b8:013d`). It works with every
SANE program: `scanimage`, Document Scanner (simple-scan), XSane,
gscan2pdf, Skanlite, and others.

Epson's own Linux support for this scanner is a closed-source plugin for
an old version of its scanning software. This driver was written from
scratch, from a description of the USB protocol (see
[PROTOCOL.md](PROTOCOL.md)), and contains no Epson code. It needs one
file from Epson, the scanner firmware, which a script here downloads
from Epson's own server.

## What works

| | 75–300 dpi | 600 dpi | 1200, 2400, 4800 dpi |
|---|---|---|---|
| Colour | ✓ | ✓ | ✓ |
| Gray | ✓ | ✓ | ✓ |
| Black & white (line-art) | ✓ | ✓ | ✓ |

* Full flatbed area, 8.5" × 11.7" (216 × 297 mm), at any position and size.
* The four front-panel buttons (Start, Copy, Scan to E-mail, PDF), for
  use with [scanbd](#scanner-buttons).
* Cancelling a scan midway.
* Calibration runs automatically before every scan. It takes 1.5–4 s up to
  1200 dpi and 4–9 s at 2400 and 4800 dpi; repeat scans in the same mode
  are at the low end.

Tested on Fedora 44 with firmware 1.42 (the version in Epson's current
driver package). Other Linux distributions should work too; please
[report](#reporting-problems) how it goes.

Not tested: the **Perfection V19** (`04b8:013c`). Epson's Windows
package for it contains the same firmware and scan code as the V39 one.
To try it, uncomment the `usb 0x04b8 0x013c` line in
`/etc/sane.d/epsonv39.conf`, and please report the result.

## Getting started

You need about five minutes and an internet connection.

### 1. Install the build tools

Fedora:

```sh
sudo dnf install gcc make pkgconf-pkg-config libusb1-devel sane-backends 7zip curl
```

Debian, Ubuntu, Linux Mint, Raspberry Pi OS:

```sh
sudo apt install build-essential pkg-config libusb-1.0-0-dev sane-utils 7zip curl
```

Use `p7zip-full` instead of `7zip` on older releases (Debian 12, Ubuntu 22.04).

Arch Linux, Manjaro:

```sh
sudo pacman -S --needed base-devel pkgconf libusb sane 7zip curl
```

### 2. Get the code and build it

```sh
git clone https://github.com/makkara/epson-v39.git
cd epson-v39/sane
make
```

The driver uses SANE's internal libraries for USB access, configuration
and threads. Distributions don't install those, so the first `make`
downloads the sane-backends 1.4.0 source (7.5 MB), checks its SHA-256,
and builds the parts it needs. This takes about ten seconds. If you
already have a configured sane-backends source tree, use it with
`make SANE_SRC=/path/to/sane-backends`.

### 3. Get the scanner firmware

The scanner has no permanent firmware. The driver sends it to the
scanner each time the scanner is switched on. The firmware belongs to
Epson, so it isn't included here. This script downloads Epson's Windows
driver package, takes the firmware file out of it, and checks it:

```sh
sudo ../tools/get-firmware.sh --system
```

This installs `/usr/share/sane/epsonv39/esfw010c.bin`. Without `sudo`
and `--system`, it installs into your home directory
(`~/.local/share/sane/epsonv39/`). That is enough if only you scan on
this computer.

### 4. Install the driver

```sh
sudo make install
sudo udevadm control --reload && sudo udevadm trigger
```

This installs the driver next to the other SANE drivers. It also adds a
rule so you can use the scanner without root. Unplug the scanner and plug
it back in.

### 5. Scan

```sh
scanimage -L
```

This should print something like
`device 'epsonv39:libusb:001:005' is a Epson Perfection V39 flatbed scanner`.
Then scan a page:

```sh
scanimage -d epsonv39 --resolution 300 --format=png -o page.png
```

The first scan after switching the scanner on takes about 10 seconds
longer, because the firmware loads and the scanner initialises. Graphical
scanning apps now find the scanner too.

## Using it

Common `scanimage` examples:

```sh
# A4 colour page as a PDF
scanimage -d epsonv39 --mode Color --resolution 300 -x 210 -y 297 --format=pdf -o page.pdf

# Gray photo, 600 dpi, 10 × 15 cm from the top-left corner
scanimage -d epsonv39 --mode Gray --resolution 600 -x 100 -y 150 --format=tiff -o photo.tif

# Black and white text, darker threshold
scanimage -d epsonv39 --mode Lineart --threshold 150 --resolution 300 --format=png -o text.png
```

`scanimage -d epsonv39 --help` lists all options:

| option | values | meaning |
|---|---|---|
| `--mode` | `Color`, `Gray`, `Lineart` | colour, gray, or black & white |
| `--resolution` | 75, 100, 150, 200, 300, 600, 1200, 2400, 4800 | dots per inch |
| `-l`, `-t` | mm | left and top edge of the scan area |
| `-x`, `-y` | mm | width and height of the scan area |
| `--threshold` | 0–255 | black/white cut-off in line-art mode |
| `--preview` | yes/no | quick low-resolution scan |
| `--calibration-cache` | yes/no | reuse the last calibration in the same mode (faster; default yes) |

High resolutions produce big files and take time. A full page at 1200 dpi
is about 400 MB of colour data. Scan only the area you need.

### Scanner buttons

The buttons appear as SANE options `scan` (Start), `copy`, `email` and
`pdf`. [scanbd](https://sourceforge.net/projects/scanbd/) can watch them
and run a script when you press one. In `scanbd.conf`, give each button
an action:

```
action scan {
    filter = "^scan.*"
    numerical-trigger { from-value = 0  to-value = 1 }
    desc   = "Scan to file"
    script = "scan.script"
}
```

The scanner reports a button only while it is held down. A quick press
lasts less than half a second, so set scanbd's polling interval to 200 ms
in the `global` section (`timeout = 200`; the default is 500).

### Sharing the scanner on the network

With `saned`, install the firmware with `--system` (step 3). saned runs
as its own user and can't see the copy in your home directory.

## Troubleshooting

**`scanimage -L` doesn't list the scanner.**
Check that the computer sees it: `lsusb | grep 04b8:013d`. If it is
there, check that the driver is installed:
`ls /etc/sane.d/dll.d/epsonv39`. Run the command with debugging to see
what the driver does:
`SANE_DEBUG_EPSONV39=2 scanimage -L`.

**"Access to resource has been denied", or it works only as root.**
The udev rule from step 4 isn't active. Run
`sudo udevadm control --reload && sudo udevadm trigger`, replug the
scanner, and make sure you are logged in at the computer, not over SSH.
On systems that use a `scanner` group, add `GROUP="scanner"` to
`/etc/udev/rules.d/60-epsonv39.rules` and add yourself to that group.

**"firmware esfw010c.bin not found".**
Run step 3 again. The driver also looks for a path given by
`firmware /path/to/esfw010c.bin` in `/etc/sane.d/epsonv39.conf`, or in
the environment variable `SANE_EPSONV39_FIRMWARE`.

**The scanner stops responding (time-outs, "Error during device I/O").**
Unplug the USB cable (or switch the scanner off) and plug it back in.
The scanner then loses the firmware, and the driver reloads it on the next
scan. Don't try to reset it in software: that can leave it unable to
reconnect until it is replugged.

**Two drivers list the same scanner.**
If Epson's own software (`iscan` or Epson Scan 2) is also installed,
the scanner may appear twice. Pick the `epsonv39:` device, or remove
the other driver.

## Reporting problems

Open an issue at <https://github.com/makkara/epson-v39/issues>. Please include:

* your Linux distribution and version,
* the command you ran,
* a debug log: `SANE_DEBUG_EPSONV39=3 scanimage -d epsonv39 ... 2> debug.log`.
  For protocol-level problems, use level 5; it logs every USB transfer.

## How it works, and what's in this repository

The V39 is a "host-based" scanner. Calibration, image processing and
even the firmware come from the computer. The driver uploads the
firmware and calibrates the sensor before each scan. It then moves the
carriage, reads the raw image, and assembles the final picture on the
computer.

| path | contents |
|---|---|
| [`sane/`](sane/) | the SANE driver ([notes for developers](sane/README.md)); `add-to-sane-backends.py` adds it to a sane-backends source tree |
| [`PROTOCOL.md`](PROTOCOL.md) | the scanner's USB protocol, worked out by observing Epson's Windows driver |
| [`ref/`](ref/) | recorded command sequences from Epson's driver for every mode (no image data) |
| [`tools/get-firmware.sh`](tools/get-firmware.sh) | firmware download script |
| [`tools/`](tools/) | research tools: a Python prototype driver, capture decoder and replay tool, and the Windows tracer |

## License

This project uses the same license as sane-backends: the
[GNU General Public License, version 2 or later](COPYING). The SANE
driver also carries SANE's standard linking exception, so programs that
load it aren't affected by the GPL. See [LICENSE](LICENSE). This doesn't
cover the scanner firmware (see below).

## Legal

This project is not affiliated with or endorsed by Seiko Epson
Corporation. "EPSON" and "Perfection" are trademarks of Seiko Epson
Corporation. The scanner firmware is Epson's copyrighted software. It is
not distributed here: `tools/get-firmware.sh` downloads it from Epson
onto your own computer.
