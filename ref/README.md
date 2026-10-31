# Reference command sequences

These files were recorded on Windows by running Epson's interpreter
(`esxi010c.dll`) under `tools/esitrace` against a real Perfection V39
(firmware 1.42). Then `tools/v39seq.py` exported them. There is one pair
per scan:

* `<name>.txt` lists every device transaction in order. Parameter blocks
  are given in full and large payloads are summarised. The header shows
  the ESC/I `FS W` request (resolution, mode, area).
* `<name>.v39` is the binary capture with all device→host payloads over
  64 bytes (image data, reference passes) reduced to their length. It
  contains no scanned content. It still drives the scanner:

  ```
  uv run --with numpy tools/v39replay.py ref/300-color-y0.v39 --out scan.raw
  uv run --with numpy tools/v39replay.py ref/300-color-y0.v39 --out scan.raw --shading-exact
  ```

  The second form recomputes all calibration values for your unit
  (PROTOCOL.md §7.4).

The firmware must already be loaded (status `18 00`) before you replay;
see PROTOCOL.md §4.2.

| file | dpi | mode | FS W area (inches: x, y, w, h) | notes |
|---|---|---|---|---|
| 300-color-y0 | 300 | colour | 0, 0, 1, 0.2 | |
| 300-color-y1 | 300 | colour | 0, 1, 1, 0.2 | |
| 300-color-y2 | 300 | colour | 2, 2, 3, 1.5 | calibration-cache hit (no AFE search) |
| 300-color-y3 | 300 | colour | 0, 3, 1, 0.2 | |
| 300-color-y6 | 300 | colour | 0, 6, 1, 0.2 | |
| 300-color-h | 300 | colour | 0, 1, 1, 2 | height variation |
| 300-color-x | 300 | colour | 5, 1, 2, 0.2 | x variation (x is host-side only) |
| 300-gray-y0 | 300 | gray | 0, 0, 1, 0.2 | |
| 300-gray-y2 | 300 | gray | 2, 2, 3, 1.5 | full AFE + LED calibration |
| 300-gray-y3 | 300 | gray | 0, 3, 1, 0.2 | |
| 300-lineart-y2 | 300 | 1-bit | 2, 2, 2.99, 1.5 | device side identical to gray |
| 600-color-y0 | 600 | colour | 0, 0, 1, 0.2 | |
| 600-color-y2 | 600 | colour | 2, 2, 3, 0.5 | |
| 600-color-y3 | 600 | colour | 0, 3, 1, 0.2 | |
| 600-gray-y1 | 600 | gray | 0, 1, 1, 0.2 | |
| 1200-color-y0 | 1200 | colour | 0, 0, 1, 0.1 | |
| 1200-color-y2 | 1200 | colour | 2, 2, 1, 0.2 | |
| 1200-color-y3 | 1200 | colour | 0, 3, 1, 0.1 | |
| 1200-color-mid | 1200 | colour | 4.08, 1, 0.5, 0.1 | crosses the sensor centre |
| 1200-gray-y1 | 1200 | gray | 0, 1, 1, 0.1 | |
| 2400-color-y1 | 2400 | colour | 2, 1, 0.5, 0.05 | |
| 2400-color-y3 | 2400 | colour | 2, 3, 0.5, 0.05 | |
| 2400-color-mid | 2400 | colour | 4.21, 1, 0.25, 0.05 | crosses the sensor centre |
| 2400-gray-y1 | 2400 | gray | 2, 1, 0.5, 0.05 | |
| 4800-color-y1 | 4800 | colour | 2, 1, 0.125, 0.025 | |

Most sequences start right after Epson's `ESC @` init (identity gamma
LUTs, `1E E4`, `1B 40`) and include the full calibration. Where the
interpreter found a matching calibration cache in device RAM, the AFE
search is skipped; `300-color-y2` is the clearest example.
