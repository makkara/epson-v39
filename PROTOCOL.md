# EPSON Perfection V39 (GT-S650) — USB protocol

Reverse-engineered notes for writing a free SANE backend for the
EPSON Perfection V39 / GT-S650 (USB `04b8:013d`).

Status: **work in progress.** Everything below was observed on real
hardware (firmware string `1.42`) by running Epson's own Windows driver
components under a tracer (see [Method](#method-and-tools)). Each statement
carries a confidence tag:

* **[confirmed]** — seen consistently on the wire and/or read from the code.
* **[inferred]** — consistent with the traces, but the meaning is a guess.
* **[unknown]** — the bytes are recorded, but the meaning is not yet understood.

---

## 1. Big picture

The V39 is a "host-based" scanner. Linux support today comes from Epson's
closed `iscan-plugin-gt-s650`, and the Windows driver uses the same
architecture:

```
 application ──ESC/I──▶ interpreter (esxi010c.dll / libesintXX.so) ──"device protocol"──▶ USB bulk ──▶ scanner
                         (closed; does calibration, cropping,
                          resolution conversion, colour handling)
```

* The scanner has **no firmware in flash**. After every power-on the host
  uploads a 64 KiB ARM image (`esfw010c.bin`), see §4. **[confirmed]**
* The firmware speaks a small, low-level command set (§5). It does not
  speak ESC/I.
* The host does ESC/I emulation, calibration, cropping and down-sampling.
  The device applies shading and gamma in hardware, using tables the host
  uploads. **[confirmed]**

A free SANE backend therefore has to implement §4–§8 itself. §9 documents
the ESC/I layer of the closed interpreter, for reference and comparison.

## 2. USB transport

| item | value |
|---|---|
| VID:PID | `04b8:013d`, bcdDevice `0x0100` |
| interface class | vendor (`ff/ff/ff`) |
| endpoints | bulk OUT `0x02` (512 B), bulk IN `0x81` (512 B) |
| control transfers | not used (only the default pipe at enumeration) |

All traffic uses the two bulk pipes. **[confirmed]**

Notes for implementers:

* Image reads (`1E 47`) use transfer sizes that are not multiples of 512.
  The device ends each image transfer with a short packet, so issue exactly
  one bulk read of the documented size per transfer (§7.3). **[confirmed]**
* The Windows driver splits the 64 KiB firmware write into 65520 + 16
  bytes. It is unknown whether a single 65536-byte write would also work.
* If the host stops reading in the middle of an image transfer and goes
  away, the device can wedge (it NAKs all OUT traffic). A USB power cycle
  recovers it. Always read image data to the end, or send cancel (`1E 18`).
  **[confirmed]**

## 3. Framing conventions

Every command starts with a 2-byte opcode written to bulk OUT. The prefix
byte is `1B` (ESC) or `1E`. **[confirmed]** The rest of the exchange follows
one of a few fixed shapes:

| shape | sequence | used by |
|---|---|---|
| **Q** query | `> op(2)`, `< reply(N)` | `1B 03`, `1B 13`, `1E 53`, `1E 65`, `1E 85`, `1E 92`, `1E 9F` |
| **A** action | `> op(2)`, `< 06` | `1B 40`, `1E 05` |
| **S** set | `> op(2)`, `< 06`, `> param(N)`, `< 06` | `1E 01`, `1E 11`, `1E 21`, `1E 22`, `1E 31`, `1E 41`, `1E 42`, `1E 43`, `1E 57`, `1E 5A`, `1E A2` |
| **SG** set+get | `> op(2)`, `< 06`, `> param(N)`, `< reply(M)` | `1B 25`, `1E 08`, `1E 87`, `1E 83` |
| **W** mem write | `> op(2)`, `< 06`, `> hdr(10)`, `> data(len)`, `< 06` | `1E 84` |
| **E4** | `> op(2)`, `< 06`, `> param(4)`, `< 06`, `< reply(4)` | `1E E4` |
| **R** stream | `> op(2)`, `< image transfers …` | `1E 47` |
| **C** cancel | `> 1E 18` (the interpreter reads no reply) | `1E 18` |

`06` is ACK. `15` (NAK) shows up in the ESC/I layer; the device-level
code paths also check for it, but it was never seen on the wire.

All multi-byte integers are **little-endian**.

## 4. Status and firmware upload

### 4.1 Status — `1B 03` (shape Q, 2-byte reply)

The interpreter sends this before almost every command.

| reply | meaning |
|---|---|
| `08 00` | powered, **firmware not loaded** (boot loader) **[confirmed]** |
| `18 00` | firmware running, idle **[confirmed]** |
| `58 00` | firmware running, busy: carriage moving, e.g. after `1E 05`/`1E 01` or at the end of a scan **[confirmed]** |

Bit `0x10` means firmware loaded. The interpreter refuses to run without
it. Bit `0x40` means busy/moving. Bit `0x08` is always set. **[inferred]**

### 4.2 Firmware download

Traced from Epson's WIA driver (`CWIADriver::DownLoadFirmware`) right
after power-on: **[confirmed]**

```
> 1B 03            < 08 00              status: no firmware
> 1B 06            < 06                 enter download
> 01 00 01 00                           4 bytes = esfw010c.bin[0x4C..0x4F]
> <65536 bytes>                         esfw010c.bin[0x100..0x100FF] (sent as 65520 + 16)
> A0                                    checksum = ~(sum of the 65536 bytes) & 0xFF
                   < 06
> 1B 16            < 06
> 80               < 06                 (probably "start firmware")
```

After this, `1B 03` returns `18 00`. The firmware is lost on USB power
loss.

`esfw010c.bin` layout (65793 bytes): **[confirmed]**

| offset | size | content |
|---|---|---|
| `0x000` | 256 | header, magic `EDLA`; at `0x40`: `00 01 00 00 01 00 00 01 00 00 B8 04 01 00 01 00` (contains VID `04B8`; the last 4 bytes are the download header) |
| `0x100` | 65536 | ARM Cortex-M image (vector table: SP `0x02000400`, reset `0x02000560`) |
| `0x10100` | 1 | file checksum (`BF`); not sent |

The firmware is copyrighted by Epson and cannot be distributed with SANE.
The backend should load it from a user-supplied path, the same way
`iscan` and `epsonscan2` do. A Linux copy ships in Epson's packages as
`/usr/share/iscan/esfw010c.bin`.

## 5. Device command reference

`>` is host→device and `<` is device→host. "Calls" counts uses in a single
300 dpi colour scan, including calibration.

### 5.1 Identification and housekeeping

| op | shape | params → reply | notes |
|---|---|---|---|
| `1B 13` | Q | → 28 bytes ASCII | `"EPSON   Perfection V39  1.42"`: vendor(8), product(16), firmware version(4). **[confirmed]** |
| `1B 40` | A | | reset / initialise. **[confirmed]** |
| `1E 65` | Q | → 1 byte (`00`) | **[unknown]**, used by ESC/I `ESC f` / `FS F` (maybe option/ADF status) |
| `1E 9F` | Q | → 1 byte (`00`) | **[unknown]**, used by ESC/I `FS I` |
| `1E 85` | Q | → 1 byte | **button status**; `00` = none pressed. The WIA service polls it once a second. Bit assignment not mapped yet. **[confirmed]** |
| `1E 18` | C | | cancel the running scan (ESC/I `CAN`). **[confirmed from code]** |

### 5.2 Memory access

Device RAM is addressed through `1E 83` (read) and `1E 84` (write). Both
use a 10-byte header: **[confirmed]**

```
byte 0     type   (03 = LUT, 06 = shading, 09 = calibration cache, 0B = ?)
byte 1     00
bytes 2-5  address (u32)
bytes 6-9  length  (u32)
```

`1E 83` (shape SG) returns `length` bytes. `1E 84` (shape W) sends
`length` bytes after the header, then ACK.

Regions seen:

| type | address | length | content |
|---|---|---|---|
| `03` | `0x021FFC00` / `0x021FFD00` / `0x021FFE00` | 256 each | 8-bit output LUT (gamma) for R / G / B. ESC/I init writes identity tables. **[confirmed]** |
| `06` | `0x02012000` | W×3×2 | per-pixel shading coefficients, u16, written after the white-reference scan (15552 bytes at 300 dpi = 2592 px × 3 × 2). **[confirmed size/place, inferred meaning]** |
| `09` | `0x021FFF00` | 12 | calibration cache: AFE offsets as 3 × u32, e.g. `80 00 00 00 76 00 00 00 46 00 00 00`. **[inferred]** |
| `09` | `0x021FFF10` | 16 | calibration cache key: u32 last resolution, u32 pixel count, then mode bytes (`58 02 00 00 84 03 00 00 11 00 06 00 01 00 13 01`). The interpreter reads it back and skips the slow AFE search when it still matches. **[inferred]** |
| `0B` | `0x00000000` | 10 | read at init, all zero. **[unknown]** (NVRAM?) |

`1E E4` with param `0B 00 04 00` returns `14 17 43 01`. It looks like a
read of type `0B`, address 4. **[unknown]**

`1B 25` (shape SG) takes a u32 address and returns a u32, e.g. address
`0x0037F844` → `0x28`, `0x58`. It looks like a hardware register read. The
same value appears in status bits. **[inferred]**

### 5.3 Motion and lamp

| op | shape | param | notes |
|---|---|---|---|
| `1E 01` | S | 6 bytes: `u16 mode, u32 distance` | move the carriage. Values seen: `10 00 30 00 00 00` (calibration), `00 10 A1 09 00 00`, and to scan start `09 00 08 07 00 00` (300 dpi colour, y=2"), `0B 00 08 07` (300 gray), `08 00 58 02` (600, y=2"), `05 00 F0 00` (1200, y=2"). The units depend on `mode`. **[inferred]** |
| `1E 05` | A | — | return to home. Poll `1B 03` until `58 00` turns into `18 00` (≈1.2 s). **[confirmed]** |
| `1E 11` | S | 1 byte (`04`) | lamp/LED power on; the host waits ~30 ms afterwards. **[inferred]** |
| `1E 08` | SG | 1 byte (`09`) → 4 bytes (`E6 00 00 00`) | **[unknown]**, part of the calibration move |
| `1E 87` | SG | 1 byte (`00`) → 6 bytes (`01 00 F6 FF E9 FF`) | three s16 values (1, −10, −23); maybe per-colour line or position offsets. **[unknown]** |
| `1E 5A` | S | 1 byte `00`/`01` | **[unknown]**, toggled around calibration |
| `1E A2` | S | 1 byte `00`/`01` | set to `01` right before the final `1E 47`, `00` otherwise. **[inferred]** (data-transfer / motor enable) |

### 5.4 Analog front end and sensor timing

| op | shape | param |
|---|---|---|
| `1E 21` | S | u32 line timing: `0x210` (300 dpi), `0x480` (600), `0x900` (1200). **[inferred]** (pixel clocks per line) |
| `1E 22` | S | 40 bytes LED/exposure block (below) |
| `1E 31` | S | 18 bytes AFE: `u16 offset[6]`, `u16 gain[3]` |
| `1E 92` | Q | → 18 bytes, same layout as `1E 31` (read-back) |
| `1E 42` | S | 18 bytes **[unknown]**. All zeros during calibration; at scan time e.g. `08 00 13 00 01 00 | 58 04 1D 04 | 58 04 1D 04 | 58 04 1D 04` (300 dpi) |
| `1E 43` | S | 12 bytes: six u16 near `0x8000`, e.g. `DB 7F E2 7F 00 80 …`; per-channel/half gain, Q15 ≈ 1.0. **[inferred]** |

`1E 22` (40 bytes):

```
u16  mode            01 = calibration, 02 = scan
u32  t_on[3]         per R,G,B   (D6 00 00 00 …; calibration: D6, 41E, A90)
u32  t_off[3]        per R,G,B   (0A55 @300, 1757 @600, 2DD7 @1200)
u16  ?[3]            02A3 @300, 0640 @600, 0C80 @1200
u16  led_duty[3]     R,G,B; tuned during calibration (0087 → 0140/0124/00AC @300)
u16  flag            00 / 01
```

**[inferred]** The field grouping follows how the values change with
resolution and during the exposure search. The exact semantics are unknown.

AFE offset calibration is a binary search on all six offsets: `0x80`,
`0xC0`, `0xA0`, `0x90`, `0x88`, `0x84`, `0x86`, `0x85`, with gains fixed at
`0x3A`. Each step takes a 4-line dark read. Six offsets fits two sensor
halves × three colours (§7.2). **[confirmed sequence, inferred meaning]**

### 5.5 Scan setup and data

`1E 57` (S, 42 bytes) / `1E 53` (Q, 42 bytes) store and read a parameter
record. It shows the device-level geometry the interpreter derived: **[inferred]**

```
u32 out_res   u32 scan_yres   u32 y_out   u32 y_scan   u32 width_px   u32 lines   u8 mode   u8 depth   …
300 colour:  300  600  600  1200  2592  900   13 08 00 00 00 03 00 00 00 80 …
300 gray:    300 1200  600  2400  2592 1800   00 08 …
```

`1E 41` set-scan (S, 22 bytes) is the main per-pass setup:

| bytes | meaning | examples |
|---|---|---|
| 0–1 | u16 first pixel (constant `0x015F` = 351) **[inferred]** | |
| 2–3 | u16 end pixel = 351 + W | `0B7F` (W=2592 @300), `158F` (5168 @600), `29BF` (10336 @1200) |
| 4–7 | u32 number of lines | `0x384` = 900 |
| 8 | LED on (`01`) / off (`00`, dark reference) **[confirmed]** | |
| 9 | `01` normal; `04` for the long shading-reference pass | |
| 10 | `02` (300/600 colour), `01` (gray, 1200 colour) **[unknown]** | |
| 11–12 | `80 80` for image scans, `00 00` for calibration | |
| 13 | `01` gray / dark 16-bit, `00` colour **[inferred]** | |
| 14 | `00` | |
| 15 | `81` @300, `72` @600/1200 **[unknown]** | |
| 16–17 | `80 A0` image scan, `80 00` calibration | |
| 18 | **lines per USB transfer** **[confirmed]** | `08`, `04`, `02`, `19` |
| 19–21 | `00` | |

`1E 47` (R) starts the pass. The device then streams
`ceil(lines / lines_per_transfer)` transfers (§7.3).

## 6. Init sequence (what ESC/I `ESC @` triggers)

Captured after firmware load, with the WIA service locked out: **[confirmed]**

```
1E 83  0B 00 00000000 0A000000        → 10 × 00
1E 84  03 00 021FFC00 00000100  + 256-byte identity LUT  (R)
1E 84  03 00 021FFD00 …         + identity LUT          (G)
1E 84  03 00 021FFE00 …         + identity LUT          (B)
1B 03                                  → 18 00
1E E4  0B 00 04 00                     → 06, 14 17 43 01
1B 40                                  → 06
```

## 7. Scanning

### 7.1 Sequence

This is how Epson's interpreter performs one scan (ESC/I `FS W` + `FS G`).
Status polls (`1B 03`) between commands are omitted.

1. **Record the parameters:** `1E 53` (read), `1E 57` (write the 42-byte record).
2. **Prepare:** `1E A2 01`, `1E 87 00`, `1B 25 0x0037F844`, `1E 83` of the
   calibration cache key `0x021FFF10`.
3. **AFE calibration**, done only if the cache key differs:
   * `1E A2 00`, `1E 5A 00`, `1E 11 04` (lamp)
   * `1E 21`, `1E 22` (mode 1, LED duty 0), `1E 42` zero, `1E 43` unity
   * repeat (8×): `1E 31` offsets → `1E 41` (4 lines, LED off) → `1E 47` read dark lines; binary-search the offsets
   * LED exposure search: `1E 22` with updated `led_duty`, `1E 41` LED on, `1E 47` (4 lines)
   * store the results with `1E 84` type `09` to `0x021FFF00` / `0x021FFF10`
   * Otherwise (cache hit) it just reads `0x021FFF00` back and programs `1E 31`.
4. **Dark + white reference:** 8 dark lines (LED off, 16-bit), then
   `1E 01` a short move, then a 64-line white pass (`1E 41` byte 9 = `04`).
5. **Upload shading:** `1E 84` type `06` to `0x02012000` (W×3 u16).
6. **Return and position:** `1B 25`, `1E 5A 01`, `1E 08 09`,
   `1E 01 00 10 A1 09 00 00`, `1E 05` (home; poll until idle).
7. **Final setup:** `1E 21`, `1E 22` (mode 2, final LED duties), `1E 42`,
   `1E 43`, `1E 01` (move to the scan start), `1E 41` (full geometry),
   `1E A2 01`.
8. **Start:** `1E 47`, then read every transfer.
9. The host sends nothing after the last transfer; the carriage appears to
   return home on its own, with status showing `58 00` until it arrives.
   **[inferred]**

From `FS G` to the first image data takes ≈3.6 s on the first scan after
power-up and ≈2.5 s when the calibration cache hits.

### 7.2 Raw line format

The device always scans the **full sensor width** at the hardware
resolution and the host crops. **[confirmed]**

| output mode | hw x-res | W (pixels/line) | hw lines per output line | bytes / raw line |
|---|---|---|---|---|
| 300 dpi colour | 300 | 2592 | 2 (600 lpi) | 3 × W = 7776 |
| 300 dpi gray / line-art | 300 | 2592 | 4 (1200 lpi) | W = 2592 |
| 600 dpi colour | 600 | 5168 | 1 | 15504 |
| 1200 dpi colour | 1200 | 10336 | 1 | 31008 |

The interpreter only accepts 300/600/1200/2400 dpi (ESC/I `ESC I` lists
`R 300, R 600, R 1200, R 2400`). The WIA driver produces 100/150/… dpi by
resampling on the host. 2400 dpi has not been captured yet.

A colour line has **three planes**, R then G then B, each W bytes of 8-bit
samples. A gray line is one plane. **[confirmed]**

Inside a plane, bytes **alternate between the left and right halves of the
sensor**: **[confirmed]**

```
plane = L0 R0 L1 R1 L2 R2 …     L = pixels 0 … W/2-1, R = pixels W/2 … W-1
```

At **1200 dpi** each half additionally arrives as all its even pixels,
then all its odd pixels (staggered sensor rows, already aligned
vertically by the device): **[confirmed]**

```
half = h0 h2 h4 … h(N-2)  h1 h3 h5 … h(N-1)        (N = W/2)
```

The data is **already shading-corrected** by the device (§5.2 type `06`).
The interpreter's 8-bit output equals the unscrambled raw data within
~2–4 levels. The remaining difference is probably the colour/gamma step.
**[confirmed]**

Reference unscramble (numpy):

```python
def unscramble(lines, W, stagger):          # lines: (n, planes, W) uint8
    left, right = lines[..., 0::2], lines[..., 1::2]
    def fix(h):
        if not stagger:
            return h
        n = h.shape[-1]; out = np.empty_like(h)
        out[..., 0::2] = h[..., :n // 2]; out[..., 1::2] = h[..., n // 2:]
        return out
    return np.concatenate([fix(left), fix(right)], axis=-1)
```

Cropping and down-sampling as Epson's interpreter does it: **[confirmed]**

* x: output pixel `x` (at output dpi) = raw pixel `x + 17·(dpi/300)`, so a
  fixed ≈1.44 mm left margin.
* y: output row `r` = mean of raw lines `r·k … r·k+k-1`, with k from the
  table above (2 for 300 dpi colour, 4 for 300 dpi gray).
* Line-art (1-bit) is host-side thresholding of the gray scan; the device
  sees the same commands as gray.

Dark-reference lines during calibration are 16-bit (`1E 41` byte 13 = `01`,
W × 2 bytes, LED off). White-reference lines are 8-bit, like image data.
**[inferred]**

### 7.3 Transfers

`1E 47` returns `ceil(lines / L)` bulk transfers, where L = `1E 41` byte 18.
Each transfer is:

```
L × line_bytes   image data
8 bytes          trailer
```

For example, 300 dpi colour: L=8, 62216 = 8 × 7776 + 8, and the last
transfer is 31112 = 4 lines + 8. The trailer looks like
`02 xx yy 00 00 00 80 00`. Byte 0 is always `02`; bytes 1–2 vary per scan
(`00 F3`, `20 FD`, `40 F2`) and differ in the last transfer (`80 79`).
**[unknown]** (status/sequence)

There is no ACK between transfers. The device streams at ~5 MB/s at 300 dpi
colour.

## 8. Things that are still open

* Exact meaning of `1E 42`, `1E 22` fields, `1E 41` bytes 9–17, `1E 01`
  units, `1E 08`, `1E 87`, `1E 5A`, `1E A2`, and the transfer trailer.
* How the shading coefficients are computed from the dark/white
  references, i.e. the format of the type `06` table. Next step: dump the
  references and the uploaded table from a capture and fit them.
* LED exposure search algorithm (target levels).
* 2400 dpi and 4800 dpi (`FS I` reports max 4800) layouts.
* Button bit mapping (`1E 85`); there are 4 buttons: Start/Copy/Send/PDF.
* Behaviour on errors / cover open / carriage stuck.

## 9. Reference: the closed ESC/I interpreter

### 9.1 Plugin ABI

These DLLs are listed under `HKLM\SYSTEM\CurrentControlSet\Control\Class\{6bdd1fc6-…}\NNNN\DeviceData`
(`ExtCmdModule=esxi010c.dll`, `ExtBinModule=esfw010c.bin`).
`esxi010c.dll` (x64) / `esint00.dll` (x86 stub) export: **[confirmed]**

```c
typedef int (*usb_cb)(void *buf, int len);       /* nonzero = success, must transfer exactly len */
BOOL INTInit(usb_cb read, usb_cb write);
BOOL INTWrite(void *buf, int len);                /* ESC/I bytes in  */
BOOL INTRead (void *buf, int len);                /* ESC/I bytes out (same entry point as INTWrite) */
void INTClose(void);
int  INTGetInterpreterError(void);
```

During `FS G` the interpreter reads USB from a background thread, so the
callbacks must be thread-safe. The interpreter does **not** upload firmware;
the host does that before `INTInit` (§4.2).

### 9.2 ESC/I commands handled (observed)

| ESC/I | reply |
|---|---|
| `ESC @` | `06` |
| `ESC I` | `02 02 13 00` + `44 32 52 2C 01 52 58 02 52 B0 04 52 60 09 41 B0 4F B0 6D` → command level `D2`, resolutions 300/600/1200/2400, area 20400 × 28080 (at 2400 dpi = 8.5" × 11.7") |
| `ESC F` | `02 02 00 00` |
| `ESC f` | `02 02 2A 00` + 42 bytes (product name at 26) |
| `FS I` | 80 bytes: `"D2"`, u32 4800 (base res), u32 300 (min), u32 4800 (max), u32 31200, u32 40800 (= 8.5" at 4800), u32 56160 (= 11.7" at 4800), …, product `Perfection V39  1.42` at offset 46 |
| `FS F` | 16 bytes, byte 3 = `C0` |
| `FS S` / `FS W` | 64-byte parameter block (below) |
| `FS G` | 14-byte header `02 st bs(4) bn(4) ls(4)`, then `bn` blocks of `bs`+1 bytes and a last block of `ls`+1. The extra byte is an error code; the host sends `06` after each block (`18` to cancel). Note that `bn` *includes* the last block. |

`FS W` block (validation read from `esxi010c.dll`; `15` = NAK):

| off | field | accepted |
|---|---|---|
| 0 | u32 x-res | 300, 600, 1200, 2400 (100/150 → NAK) |
| 4 | u32 y-res | same as x |
| 8, 12 | u32 x, y offset (pixels at res) | |
| 16, 20 | u32 width, height (pixels) | |
| 24 | colour mode | `00`/`10`/`20`/`30` mono, `12` line-seq colour, `13` pixel colour |
| 25 | bit depth | 1 or 8 |
| 26 | option control | must be 0 |
| 27 | scan mode | 0 or 1 |
| 28 | block line count | multiple of 3 for mode `12` |
| 29 | gamma | 3 or 4 |
| 33 | threshold | any |
| 37 | film type | 0 or 1 |
| 38 | lamp mode | stored |
| 42–43 | u16 | 5000…15000, otherwise forced to 10000 |

## Method and tools

* `tools/esitrace.c`: a Windows x64 tool that loads Epson's
  `esxi010c.dll`, passes it logging USB callbacks on `\\.\UsbscanN`, and
  speaks ESC/I to it (`info`, `scan`, `raw`, `drain`, `usbinfo`).
  `-r file.bin` records every bulk transfer. It holds the WIA driver's
  `Global\Es_Mutex_NNNN` mutex: the WIA service otherwise polls the scanner
  once a second (status + buttons), and those polls corrupt a running scan.
* `tools/v39dump.py`: decodes `-r` captures into command transactions
  (`--summary` for a histogram).
* Epson's WIA driver has a built-in trace. Put a file
  `C:\ProgramData\epson\wialog\logsetting.ini` containing
  `[Setting]` / `LogLevel=1`, then replug the scanner; the log goes to the
  same folder. Raw writes and reads under 2 KiB are dumped, including the
  firmware download.

Build `esitrace` with LLVM: `clang -O1 -o esitrace.exe tools/esitrace.c`.
