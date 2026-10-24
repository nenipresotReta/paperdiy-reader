# Third-party notices

[English](THIRD_PARTY_NOTICES.md) · [简体中文](THIRD_PARTY_NOTICES.zh-CN.md)

- [M5Unified](https://github.com/m5stack/M5Unified), commit `e125e73f28b6b83e621e48df432af426bb2ff935`: MIT, © M5Stack. See `licenses/M5Unified-MIT.txt`.
- [M5GFX](https://github.com/m5stack/M5GFX), 0.2.33 (`347a23274081a105cb9fc0fc3bbf0e15c78e79c3`): MIT, © M5Stack; its bundled components retain their own notices. See `licenses/M5GFX-MIT.txt` and upstream source. EPUB decompression uses its miniz implementation (public-domain notices in the source).
- [MD4C](https://github.com/mity/md4c/tree/release-0.5.2), 0.5.2: MIT, © 2016–2024 Martin Mitáš. Only the parser is vendored in `lib/md4c/`; its license is [LICENSE.md](lib/md4c/LICENSE.md). Local source-range, checked-allocation, cooperative-work and allocation-failure fixes are documented with upstream checksums in [PATCHES.md](lib/md4c/PATCHES.md).
- [TinyXML2](https://github.com/leethomason/tinyxml2), 10.0.0: zlib, © Lee Thomason. Vendored source and license are in `lib/tinyxml2/`. The optional per-document progress observer is documented in [PAPERDIY_PATCH.md](lib/tinyxml2/PAPERDIY_PATCH.md).
- [Noto Serif CJK SC](https://github.com/notofonts/noto-cjk/tree/Serif2.003), 2.003: SIL Open Font License 1.1, © 2017–2024 Adobe. See `licenses/Noto-OFL.txt`. The committed `src/native_font_data.h` is a build-generated 24/28 px monochrome bitmap subset of this font; its internal name is `NativeFont` and it does not use the reserved name “Noto”. The original OTF is not committed; `tools/build_fonts.py` downloads it with a pinned SHA-256 only when regenerating.
- **PaperDIY JP** native vertical/ruby derivative of the same pinned Noto Serif CJK SC 2.003 source: SIL OFL 1.1, © 2017–2024 Adobe. [Provenance](licenses/PaperDIY-JP-source.txt) and [OFL](licenses/Noto-OFL.txt). `src/vertical_font_data.h` bakes Japanese `JAN` regional and vertical substitutions at native 24/28 px, with native 12/14 px kana/Latin ruby. It reuses identical body glyphs and uses the derivative name PaperDIY JP, not the reserved name Noto. Original source fonts are not committed; `tools/build_vertical_fonts.py` verifies the pinned source SHA-256.
- [Unicode Vertical_Orientation 17.0.0](https://www.unicode.org/Public/17.0.0/ucd/VerticalOrientation.txt): Unicode License v3. The generated `src/vertical_orientation_data.h` contains bounded orientation ranges derived from this pinned data. See [Unicode.txt](licenses/Unicode.txt) and [source provenance](licenses/PaperDIY-JP-source.txt).
- [STIX Two Math](https://github.com/stipub/stixfonts/tree/v2.13b171), 2.13 b171: SIL Open Font License 1.1, © 2001–2021 The STIX Fonts Project Authors. See [STIXTwoMath-OFL.txt](licenses/STIXTwoMath-OFL.txt) and [source provenance](licenses/STIXTwoMath-source.txt). The committed `src/math_font_data.h` contains the derivative **PaperDIY Math** monochrome bitmap subset at native 14/20/28 px, plus selected OpenType MATH metrics and variants. `tools/build_math_fonts.py` verifies the pinned OTF SHA-256 when regenerating; the original OTF is not committed. The font data remains OFL; the original renderer code is MIT.
- [Arduino-ESP32](https://github.com/espressif/arduino-esp32), supplied by pinned PlatformIO Espressif32 6.12.0: LGPL-2.1 and component-specific licenses. Framework sources and license notices are installed by PlatformIO. See `licenses/Arduino-ESP32-LGPL.txt` and `licenses/ESP-IDF-Apache.txt`; firmware releases include linked framework components under their original licenses. The matching application source, pinned dependencies, and rebuild instructions are available with each release; rebuild with modified LGPL components using the same PlatformIO workflow. Application source is MIT; this does not relicense dependencies.

British English hyphenation patterns by Dominik Wujastyk and Graham Toal are distributed under the MIT license. See [British-English-hyphenation.txt](licenses/British-English-hyphenation.txt) for the notice and pinned source.

Arduino-ESP32 2.0.17's `libraries/SD/src/sd_diskio.cpp` retains Espressif's
Apache-2.0 notice. A build-local correction ends `AcquireSPI` before freeing its
card and checks the drive index before access. [patch_sd_uninit.py](tools/patch_sd_uninit.py)
verifies original SHA-256 `0401e23009f06e3a418470a144403e89727f3d007b1d16d429dc370ce8bdc5f0`;
[pio_sd_uninit.py](tools/pio_sd_uninit.py) compiles a generated copy without
modifying the installed framework. An unrecognized SDK source fails the build.

The ESP32-S2/S3 TinyUSB device controller retains its MIT notice, © Scott
Shawcroft, William D. Jones/Adafruit, Ha Thach and Espressif. The unmodified
[vendored source](tools/vendor/arduino-esp32-2.0.17/dcd_esp32sx.c) comes from
[lib-builder commit `62d849ae49ac9b71c408bcc35039a5e487802806`](https://github.com/espressif/esp32-arduino-lib-builder/blob/62d849ae49ac9b71c408bcc35039a5e487802806/components/arduino_tinyusb/src/dcd_esp32sx.c),
SHA-256 `63fa1b596ce892a8e9e8e81934e48f7d0f92133b7968c0707a9a3cfc2c01e0a8`.
Its code and relocations match the bundled Arduino-ESP32 2.0.17 / ESP-IDF 4.4.7
DCD object. [patch_usb_dcd.py](tools/patch_usb_dcd.py) generates the local mask
synchronization/completion-ordering correction;
[pio_usb_dcd.py](tools/pio_usb_dcd.py) compiles it and substitutes one checked
build-directory archive without modifying the installed SDK. Unknown source,
archive or object identities stop the build. Original notices remain intact;
[provenance and changes](tools/vendor/arduino-esp32-2.0.17/README.md) describe the
bounded correction. Offline tests and linking do not establish the cause of an
observed physical USB failure.

The offline browser EPUB preparer includes JavaScript and WebAssembly runtime
components generated by [Emscripten 4.0.18](https://github.com/emscripten-core/emsdk/releases/tag/4.0.18),
using emsdk commit `eb5eb4b3b2b792f44ded198bfe615c985da5be2c`. Emscripten's
[MIT/NCSA notices](https://github.com/emscripten-core/emscripten/blob/4.0.18/LICENSE),
musl's MIT and component notices, and libc++/libc++abi/compiler-rt's Apache-2.0
with LLVM exceptions and retained component notices are reproduced in
[browser-preparation-runtime.txt](licenses/browser-preparation-runtime.txt).
The same pinned TinyXML2 and M5GFX miniz sources listed above are linked into
this module; their original licenses also apply. The generated resources embed
no books or personal data. The original adapter and SHA-256 implementation are
covered by this project's MIT license.
