# img2jxl

[English](README.md) | 中文

多格式图片 → JPEG XL 无损转换器，带 CLI 与 Qt 图形界面。它只做一件事：
原封不动地保留像素，写出 `.jxl`。

基于 [libjxl](https://github.com/libjxl/libjxl) 0.12.0（内置 brotli、highway、
skcms），以及 libpng、libjpeg-turbo、giflib、libwebp、libtiff。所有依赖都已
内置在 `thirdparty/`，完全离线即可构建。

## 特点

- **真无损，且经过校验**：JPEG XL 距离恒为 0。自测试会把每个输出解码回来，
  与源图逐像素比对。
- **JPEG 零代损**：JPEG 输入在 DCT 系数层面转码（`JxlEncoderAddJPEGFrame`），
  内嵌还原数据，原文件可逐字节还原。拍摄信息（Exif / XMP / JUMBF）写入**未压缩**的
  `Exif` / `xml ` / `jumb` 盒，普通看图软件可直接读取。
- **GIF 动画 → JXL 动画**：帧延时、循环次数、帧偏移、透明与 disposal 语义全部保留。
- **位深匹配**：颜色模型、位深与透明度跟随源图——16 位仍是 16 位，调色板仍是
  调色板，声明 10/12 位的 JXL 保留该深度。
- **编码工具自动选择**：默认开启 Modular 模式，libjxl 会按图像内容自动挑选
  预测器、调色板变换、RCT 与 squeeze 等变换。除了努力程度，无需调节任何参数。
- **保留元数据**：ICC、Exif、XMP、文本块与物理分辨率（DPI）都会随源文件带过来——转换后的照片仍保留拍摄信息，PNG 转换后仍保留 DPI。加 `--no-metadata` 可剥离。
- **适合批处理**：文件夹递归扫描，多文件并行转换吃满所有核心，输出时间戳与输入一致。

## 支持的格式

输出一律是 `.jxl` 文件（无损，距离 0）。下表列出的是**声明**的通道布局与位深；
调色板与子字节源在编码时会展开——这是按值无损的，因为样本值精确保留。

| 输入 | 输出 |
|---|---|
| BMP 1/4/8 位（调色板） | 8 位 RGB(A)，调色板展开 |
| BMP 24 位 | 8 位 RGB |
| BMP 32 位（有 alpha 掩码） | 8 位 RGBA |
| BMP 32 位（`BI_RGB`，无掩码） | 8 位 RGB —— 第 4 字节未定义，不当作 alpha |
| TGA 8 位灰度 / 24 位 / 32 位 | GRAY / RGB / RGBA |
| TGA 16 位 | GRAY_ALPHA（15/16 位真彩会被明确拒绝，不会误读） |
| PNM P1/P2/P4/P5 | GRAY；PBM 保持 1 位；maxval > 255 → 16 位 |
| PNM P3/P6 | RGB；maxval > 255 → 16 位 |
| ICO | 取最大条目，保留其颜色类型与位深；应用 AND 掩码透明 |
| JPEG | **位流转码**（内嵌还原数据，EXIF 以未压缩的 `Exif` 盒保存、可被普通软件读取）；失败时回退像素级重编码 |
| PNG | 原生重编码：1/2/4/8/16 位、调色板、tRNS 全部保留 |
| GIF | 多帧 → **JXL 动画**；单帧 → 8 位 RGB(A) + tRNS |
| QOI | 8 位 RGB / RGBA |
| WebP | 8 位 RGB / RGBA（有损与无损） |
| TIFF 1/2/4 位灰度 | GRAY，展开为 8 位；MINISWHITE 反相为 0=黑约定 |
| TIFF 8/16 位灰度 | GRAY 8/16 位 |
| TIFF 8/16 位灰度 + alpha | GRAY_ALPHA 8/16 位 |
| TIFF 8/16 位 RGB(+alpha) | RGB / RGBA 8/16 位 |
| TIFF 1/2/4/8 位调色板 | 8 位 RGB，调色板展开 |
| JXL | 静态图按当前设置重编码；10/12 位源保留其声明深度（样本绝不做重缩放） |

TIFF 支持 chunky（交错）无符号整数样本。平面（planar）布局、CMYK 与浮点格式会
给出明确报错，而不是被静默降级。

`--auto`（默认关闭）会在编码前去掉全不透明的 alpha 通道，并把纯灰度 RGB 折叠为 GRAY。

### 动画说明

- GIF 的厘秒延时 1:1 映射到 JPEG XL 的 tick（100 tick/秒），`num_loops` 直接
  沿用（GIF 的 0 = 无限循环）。
- 恢复背景（disposal-to-background）用零时长清除帧重现，播放时不可见。
- 恢复前帧（disposal-to-previous）为近似实现：把被释放区域清除为透明。帧区域以
  预合成方式存储，因此每个显示帧的可见内容完全精确；仅当"恢复前帧"揭示的像素
  落在后续帧矩形**之外**时，才可能与原生实现该模式的播放器有差异。

### 元数据

默认会携带源文件的元数据，让 `.jxl` 保留原文件的信息：

- **ICC 配置文件** —— 成为码流的颜色编码（逐字节往返），而不是简单地标记为 sRGB。
- **Exif** —— 写成**未压缩**的 `Exif` 盒。（JPEG 输入走 libjxl 的位流转码，由它自己写出该盒。）
- **XMP** —— 写成 `xml ` 盒。
- **文本** —— PNG 的 `tEXt`/`iTXt`/`zTXt`、TIFF 的描述/软件/作者/版权/日期、GIF 注释，统一写入私有 `jxtx` 盒；JPEG XL 没有标准的文本块承载方式，img2jxl 在再编码时会把它读回来。
- **物理分辨率（DPI）** —— PNG 的 `pHYs`、TIFF 分辨率、JFIF 密度会存进 Exif 分辨率标签（`XResolution` / `YResolution` / `ResolutionUnit`），这也是看图软件读取 DPI 与打印尺寸的依据。

命令行加 `--no-metadata`，或取消勾选 GUI 的「保留元数据」，则输出不含元数据的 `.jxl`。

各输入格式能贡献的内容：

| 源格式 | 写入 .jxl 的内容 |
|---|---|
| JPEG | Exif、XMP、JUMBF，由位流转码写出（该模式下无法剥离——libjxl 需要它们才能逐字节还原原 JPEG） |
| PNG | `eXIf`、`iCCP`、`pHYs`、`tEXt`/`iTXt`/`zTXt` |
| TIFF | 分辨率、ICC、XMP、描述/软件/作者/版权/日期 |
| WebP | `EXIF`、`ICCP`、`XMP ` 块 |
| GIF | 注释扩展 |
| JXL | 盒与 ICC 会被读回并重新写出，因此 `.jxl` 再编码不丢元数据 |
| BMP、TGA、PNM、QOI | 无元数据可带（ICO 继承其内嵌 PNG 的内容） |

## 构建

### 环境要求

- CMake ≥ 3.21
- 一个 C 编译器，以及 libjxl 需要的 C++17 编译器——Windows 上以 MinGW-w64 gcc
  为实测工具链；Linux 用 gcc/clang，macOS 用 clang
- Qt 6，仅 GUI 需要。没有 Qt 时会单独构建 CLI。
- 无需联网、无需手动安装依赖：libjxl、brotli、highway、skcms、zlib、libpng、
  libjpeg-turbo、giflib、qoi、libwebp、libtiff 全部内置在 `thirdparty/`。

Linux 包：`cmake ninja-build g++ qt6-base-dev`；macOS：`brew install cmake ninja qt`。

### 构建命令

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="<Qt6 前缀>"
cmake --build build
```

产物为 `build/img2jxl`（CLI）、`build/img2jxl-gui`（GUI）、`build/img2jxl_selftest`
（测试）。`CMAKE_PREFIX_PATH` 只有 GUI 需要，去掉它即只构建 CLI。

### 使用自己的依赖副本

每个库的解析顺序为：内置 `thirdparty/` → 本地覆盖参数 → 通过 FetchContent 在线
下载（`-DIMG2JXL_FETCH_DEPS=OFF` 关闭最后一步，缺依赖时直接报错）。

```bash
cmake -S . -B build -G Ninja -DIMG2JXL_GUI=OFF \
  -DLIBJXL_SOURCE_DIR=<libjxl 源码树> \
  -DLIBPNG_SOURCE_DIR=<libpng 源码树> \
  -DZLIB_SOURCE_DIR=<zlib 源码树> \
  -DLIBJPEG_PREBUILT_DIR=<libjpeg-turbo 安装目录，含 include/ 与 lib/>
```

### MinGW 注意事项

MinGW 构建会自动做两处调整，改动构建时值得留意：

- `JPEGXL_ENABLE_HWY_AVX2=OFF` —— Windows 上的 gcc 会误编译 highway 的 AVX2
  代码路径（解码器会在其 AVX2 输出阶段崩溃）；SSE4 保持启用。
- 可执行文件预留 16 MB 栈，因为高努力程度的 Modular 模式递归较深。

## CLI

```
img2jxl [-o out.jxl|outdir] [-e 1-10] [--no-modular] [-j N] [--auto]
        [--no-metadata] [--no-keep-time] 文件或文件夹...
```

| 选项 | 含义 | 默认 |
|---|---|---|
| `-o` | 单个 `.jxl` 即精确指定输出名；其他值视为输出目录（不存在则创建） | 在输入旁生成同名 `.jxl` |
| `-e 1-10` | 编码努力程度，1 = lightning … 10 = glacier | 10 |
| `--no-modular` | 由编码器逐帧选择 VarDCT/Modular | 强制 Modular |
| `-j N` | 并行工作线程数 | 逻辑 CPU 核数 |
| `--auto` | 优化模式（去无用 alpha、灰度检测） | 关 |
| `--no-metadata` | 不把源文件的 EXIF/ICC/DPI/文本写入 .jxl | 默认保留元数据 |
| `--no-keep-time` | 不从输入复制时间戳 | 保持时间戳 |

需要注意的行为：

- 输入可以是文件或文件夹；文件夹递归扫描。
- 不带 `-o` 时，非 JXL 文件在输入旁生成同名 `.jxl`；`.jxl` 输入**原地**重编码
  （时间戳保留）。
- 输出文件继承输入的创建与修改时间。
- `-o outdir` 时输出按文件名平铺，因此不同子目录下的同名输入会相互覆盖。

```bash
img2jxl photo.jpg                     # -> photo.jxl，可还原为原 JPEG
img2jxl my-pictures                   # 整个文件夹，递归
img2jxl -o outdir folder *.bmp *.jpg  # 全部输出到 outdir
img2jxl -e 7 --no-modular a.ppm       # 更快编码，模式交给编码器
```

## GUI

`img2jxl-gui`（GUI 子系统——无控制台黑框；需要 Qt6 Core/Gui/Widgets 库在 `PATH` 中）：

- 把文件或文件夹拖到窗口，或用 **添加文件…** / **添加文件夹…**（文件夹递归扫描，
  自动去重）。
- 努力程度滑条 1–10（默认 10）、**Modular 模式**复选框（默认开）、线程数。
- **保留元数据**（默认开）携带 ICC/EXIF/DPI/文本；取消勾选则输出不含元数据。
- **严格匹配源图位深**（默认开）——取消勾选即启用 `--auto` 优化。**时间戳与输入一致**
  默认开。
- **覆盖原文件**（默认关）。不勾选时，输出会落在自身输入上的任务会被跳过并提示。
  勾选后允许原地重压缩，且转换到自身目录的非 JXL 源文件会在转换成功后被删除，由
  同名 `.jxl` 取代（日志注明）。输出到其他目录时从不删除任何文件。
- 输出目录框支持拖入文件夹（拖入文件则填入其所在目录）。
- 界面语言在右上角切换中文/English，即时生效并记忆（默认中文）。
- 点击 **开始转换**，日志会实时输出每个文件的结果。

## 测试

```bash
./build/img2jxl_selftest             # 全部测试，138 项
./build/img2jxl_selftest some.jxl    # 仅校验某文件能否解码
```

测试套件会为每种格式生成样本输入、转换、再把结果解码回来逐像素比对。覆盖各格式
解码正确性、位深与颜色匹配、JPEG 逐字节还原、动画合成/时长/循环、努力程度、
时间戳保持、灰度+tRNS 与灰度+alpha 的深度处理，以及 12 位 JXL 深度保留。

## 设计说明

- **结构上保证无损**——码流距离恒为 0；努力程度只影响时间与体积的权衡。
- **预测器与变换不可配置，因为不需要**：Modular 编码会按组尝试调色板、RCT、
  squeeze 变换与多个预测器，保留最优者。这是格式本身的特性，不是设置项。
- **调色板源**在编码前展开。Modular 的自动调色板变换通常能找回压缩率，因此输出
  一般不会比调色板索引的原图更大。
- **JPEG 源**保留其 DCT 系数；libjxl 会存入重建原文件所需的信息，所以
  `img2jxl photo.jpg` 是重新压缩，而不是重新编码。Exif/XMP/JUMBF 盒刻意保持未压缩：
  libjxl 默认会把它 brotli 压进 `brob` 盒，而多数看图软件不解压这种盒，导致拍摄信息
  看起来丢失。
- **没有 GPU 路径**——批量并行转换已能吃满 CPU，而 GPU 熵编码器对单文件并不划算。

## 已知限制

- 不支持 16 位 BMP 输入。
- `-o outdir` 输出平铺（见上文）。
- 动图 `.jxl` 输入不重编码，会跳过并提示。
- GIF 恢复前帧为近似实现（见*动画说明*）。
- 子字节源（1/2/4 位灰度或调色板）输出时展开为 8 位。样本值精确保留，但声明
  深度变为 8。
- TIFF 的关联（预乘）alpha 原样保留，未做反预乘。
- JPEG 位流转码路径上的元数据盒无法剥离（`--no-metadata` 也会保留）：libjxl 要求
  Exif 与 XMP 必须在，才能逐字节还原原 JPEG。
- TIFF 的 Exif IFD 不会被序列化进 Exif 盒；分辨率、ICC、XMP 与描述性标签会带过去。
- 再编码 `.jxl` 时，其他工具写入的 brotli 压缩盒（`brob`）内的元数据会被跳过
  （img2jxl 自己始终写未压缩盒）。
- 文本写入私有 `jxtx` 盒，标准看图软件不显示——数据为再编码而保留，不作展示。
- Windows 路径经 ANSI 代码页处理；Linux 与 macOS 使用 UTF-8。
- Windows x64 已本地实测。`.github/workflows/build.yml` 中的 Linux 与 macOS 任务
  在 CI 中能构建并通过自测试，但尚未在真机验证。
- MSVC 理论可用，但未测试。

## 第三方组件

| 组件 | 版本 | 许可 | 说明 |
|---|---|---|---|
| [libjxl](https://github.com/libjxl/libjxl) | 0.12.0 | BSD-3-Clause | `thirdparty/libjxl`；已关闭 tools、基准测试与 extras |
| [brotli](https://github.com/google/brotli) | 1.2.0 | MIT | `thirdparty/libjxl/third_party/brotli` |
| [highway](https://github.com/google/highway) | 1.4.0 | Apache-2.0 | `thirdparty/libjxl/third_party/highway`；MinGW 下禁用 AVX2 |
| [skcms](https://skia.googlesource.com/skcms) | Chromium 快照 | Apache-2.0 / BSD | `thirdparty/libjxl/third_party/skcms`；已删测试 ICC profile |
| [zlib](https://github.com/madler/zlib) | 1.3.1 | zlib | `thirdparty/zlib` |
| [libpng](https://sourceforge.net/projects/libpng/) | 1.6.59 | PNG License (zlib-style) | `thirdparty/libpng`；仅用于解码 PNG 输入 |
| [libjpeg-turbo](https://github.com/libjpeg-turbo/libjpeg-turbo) | 3.2.0 | IJG / BSD-3-Clause / zlib | `thirdparty/libjpeg-turbo`；仅像素回退路径与自测试使用 |
| [giflib](https://giflib.sourceforge.io/) | 6.x 子集 | MIT | `thirdparty/giflib` |
| [QOI](https://phoboslab.org/qoi) | master | MIT | 单头文件，`thirdparty/qoi` |
| [libwebp](https://github.com/webmproject/libwebp) | 1.6.0 | BSD-3-Clause | `thirdparty/libwebp` |
| [libtiff](https://download.osgeo.org/libtiff/) | 4.7.2 | libtiff | `thirdparty/tiff`；可选编解码器已关闭 |
| [Qt 6](https://www.qt.io/) | 6.x | LGPL-3.0 / GPL | 可选，仅 GUI |

内置库均为裁剪副本：删去文档、测试素材与未使用的架构相关代码，各自保留原始许可文件。

## 许可

GPL-3.0-or-later —— 见 [LICENSE](LICENSE)。
