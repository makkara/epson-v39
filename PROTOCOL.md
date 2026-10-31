# EPSON Perfection V39 (GT-S650) — USB protocol

Reverse-engineered notes for writing a free SANE backend for the
EPSON Perfection V39 / GT-S650 (USB `04b8:013d`). The working backend
built from these notes is in [`sane/`](sane/); see the
[README](README.md) to install it.

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
> A0                                    checksum = (sum of the 65536 bytes) & 0xFF
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
| `1E 85` | Q | → 1 byte | **button status**: the number of the button held down now, not a bit mask. `00` none, `01` Start, `02` Copy, `03` Scan to E-mail, `04` PDF. It does not latch; a short press reads non-zero for about 0.4–0.9 s, so poll several times a second. **[confirmed on hardware]** |
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
| `06` | `0x02012000` | W×P×2 | per-pixel shading table, u16, P planes of W entries in raw (interleaved) pixel order, written after the white-reference scan. The device divides by it (§7.4). **[confirmed]** |
| `09` | `0x021FFF00` | 12 | calibration cache data: the three LED duties (R, G, B) as u32, divided by a per-resolution factor f = 2.5, 2.5, 5, 10, 20 at 300…4800 dpi and rounded. Duties 317/292/170 @300 are stored as `7F 75 44`; 1205/1195/690 @1200 as `F1 EF 8A`. On a hit the interpreter restores duty = floor(value × f). **[confirmed in all captures]** |
| `09` | `0x021FFF10` | 16 | calibration cache key: u32 x and u32 width of the last request (pixels at its resolution), then 8 mode bytes. Byte 8 = 4800/dpi + 1, plus 20 for gray (`11` 300 colour, `25` 300 gray, `05` 1200 colour, `19` 1200 gray). Byte 14 = `13` colour / `00` gray; bytes 10, 12, 15 are not understood. y is not part of the key. **[inferred]** |
| `0B` | `0x00000000` | 10 | read at init, all zero. **[unknown]** (NVRAM?) |

`1E E4` with param `0B 00 04 00` returns `14 17 43 01`. It looks like a
read of type `0B`, address 4. **[unknown]**

`1B 25` (shape SG) takes a u32 address and returns a u32, e.g. address
`0x0037F844` → `0x28`, `0x58`. It looks like a hardware register read. The
same value appears in status bits. **[inferred]**

### 5.3 Motion and lamp

| op | shape | param | notes |
|---|---|---|---|
| `1E 01` | S | 6 bytes: `u16 mode, u32 steps` | motor command; steps are **1/1200"**. `mode 0x1000` = fast move (positioning to the scan start). Any other mode = motor speed profile for the next pass, with `steps` = the length of that pass. See §7.5 for the mode codes and §7.1 for the formulas. **[confirmed]** |
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
| `1E 31` | S | 18 bytes AFE: `u16 offset[6]` (left half R,G,B, right half R,G,B), `u16 gain[3]` (always `0x3A`). **[confirmed]** |
| `1E 92` | Q | → 18 bytes: `u16 offL, 0, 0, offR, 0, 0, gain, 0, 0` (read-back) **[confirmed]** |
| `1E 42` | S | 18 bytes: `u16 base[3]` (per colour), then `u16 black_L, black_R` repeated for R, G, B. All zeros during the AFE/LED search. See §7.4. **[confirmed]** |
| `1E 43` | S | 12 bytes: six u16 near `0x8000`, e.g. `DB 7F E2 7F 00 80 …`; per-colour gain, Q15 ≈ 1.0. The interpreter also uses these words (clamped to ≥ `0x8000`) to scale the LED targets (§7.4). **[partly exact]** |

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

The AFE offset search and LED exposure search are described in §7.4.

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
| 8 | **output format**: `00` = 16-bit raw (no shading), `01` = 8-bit. Tested by patching only this byte. In the interpreter the high nibble is a line-period speed code (`0x10`…`0xB0`), which is 0 in all captured modes. **[confirmed]** | |
| 9 | `01` normal; `04`/`02` for the white-reference pass | |
| 10 | `02` (300/600 colour), `01` (gray, 1200 colour), `03` (white pass) **[unknown]** | |
| 11 | bit 7 = **shading on**, bits 0–1 = shading-table field split `sel` (§7.4); `40` white pass, `00` dark/AFE passes. **[exact for image scans]** | `80` |
| 12 | `80` image scan, `00` calibration passes | |
| 13 | `01` gray and AFE dark reads (one plane), `00` colour **[inferred]** | |
| 14 | `00` | |
| 15 | shading-table shifts: high nibble = gain shift shB, low nibble = dark shift shA (§7.4). **[exact]** | `81` @300, `72` @600/1200 |
| 16–17 | `80 A0` image scan, `80 80` white pass, `80 00` dark passes | |

Bytes 9–17 together select shading, LED gating and the plane layout.
Copying the image-scan values onto a calibration pass turns the LEDs on
and enables shading, so these bits interact. A driver should send the
exact per-pass blocks listed in §7.1; the individual bits are
**[unknown]**.
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
   calibration cache key `0x021FFF10` (§5.2).
3. **AFE and LED calibration.** On a cache miss:
   * `1E A2 00`, `1E 5A 00`, `1E 11 04` (lamp)
   * `1E 21`, `1E 22` (mode 1, LED duty 0), `1E 42` zero, `1E 43` unity
   * repeat (8×): `1E 31` offsets → `1E 41` (4 lines, LED off) → `1E 47` read dark lines; binary-search the offsets
   * LED exposure search: `1E 22` with updated `led_duty`, `1E 41` LED on, `1E 47` (4 lines)
   * the AFE search again
   * store the key and the scaled duties with `1E 84` type `09`

   On a cache hit (key equal to the current request), the AFE offsets
   are read back from the AFE with `1E 92`, and the duties come from
   `0x021FFF00`. A single 8-line dark read and a single 4-line LED read
   follow; both searches are skipped. **[confirmed in 300-color-y1/y2/y3/y6/h]**
4. **Dark + white reference:** a 32-line 16-bit dark pass (LED off), then
   `1E 42` with the black levels, `1E 01 mode=Mcal steps=48` (the
   calibration motor mode from §7.5; 48/1200" over the white calibration
   strip), then a 16-bit white pass (64 lines @300 colour, 192 @300 gray,
   32 at 600 dpi and above).
5. **Upload shading:** `1E 84` type `06` to `0x02012000` (W × planes u16), computed as in §7.4.
6. **Position:** `1B 25`, `1E 5A 01`, `1E 08 09`, then
   `1E 01 mode=0x1000 steps=S`, then `1E 05`, then poll until idle. S is
   the distance from the calibration position to the top of the scan
   area, in 1/1200" (y is the top edge requested in FS W, in inches):

   ```
   S = round(y · 1200) + 65     at 300 and 600 dpi (gray @300: + 64)
   S = round(y · 1200) + 174    at 1200, 2400 and 4800 dpi
   ```

   Checked at y = 0, 1, 2, 3 and 6 inches. **[confirmed]**
7. **Final setup:** `1E 21`, `1E 22` (mode 2, final LED duties), `1E 42`,
   `1E 43`, `1E 01 mode=M steps=round(h·1200)` (h = scan height in
   inches, M = the image-scan motor mode from §7.5), `1E 41` (lines =
   h · vertical rate, see §7.2), `1E A2 01`.
8. **Start:** `1E 47`, then read every transfer.
9. The host sends nothing after the last transfer; the carriage appears to
   return home on its own, with status showing `58 00` until it arrives.
   **[inferred]**

From `FS G` to the first image data takes ≈3.6 s on the first scan after
power-up and ≈2.5 s when the calibration cache hits.

### 7.2 Raw line format

The device always scans the **full sensor width** at the hardware
resolution and the host crops. **[confirmed]**

| dpi | W (pixels/line) | phase groups k | dummies/half | raw lines per output line | x offset |
|---|---|---|---|---|---|
| 300 | 2592 | 1 | 4 | colour 2, gray 4 | 17 |
| 600 | 5168 | 1 | 0 | 1 | 34 |
| 1200 | 10336 | 2 | 0 | 1 | 68 |
| 2400 | 20672 | 4 | 0 | 1 | 136 |
| 4800 | 41344 | 8 | 0 (not checked) | 1 | 272 |

Bytes per raw line = W × planes (3 colour, 1 gray), 8-bit. All rows were
verified against the interpreter's output, colour and gray. The only
exception is 4800 dpi, which is colour only. **[confirmed]**

The interpreter accepts 300/600/1200/2400/4800 dpi in `FS W` (ESC/I
`ESC I` lists only up to 2400). The WIA driver produces 100/150/… dpi by
resampling on the host.

A colour line has **three planes**, R then G then B, each W bytes of 8-bit
samples. A gray line is one plane. **[confirmed]**

Inside a plane, bytes **alternate between the left and right halves of the
sensor**: **[confirmed]**

```
plane = L0 R0 L1 R1 L2 R2 …     L = pixels 0 … W/2-1, R = pixels W/2 … W-1
```

At **1200 dpi and above** each half additionally arrives as k phase
groups (k = dpi/600). The device has already aligned the staggered
sensor rows vertically: **[confirmed]**

```
half = h0 hk h2k …  h1 h(k+1) …  …  h(k-1) h(2k-1) …      (group j = pixels j, j+k, j+2k, …; N/k each)
k=2 @1200:  h0 h2 h4 … h1 h3 h5 …
```

At **300 dpi the last 4 pixels of each half are dummies**: they read as
zero/saturated, and the shading table holds `0xFFFF`/tiny values there.
The image is therefore `L[0:1292] + R[0:1292]`, 2584 pixels wide.
Leaving the dummies in produces a coloured seam at the sensor centre and
shifts the right half by 4 pixels. At 600, 1200 and 2400 dpi there are no
dummies; this was fitted on captures that cross the centre.
**[confirmed]**

In 8-bit mode the data is **already shading-corrected** by the device
(§7.4). After unscrambling and dummy removal, the interpreter's output
equals the raw data within 0.4 (gray) to 1.7 (colour) levels of 255.
**[confirmed]**

Reference unscramble (numpy):

```python
def unscramble(lines, k, dummy):   # lines: (n, planes, W); k, dummy from the table
    left, right = lines[..., 0::2], lines[..., 1::2]
    def fix(h):
        n = h.shape[-1]; out = np.empty_like(h)
        for j in range(k):                       # phase group j -> pixels j, j+k, ...
            out[..., j::k] = h[..., j * n // k:(j + 1) * n // k]
        return out[..., :n - dummy]
    return np.concatenate([fix(left), fix(right)], axis=-1)
```

**Noise.** Sensor noise in 8-bit output is ≈0.4 levels in black and
≈3–5 levels per pixel in mid/bright areas at 300 dpi; it is slightly worse
in red. Epson's driver output has the same grain: the interpreter does no
denoising. Its optional "Descreening" / "Unsharp mask" filters run on the
host. On newsprint, most of the visible "dirt" is paper fibre and the
halftone screen. **[confirmed by comparison]**

Cropping and down-sampling as Epson's interpreter does it: **[confirmed]**

* x: output pixel `x` (at output dpi) = unscrambled pixel `x + 17·(dpi/300)`,
  a fixed ≈1.44 mm left margin. Verified at 300–4800 dpi. x is never
  sent to the device.
* y: output row `r` = mean of raw lines `r·v … r·v+v-1`, with v = "raw
  lines per output line" from the table (2 for 300 dpi colour, 4 for
  300 dpi gray, otherwise 1). The device is positioned at y with
  `1E 01 0x1000` (§7.1).
* Line-art (1-bit) is host-side thresholding of the gray scan; the device
  sees the same commands as gray.

Calibration passes with `1E 41` byte 8 = `00` return the same layout
with **16-bit little-endian** samples: 2 bytes per sample, one plane for
AFE reads and gray, three planes for colour dark/white passes.
**[confirmed]**

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

### 7.4 Calibration maths

Everything in this section was read out of `esxi010c.dll` (function
addresses are given for reference). It was then checked against all four
captures and on hardware. Implementations are in `tools/v39.py`
(`shading_table`, `afe_measure`/`afe_next`, `led_measure`/`led_targets`).

**AFE offsets** (`1E 31`, `0x180003F20` / decision `0x1800015E0`).
LEDs off, all three colours of a half share one offset. For each sensor
half independently, run a successive-approximation search:

```
offset = 0x80, bit = 0x40
loop:  program 1E 31, scan 4 dark lines (16-bit, one plane)
       m = min over x of the 8-pixel box average (within the half, raw stride 2)
           of the 4-line mean
       if 0x380 <= m <= 0x480 (896…1152): done
       if m > 0x480: offset &= ~(bit << 1)        # undo the last bit
       offset |= bit; bit >>= 1                     # try the next bit
```

A relaxed window `0x300…0x500` is used when an internal flag is set
(`obj+0x376`). Every decision in the captures matches, including
gray @300 where the right half ends at 133 because its measure sat on
the window edge (1152/1153). **[exact]**

**LED exposure** (`1E 22` `led_duty`, `0x180004426`…). LEDs on, 8-bit
unshaded reads of 4 lines.

```
peak_c   = max over x of the 8-pixel box average (within a half) of the 4-line mean
targets  = 0x698000 / g, 0x6B8000 / g, 0x6D8000 / g   with g = max(1E 43 word_c, 0x8000)
         = 211, 215, 219 for g = 0x8000 (the normal case)
```

A colour whose peak is inside 211…219 is left alone. Otherwise its duty is
scaled towards 215 (≈ `duty × 215 / peak`; observed updates are within
1–2% of that). It is repeated 2–3 times. Start duties: 135 @300, 320 @600,
640 @1200. **[exact constants; update rounding approximate]**

**Shading table, black and base** (`0x180002F50`, `0x180002990`,
`0x1800049A0`, `0x180002B50`, packing `0x180002840`). Inputs are the
16-bit dark pass (32 lines) and the 16-bit white pass, both in raw
(interleaved) pixel order, P planes of W pixels.

```
A[x]      = floor( sum over lines and planes of dark / (lines·P) )      # one per pixel
black_h   = min of A over half h (h = x mod 2)                         -> 1E 42 words 3..8 (L,R)×3
A[x]     -= black_h
rngA      = max over halves of (max A − min A)       # before subtracting
sel, shA  = 3,0 if rngA<0x20 | 2,0 if <0x40 | 1,0 if <0x80 | 0,0 if <0x100
            else 0,k with k = smallest 1..8 such that rngA < 0x100<<k
white is split into 4 equal groups of lines; G_g[c][x] = floor(mean of group g)
B[c][x]   = (max_g G_g[c][x] − A[x]) mod 65536       # u16 arithmetic; dummy pixels wrap
base_c    = min over x of B[c]                         -> 1E 42 words 0..2
shB       = smallest k ≥ 0 with max_c(max B[c] − base_c) < 2^(8+sel+k)
T[c][x]   = (((B[c][x] − base_c) >> shB) << (8 − sel)) | (A[x] >> shA)
1E 41 byte 11 = 0x80 | sel        (bit 7 = shading on)
1E 41 byte 15 = shB << 4 | shA
```

Each table entry thus packs the pixel's white gain in the high bits and
its residual dark offset in the low `8 − sel` bits. During calibration
passes `1E 42` is all zeros. With gray, P = 1 and base is repeated for
all three words.

Re-implementing this reproduces Epson's uploaded table **bit for bit** in
all four captures (2592, 7776, 15504 and 31008 entries). It also
reproduces all nine `1E 42` words and `1E 41` byte 15 (`0x81` @300,
`0x72` @600/1200). **[exact]** The device's own arithmetic is then
presumably `out = (raw − black_h − A·2^shA) · K / (B·2^shB + base_c)`.
Patching constant tables confirmed that the output is proportional to
1/T. K and the role of `1E 43` beyond the LED targets are not
separated yet.

**Hardware check:** `v39replay.py --shading-exact` replays Epson's
sequence, recomputes table, `1E 42` and `1E 41` bytes 11/15 from the
replay's own fresh dark/white passes, and scans. The result matches
the replay with Epson's captured values within 0.1–0.3% brightness per
colour/half. The mean pixel difference is 2.2–2.3 levels, which is the
replay-to-replay noise. **[confirmed on hardware]**

**Unit independence.** None of the above uses per-unit constants. All
inputs are measured on every scan (dark, white, AFE and LED readings),
and the constants (windows 896…1152 and 211…219, 4 white groups, 8-pixel
boxes) are fixed in Epson's model-wide interpreter.

### 7.5 Mode tables

Values per hardware resolution, collected from the captures in `ref/`.
A complete command sequence for each mode, with every parameter block,
is in `ref/<dpi>-<mode>-<pos>.txt` (see `ref/README.md`). Copy blocks from
there where this table says "see ref".

| | 300 | 600 | 1200 | 2400 | 4800 |
|---|---|---|---|---|---|
| W (`1E 41` end = 351 + W) | 2592 | 5168 | 10336 | 20672 | 41344 |
| `1E 21` | `0x210` | `0x480` | `0x900` | `0x1200` | `0x2400` |
| motor mode, calibration move (`Mcal`, 48 steps) | `0x10` | `0x0F` | `0x0E` | `0x0D` | `0x0C` |
| motor mode, image scan colour / gray | `0x09` / `0x0B` | `0x08` / `0x0A` | `0x05` / `0x06` | `0x03` / `0x04` | `0x01` / (`0x02`?) |
| start-position offset (steps, §7.1) | 65 (gray 64) | 65 | 174 | 174 | 174 |
| `1E 41` byte 9 (image) | 1 | 1 | 1 | 2 | 4 |
| LED start duty | 135 | 320 | 640 | 900 | 1800 |
| raw lines per output line colour / gray | 2 / 4 | 1 / 1 | 1 / 1 | 1 / 1 | 1 / – |

`1E 22` blocks (`u16 mode, u32 a[3], u32 b[3], u16 c[3], u16 duty[3], u16 flag`):

| dpi | mode 1 (calibration) a / b / c | mode 2 (scan) a / b / c |
|---|---|---|
| 300 | `D6 41E A90` / `2F3 965 A91` / `2A3`×3 | `D6`×3 / `A55`×3 / `2A3`×3 |
| 600 | `D6 874 1792` / `5D7 14F5 1793` / `640`×3 | `D6`×3 / `1757`×3 / `640`×3 |
| 1200 | `D6 FF4 2E12` / `AD7 28F5 2E13` / `C80`×3 | `D6`×3 / `2DD7`×3 / `C80`×3 |
| 2400 | `D6 160C 405A` / `EE7 3935 405B` / `1194`×3 | `D6`×3 / `401F`×3 / `1194`×3 |
| 4800 | `D6 2B24 7FA2` / `1CF7 7175 7FA3` / `2328`×3 | `D6`×3 / `7F67`×3 / `2328`×3 |

Mode 1 is used for the AFE/LED/reference passes. Mode 2 is used for
the LED exposure reads and the image scan, with `duty` = the calibrated
duties (`0,0,0` = LEDs off).

Final image-scan `1E 41`, bytes 8–17 and 18 (bytes 11 and 15 are the
computed shading values from §7.4; typical values shown):

| mode | bytes 8–17 | byte 18 (lines/transfer) |
|---|---|---|
| 300 colour | `01 01 02 80 80 00 00 81 80 A0` | 8 |
| 300 gray | `01 01 01 80 80 01 00 81 80 A0` | 25 |
| 600 colour | `01 01 02 80 80 00 00 72 80 A0` | 4 |
| 600 gray | `01 01 02 80 80 01 00 72 80 A0` | 12 |
| 1200 colour / gray | `01 01 01 80 80 0g 00 72 80 A0` (g = 0 colour, 1 gray) | 2 / 6 |
| 2400 colour / gray | `01 02 01 80 80 0g 00 72 80 A0` | 1 / 3 |
| 4800 colour | `01 04 01 80 80 00 00 72 80 A0` | 1 |

Lines per transfer = max(1, floor(65535 / bytes per raw line)) in every
capture. **[confirmed]** The AFE, LED, dark and white passes use their
own blocks (byte 8 = `00` for 16-bit). These follow the same pattern
with byte 9 per resolution. They are listed in order in each `ref/*.txt`.

`1E 57` (parameter record) for a scan: `u32 dpi, u32 vertical scan dpi
(dpi × raw-lines-per-output-line), u32 x, u32 y × that factor, u32 W,
u32 lines, u8 colour mode (0x13 colour / 0x00 gray), u8 depth (8),
…`. The rest is copied from the ESC/I `FS W` block. It looks like
bookkeeping only. A driver can send the same layout. **[inferred]**

## 8. Things that are still open

* Exact meaning of the `1E 22` timing fields, `1E 41` bytes 9, 10, 12,
  16, 17 (LED gating / plane layout), `1E 01` units, `1E 08`, `1E 87`,
  `1E 5A`, `1E A2`, and the transfer trailer.
* The device gain K, and whether `1E 43` does more than scale the LED
  targets.
* The exact rounding of the LED duty update.
* Dummy pixels at 4800 dpi, and 4800 dpi gray (not captured).
* In 16-bit output mode a final-geometry scan starts ≈1.2" further down
  the page than the same scan in 8-bit mode. Not understood.
* Behaviour on errors / cover open / carriage stuck.

### 8.1 Confirmed on Linux with our own code (2026-10-31)

* **Calibration cache.** `sane/epsonv39.c` keeps its own entry in the
  two type-`09` regions, tagged `V39S` and checksummed, so neither driver
  can mistake the other's entry for a hit. The key is the hardware
  resolution plus colour/gray. On a hit, one dark read checks the
  cached AFE offsets against the 896…1152 window. The LED search then
  starts from the cached duties and normally stops after its first read.
  Calibration time drops from 2.7 s to 1.7 s at 300 dpi and from 3.7 s to
  2.3 s at 1200 dpi. Output brightness matches a full calibration
  within 0.3 levels. The cache survives across processes and is lost
  on power-off, together with the firmware.

* Firmware upload (§4.2) works from Linux via libusb and takes about
  10 ms. The checksum byte is the plain sum of the image bytes.
* **Firmware boot.** After the final ACK of the upload, the device does
  not answer `1B 03` for about 3.9 s. It then reports `1A` and `5A`
  while it moves the carriage twice, and reaches `18` after about 10 s.
  Bit `0x02` seems to mean "initialising". A set or get command sent
  before `18` may never be ACKed. Abandoning it leaves the framing out of
  sync, and the device then NAKs all OUT traffic until it is replugged.
  Wait for `18` before sending anything other than `1B 03` and `1B 13`.
* A complete scan driven only by `sane/epsonv39.c` works at every
  resolution from 75 to 4800 dpi, in colour, gray and line-art.
  Calibration converges to values close to Epson's (AFE `0x86`; LED
  duties within a few percent).
* **The device does not answer commands other than `1B 03` while busy.**
  `1E 84` sent right after the white pass got no ACK. `1B 13` sent while
  the carriage returned after a scan timed out. Poll `1B 03` until bit
  `0x40` clears after every pass and before any command.
* **Wait for the positioning move.** Right after `1E 05`, `1B 03` still
  reads idle for about 30 ms before the move starts. At 1200 dpi and
  above, the move takes about 1.8 s. If the final setup (`1E 21` …
  `1E A2`) is sent during the move, every command is still ACKed. But
  the image pass then stalls about one plane-line before the end, or
  returns no data at all. Poll until bit `0x40` has been seen set and
  then cleared, or until 300 ms pass with no sign of motion.
* Cancelling with `1E 18` in the middle of the image pass works. Read
  and discard IN data until a 1 s timeout; the device is then idle and
  ready for the next scan. With `sanei_usb`, that final time-out also
  clears the halt on the IN endpoint; the device tolerates it, and the
  next scan works normally.
* If the host stops reading during the image pass for about a minute
  and the transfer is then abandoned, the device can wedge (OUT NAKed,
  no IN data) until it is replugged. Read the image without long pauses.
* A USB port reset on a wedged device made it fail to re-enumerate
  (`error -71`). Only a physical replug recovers it.

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
| 0 | u32 x-res | 300, 600, 1200, 2400, 4800 (100/150 → NAK) |
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

* `sane/`: the SANE backend built from this document (see `sane/README.md`).
* `tools/v39.py`: a prototype driver that speaks the device protocol
  directly, with no Epson code. It uses usbscan.sys via ctypes on
  Windows and pyusb on Linux. It covers firmware upload, status, memory,
  AFE, LED, scan setup and image reads.
* `tools/v39replay.py`: replays a captured session through `v39.py`. It
  can patch individual parameter bytes (`--patch I:K=V`), replace the
  shading table with a constant (`--patch-shading C`), recompute the
  table, `1E 42` and `1E 41` bytes 11/15 with the exact §7.4 algorithm
  from the replay's own reference passes (`--shading-exact`), and dump
  every image read. Run it with `uv run --with numpy`. It accepts both
  full captures and the stripped `ref/*.v39` files.
* `tools/v39seq.py`: turns a capture into a `ref/` pair: a readable
  `.txt` command sequence and a stripped `.v39` with image data removed.
* `ref/`: reference sequences for every mode (300–4800 dpi, colour and
  gray, several positions). See `ref/README.md`.
