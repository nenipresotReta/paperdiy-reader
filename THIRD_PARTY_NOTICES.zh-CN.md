# 第三方声明

[English](THIRD_PARTY_NOTICES.md) · [简体中文](THIRD_PARTY_NOTICES.zh-CN.md)

- [M5Unified](https://github.com/m5stack/M5Unified)，提交 `e125e73f28b6b83e621e48df432af426bb2ff935`：MIT，© M5Stack。见 `licenses/M5Unified-MIT.txt`。
- [M5GFX](https://github.com/m5stack/M5GFX)，0.2.33（`347a23274081a105cb9fc0fc3bbf0e15c78e79c3`）：MIT，© M5Stack。内含组件保留各自声明，见 `licenses/M5GFX-MIT.txt` 及上游源码。EPUB 解压使用其中的 miniz，其公有领域声明保留在源码中。
- [MD4C](https://github.com/mity/md4c/tree/release-0.5.2)，0.5.2：MIT，© 2016–2024 Martin Mitáš。仅解析器纳入 `lib/md4c/`；许可见 [LICENSE.md](lib/md4c/LICENSE.md)。源位置、分配检查、协作让出及分配失败清理的本地改动和上游校验值见 [PATCHES.md](lib/md4c/PATCHES.md)。
- [TinyXML2](https://github.com/leethomason/tinyxml2)，10.0.0：zlib，© Lee Thomason。源码和许可位于 `lib/tinyxml2/`。可选的文档级进度回调见 [PAPERDIY_PATCH.md](lib/tinyxml2/PAPERDIY_PATCH.md)。
- [Noto Serif CJK SC](https://github.com/notofonts/noto-cjk/tree/Serif2.003)，2.003：SIL Open Font License 1.1，© 2017–2024 Adobe。见 `licenses/Noto-OFL.txt`。`src/native_font_data.h` 是构建生成的 24/28 px 单色子集，内部名称为 `NativeFont`，不使用保留名称“Noto”。原始 OTF 不入库；只有重新生成时，`tools/build_fonts.py` 才下载并核对固定 SHA-256。
- **PaperDIY JP**：基于同一固定 Noto Serif CJK SC 2.003 源文件的竖排及 ruby 派生字库，SIL OFL 1.1，© 2017–2024 Adobe。见[来源](licenses/PaperDIY-JP-source.txt)及 [OFL](licenses/Noto-OFL.txt)。`src/vertical_font_data.h` 包含 24/28 px 日文 `JAN` 地域及竖排替换、12/14 px 假名与拉丁 ruby 字形；相同轮廓复用正文字形。派生名称为 PaperDIY JP。原始字体不入库，生成脚本核对固定 SHA-256。
- [Unicode Vertical_Orientation 17.0.0](https://www.unicode.org/Public/17.0.0/ucd/VerticalOrientation.txt)：Unicode License v3。`src/vertical_orientation_data.h` 的有界方向区间由此固定数据生成，见 [Unicode.txt](licenses/Unicode.txt) 及[来源](licenses/PaperDIY-JP-source.txt)。
- [STIX Two Math](https://github.com/stipub/stixfonts/tree/v2.13b171)，2.13 b171：SIL Open Font License 1.1，© 2001–2021 The STIX Fonts Project Authors。见 [OFL](licenses/STIXTwoMath-OFL.txt) 及[来源](licenses/STIXTwoMath-source.txt)。`src/math_font_data.h` 为派生字库 **PaperDIY Math** 的 14/20/28 px 单色子集，附部分 OpenType MATH 度量与变体。生成脚本核对原始 OTF 的 SHA-256；原始 OTF 不入库。字体数据仍为 OFL，原创排版器为 MIT。
- [Arduino-ESP32](https://github.com/espressif/arduino-esp32)：由固定 PlatformIO Espressif32 6.12.0 提供，适用 LGPL-2.1 及组件各自许可。PlatformIO 安装框架源码和声明；见 `licenses/Arduino-ESP32-LGPL.txt` 与 `licenses/ESP-IDF-Apache.txt`。固件发布中的框架组件保留原许可，配套应用源码、固定依赖和重建说明随发布提供。可按同一 PlatformIO 流程重新构建修改后的 LGPL 组件；应用的 MIT 许可不改变依赖许可。

Dominik Wujastyk 与 Graham Toal 的英式英语断词规则使用 MIT 许可。声明及固定来源见 [British-English-hyphenation.txt](licenses/British-English-hyphenation.txt)。

Arduino-ESP32 2.0.17 的 `libraries/SD/src/sd_diskio.cpp` 保留 Espressif 的 Apache-2.0 声明。构建时修正了 `AcquireSPI` 的生命周期，使其先析构再释放 card，并在访问前检查驱动器编号。[patch_sd_uninit.py](tools/patch_sd_uninit.py) 核对原文件 SHA-256 `0401e23009f06e3a418470a144403e89727f3d007b1d16d429dc370ce8bdc5f0`；[pio_sd_uninit.py](tools/pio_sd_uninit.py) 编译构建目录中的生成副本，不修改已安装框架。无法识别的 SDK 源码会使构建失败。

ESP32-S2/S3 TinyUSB 设备控制器保留 MIT 声明，© Scott Shawcroft、William D. Jones/Adafruit、Ha Thach 和 Espressif。未修改的[内置源文件](tools/vendor/arduino-esp32-2.0.17/dcd_esp32sx.c)来自 [lib-builder 提交 `62d849ae49ac9b71c408bcc35039a5e487802806`](https://github.com/espressif/esp32-arduino-lib-builder/blob/62d849ae49ac9b71c408bcc35039a5e487802806/components/arduino_tinyusb/src/dcd_esp32sx.c)，SHA-256 为 `63fa1b596ce892a8e9e8e81934e48f7d0f92133b7968c0707a9a3cfc2c01e0a8`。其代码及重定位与 Arduino-ESP32 2.0.17 / ESP-IDF 4.4.7 内置 DCD 对象一致。[patch_usb_dcd.py](tools/patch_usb_dcd.py)生成 mask 同步和完成顺序修正，[pio_usb_dcd.py](tools/pio_usb_dcd.py)编译并替换构建目录中的一份已校验 archive，不修改已安装 SDK。未知源/archive/对象身份会使构建失败。原声明完整保留，[来源与改动](tools/vendor/arduino-esp32-2.0.17/README.md)说明有界修正；离线测试和链接通过不能证明实际 USB 故障的原因。

离线浏览器 EPUB 预处理器包含 [Emscripten 4.0.18](https://github.com/emscripten-core/emsdk/releases/tag/4.0.18) 生成的 JavaScript 与 WebAssembly 运行时；emsdk 提交为 `eb5eb4b3b2b792f44ded198bfe615c985da5be2c`。Emscripten 的 [MIT/NCSA 声明](https://github.com/emscripten-core/emscripten/blob/4.0.18/LICENSE)、musl 的 MIT 及组件声明，以及 libc++/libc++abi/compiler-rt 的 Apache-2.0（含 LLVM exceptions）和保留声明位于 [browser-preparation-runtime.txt](licenses/browser-preparation-runtime.txt)。该模块链接上述固定版本 TinyXML2 和 M5GFX miniz，仍适用各自许可。生成资源不含书籍或个人数据；原创适配器和 SHA-256 实现适用本项目 MIT 许可。
