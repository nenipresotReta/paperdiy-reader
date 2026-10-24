# PaperDIY Reader

[English](README.md) · [简体中文](README.zh-CN.md)

An EPUB, TXT and Markdown reader for M5Stack PaperDIY. English and CJK typography, Japanese vertical text, offline math, per-book progress and progressive pagination.

A/B turns pages. Press A+B to open or return; hold for two seconds to shut down. Double-click in the library to move one screen.

[Install](docs/upgrading.md) · [Use](docs/usage.md) · [Markdown and math](docs/markdown.md) · [Architecture and implementation](docs/architecture-and-implementation.md)

## Build

Python 3.12+; ESP32-S3 with 16 MiB flash and 8 MiB OPI PSRAM.

```sh
python -m venv .venv
. .venv/bin/activate
pip install -r requirements-dev.txt
pio run
```

Open font subsets are included. See [development](docs/development.md) for tests and [release notes](docs/releases/v0.3.0.md) for released behavior. Features in the working source may be newer than the latest release.

Public builds contain no personal images, books, reading records or Wi-Fi credentials.

[MIT](LICENSE); fonts and dependencies retain their [own licenses](THIRD_PARTY_NOTICES.md).
