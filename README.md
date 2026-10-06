# img2jxl

English | [中文](README.zh.md)

A lossless multi-format image → JPEG XL converter with a CLI and a Qt GUI.
Just leaves the pixels alone and writes a `.jxl`.

Built on [libjxl](https://github.com/libjxl/libjxl) 0.12.0 (with vendored
brotli, highway and skcms) plus libpng, libjpeg-turbo, giflib, libwebp and
libtiff — every dependency is bundled, so the project builds offline.

## Highlights

- **Lossless, verified**: JPEG XL runs at distance 0. The self-test decodes
  every output back and compares it pixel-by-pixel against the source.
- **JPEG without generation loss**: JPEG inputs are transcoded at the
  DCT-coefficient level (`JxlEncoderAddJPEGFrame`) with reconstruction data
  embedded, so the original file can be restored byte-for-byte. Camera metadata
  (Exif, XMP, JUMBF) is written into **uncompressed** `Exif` / `xml ` / `jumb`
  boxes so ordinary viewers can read it.
- **Animated GIF → JXL animation**: delays, loop count, frame offsets,
  transparency and disposal semantics are carried over.
- **Bit-depth matching**: color model, bit depth and alpha follow the source —
  16-bit stays 16-bit, palettes stay palettes, and a JXL declaring 10/12 bits
  keeps that depth.
- **Automatic coding-tool selection**: Modular mode is on by default, so
  libjxl picks the predictors, palette transform, RCT and squeeze passes per
  image. Nothing to tune beyond the effort level.
- **Metadata preserved**: ICC, Exif, XMP, text chunks and the physical resolution
  (DPI) come across from the source — a converted photo still carries its camera
  data, a converted PNG keeps its DPI. `--no-metadata` strips it.
- **Batch-friendly**: folders are scanned recursively, files convert in
  parallel on all cores, and output timestamps match the input's.

## Supported formats

Output is always a `.jxl` file (lossless, distance 0). Encoded channel layouts
and depths below are the *declared* ones; palette and sub-byte sources are
expanded during encoding — losslessly, since the values are preserved exactly.

| Input | Output |
|---|---|
| BMP 1/4/8-bit (paletted) | 8-bit RGB(A), palette expanded |
| BMP 24-bit | 8-bit RGB |
| BMP 32-bit with alpha mask | 8-bit RGBA |
| BMP 32-bit, `BI_RGB`, no mask | 8-bit RGB — the 4th byte is undefined, not alpha |
| TGA 8-bit gray / 24-bit / 32-bit | GRAY / RGB / RGBA |
| TGA 16-bit | GRAY_ALPHA (15/16-bit truecolor is rejected, not misread) |
| PNM P1/P2/P4/P5 | GRAY; PBM stays 1-bit; maxval > 255 → 16-bit |
| PNM P3/P6 | RGB; maxval > 255 → 16-bit |
| ICO | largest entry, keeping its color type and depth; AND-mask transparency applied |
| JPEG | **bitstream transcode** (reconstruction data stored, EXIF kept as a readable, uncompressed `Exif` box); pixel re-encode as fallback |
| PNG | re-encoded natively: 1/2/4/8/16-bit, palette and tRNS all preserved |
| GIF | multi-frame → **JXL animation**; single frame → 8-bit RGB(A) with tRNS |
| QOI | 8-bit RGB / RGBA |
| WebP | 8-bit RGB / RGBA (lossy and lossless) |
| TIFF 1/2/4-bit gray | GRAY, expanded to 8-bit; MINISWHITE inverted to the 0=black convention |
| TIFF 8/16-bit gray | GRAY 8/16-bit |
| TIFF 8/16-bit gray + alpha | GRAY_ALPHA 8/16-bit |
| TIFF 8/16-bit RGB(+alpha) | RGB / RGBA 8/16-bit |
| TIFF 1/2/4/8-bit palette | 8-bit RGB, palette expanded |
| JXL | still images re-encoded with the current settings; 10/12-bit sources keep their declared depth (samples are never rescaled) |

TIFF support covers chunky (interleaved) unsigned-integer samples. Planar
layouts, CMYK and float formats are rejected with a clear message rather than
silently downconverted.

`--auto` (off by default) drops a fully-opaque alpha channel and collapses
pure-grayscale RGB to GRAY before encoding.

### Animation notes

- GIF delay centiseconds map 1:1 onto JPEG XL ticks (100 ticks/second), and
  `num_loops` carries over directly (GIF's 0 = infinite).
- Disposal-to-background is reproduced with zero-duration clear frames, which
  are invisible during playback.
- Disposal-to-previous is approximated by clearing the disposed area to
  transparency. Frame regions are stored pre-composited, so the visible content
  of every displayed frame is exact; only pixels that a restore-to-previous
  would reveal *outside* later frames' rectangles can differ from a player
  implementing the mode natively.

### Metadata

Source metadata is carried over by default, so a `.jxl` keeps what the original had:

- **ICC profile** — becomes the colour encoding of the codestream (byte-exact round-trip), instead of the plain sRGB tag.
- **Exif** — written as an **uncompressed** `Exif` box. (JPEG inputs go through libjxl's bitstream transcode, which writes that box itself.)
- **XMP** — written as an `xml ` box.
- **Text** — PNG `tEXt`/`iTXt`/`zTXt` chunks, TIFF description/software/artist/copyright/date and GIF comments are written into a private `jxtx` box; JPEG XL has no standard text-chunk home, and img2jxl reads this box back on re-encode.
- **Physical resolution (DPI)** — PNG `pHYs`, TIFF resolution and JFIF density are stored in an Exif resolution blob (`XResolution` / `YResolution` / `ResolutionUnit`), which is what viewers read for DPI and print size.

`--no-metadata` on the CLI, or unchecking *Keep metadata* in the GUI, writes a bare `.jxl` instead.

What each input can contribute:

| Source | Carried into the .jxl |
|---|---|
| JPEG | Exif, XMP and JUMBF, written by the bitstream transcode (cannot be stripped in this mode — libjxl needs them for byte-exact JPEG reconstruction) |
| PNG | `eXIf`, `iCCP`, `pHYs`, `tEXt`/`iTXt`/`zTXt` |
| TIFF | resolution, ICC, XMP, description / software / artist / copyright / date |
| WebP | `EXIF`, `ICCP` and `XMP ` chunks |
| GIF | comment extensions |
| JXL | boxes and ICC are read back and re-emitted, so re-encoding a `.jxl` is metadata-preserving |
| BMP, TGA, PNM, QOI | nothing to carry (ICO inherits whatever its embedded PNG holds) |

## Building

### Requirements

- CMake ≥ 3.21
- A C compiler and a C++17 compiler for libjxl — MinGW-w64 gcc is the tested
  Windows toolchain; gcc/clang work on Linux, clang on macOS
- Qt 6, only for the GUI. Without Qt the CLI builds on its own.
- No downloads and no manual dependency installs: libjxl, brotli, highway,
  skcms, zlib, libpng, libjpeg-turbo, giflib, qoi, libwebp and libtiff are all
  vendored under `thirdparty/`.

Linux packages: `cmake ninja-build g++ qt6-base-dev`; macOS: `brew install cmake ninja qt`.

### Build

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="<Qt6 prefix>"
cmake --build build
```

This produces `build/img2jxl` (CLI), `build/img2jxl-gui` (GUI) and
`build/img2jxl_selftest` (tests). `CMAKE_PREFIX_PATH` is only needed for the
GUI; drop it to build the CLI alone.

### Using your own dependency copies

Resolution order per library is: vendored `thirdparty/` → local override
flags → online download via FetchContent (`-DIMG2JXL_FETCH_DEPS=OFF` disables
the last step and turns a missing dependency into a hard error).

```bash
cmake -S . -B build -G Ninja -DIMG2JXL_GUI=OFF \
  -DLIBJXL_SOURCE_DIR=<libjxl tree> \
  -DLIBPNG_SOURCE_DIR=<libpng tree> \
  -DZLIB_SOURCE_DIR=<zlib tree> \
  -DLIBJPEG_PREBUILT_DIR=<libjpeg-turbo install dir, include/ + lib/>
```

### MinGW notes

Two adjustments are made automatically for MinGW builds and are worth knowing
about if you touch the build:

- `JPEGXL_ENABLE_HWY_AVX2=OFF` — gcc on Windows miscompiles highway's AVX2
  code paths (the decoder crashes in its AVX2 output stage); SSE4 stays on.
- Executables reserve a 16 MB stack, because Modular mode at high effort
  recurses deeply.

## CLI

```
img2jxl [-o out.jxl|outdir] [-e 1-10] [--no-modular] [-j N] [--auto]
        [--no-metadata] [--no-keep-time] file-or-folder...
```

| Option | Meaning | Default |
|---|---|---|
| `-o` | a single `.jxl` names the output exactly; anything else is treated as an output directory (created if missing) | write a same-name `.jxl` next to each input |
| `-e 1-10` | encode effort, 1 = lightning … 10 = glacier | 10 |
| `--no-modular` | let the encoder choose VarDCT/Modular per frame | Modular enforced |
| `-j N` | parallel worker threads | logical CPU count |
| `--auto` | optimization mode (drop useless alpha, detect grayscale) | off |
| `--no-metadata` | do not carry source EXIF/ICC/DPI/text into the .jxl | metadata is kept |
| `--no-keep-time` | don't copy timestamps from the input | timestamps kept |

Behaviour worth remembering:

- Inputs may be files or folders; folders are scanned recursively.
- Without `-o`, non-JXL files get a same-name `.jxl` beside them, and `.jxl`
  inputs are re-encoded **in place** (timestamps preserved).
- Output files inherit the input's creation and modification times.
- With `-o outdir` the output is flattened by file name, so identically-named
  inputs from different subdirectories overwrite each other.

```bash
img2jxl photo.jpg                     # -> photo.jxl, restorable to the original JPEG
img2jxl my-pictures                   # whole folder, recursively
img2jxl -o outdir folder *.bmp *.jpg  # everything into outdir
img2jxl -e 7 --no-modular a.ppm       # faster encode, encoder picks the mode
```

## GUI

`img2jxl-gui` (GUI subsystem — no console window; the Qt6 Core/Gui/Widgets
libraries must be on `PATH`):

- Drag files or folders onto the window, or use **Add files…** / **Add folder…**
  (folders are scanned recursively, duplicates are skipped).
- Effort slider 1–10 (default 10), **Modular mode** checkbox (default on), and
  a thread count.
- **Keep metadata** (default on) carries ICC/EXIF/DPI/text; uncheck for a bare file.
- **Match source bit depth strictly** (default on) — unchecking enables the
  `--auto` optimizations. **Keep timestamps** is on by default.
- **Overwrite originals** (default off). Unchecked, a job whose output would
  land on its own input is skipped with a notice. Checked, in-place
  recompression is allowed, and a non-JXL source converted into its own folder
  is deleted after a successful conversion and replaced by the same-name
  `.jxl` (noted in the log). Outputs going elsewhere never delete anything.
- The output-directory box accepts dropped folders (or a dropped file, which
  fills in its parent directory).
- The interface language switches between 中文 and English in the top right,
  applies immediately and is remembered (Chinese by default).
- **Convert** streams each file's result into the log.

## Tests

```bash
./build/img2jxl_selftest             # full suite, 138 checks
./build/img2jxl_selftest some.jxl    # just verify a file decodes
```

The suite generates sample inputs for every format, converts them, decodes the
results back and compares pixels. It covers per-format decode correctness,
bit-depth and color matching, JPEG byte-exact reconstruction, animation
composition/durations/loops, effort levels, timestamp preservation, gray+tRNS
and gray+alpha depth handling, and 12-bit JXL depth preservation.

## Design notes

- **Lossless by construction** — the codestream distance is always 0; effort
  trades only time against file size.
- **Predictors and transforms** are not configurable because they don't need to
  be: Modular encoding tries palette, RCT and squeeze transforms plus multiple
  predictors per group and keeps what wins. That is a property of the format,
  not a setting.
- **Palette sources** are expanded before encoding. Modular's automatic palette
  transform generally recovers the savings, so the output is usually no larger
  than the palette-indexed original.
- **JPEG sources** keep their DCT coefficients; libjxl stores the information
  needed to rebuild the original file, so a `img2jxl photo.jpg` is a
  re-compression rather than a re-encode. The Exif/XMP/JUMBF boxes are left
  uncompressed on purpose: libjxl's default brotli-wraps them into `brob`
  boxes, which most viewers don't decompress, making the metadata look lost.
- **No GPU path** — converting many images in parallel already saturates the
  CPU, and a GPU entropy coder would not pay off on single files.

## Known limitations

- 16-bit BMP input is unsupported.
- `-o outdir` flattens the output (see above).
- Animated `.jxl` inputs are not re-encoded; they're skipped with a notice.
- GIF disposal-to-previous is approximated (see *Animation notes*).
- Sub-byte sources (1/2/4-bit gray or palette) are expanded to 8 bits on
  output. Values are preserved exactly, but the declared depth becomes 8.
- Associated (premultiplied) alpha in TIFF is carried as-is, not
  un-premultiplied.
- On the JPEG bitstream-transcode path the metadata boxes cannot be stripped
  (`--no-metadata` still leaves them): libjxl requires Exif and XMP to stay for
  byte-exact JPEG reconstruction.
- A TIFF's Exif IFD is not serialized into the Exif box; the resolution, ICC,
  XMP and descriptive tags are carried.
- Metadata inside brotli-wrapped `brob` boxes written by other tools is skipped
  when re-encoding a `.jxl` (img2jxl itself always writes plain boxes).
- Text goes into a private `jxtx` box, which standard viewers ignore — the data
  is preserved for re-encoding, not displayed.
- Windows paths go through the ANSI code page; Linux and macOS use UTF-8.
- Windows x64 is tested locally. The Linux and macOS jobs in
  `.github/workflows/build.yml` build and pass the self-test in CI, but haven't
  been run on real hardware yet.
- MSVC should work but is untested.

## Third-party components

| Component | Version | License | Notes |
|---|---|---|---|
| [libjxl](https://github.com/libjxl/libjxl) | 0.12.0 | BSD-3-Clause | `thirdparty/libjxl`; tools, benchmarks and extras disabled |
| [brotli](https://github.com/google/brotli) | 1.2.0 | MIT | `thirdparty/libjxl/third_party/brotli` |
| [highway](https://github.com/google/highway) | 1.4.0 | Apache-2.0 | `thirdparty/libjxl/third_party/highway`; AVX2 disabled on MinGW |
| [skcms](https://skia.googlesource.com/skcms) | Chromium snapshot | Apache-2.0 / BSD | `thirdparty/libjxl/third_party/skcms`; test ICC profiles removed |
| [zlib](https://github.com/madler/zlib) | 1.3.1 | zlib | `thirdparty/zlib` |
| [libpng](https://sourceforge.net/projects/libpng/) | 1.6.59 | PNG License (zlib-style) | `thirdparty/libpng`; only used to decode PNG input |
| [libjpeg-turbo](https://github.com/libjpeg-turbo/libjpeg-turbo) | 3.2.0 | IJG / BSD-3-Clause / zlib | `thirdparty/libjpeg-turbo`; pixel-fallback path and the self-test only |
| [giflib](https://giflib.sourceforge.io/) | 6.x subset | MIT | `thirdparty/giflib` |
| [QOI](https://phoboslab.org/qoi) | master | MIT | header-only, `thirdparty/qoi` |
| [libwebp](https://github.com/webmproject/libwebp) | 1.6.0 | BSD-3-Clause | `thirdparty/libwebp` |
| [libtiff](https://download.osgeo.org/libtiff/) | 4.7.2 | libtiff | `thirdparty/tiff`; optional codecs disabled |
| [Qt 6](https://www.qt.io/) | 6.x | LGPL-3.0 / GPL | optional, GUI only |

Vendored libraries are trimmed copies: documentation, test assets and unused
architecture-specific code are removed, and each keeps its original license
files.

## License

GPL-3.0-or-later — see [LICENSE](LICENSE).
