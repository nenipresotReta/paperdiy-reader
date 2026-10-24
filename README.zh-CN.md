# PaperDIY Reader

[English](README.md) · [简体中文](README.zh-CN.md)

M5Stack PaperDIY 的 EPUB、TXT 与 Markdown 阅读器。支持中英文排版、日文竖排、离线数学公式、独立阅读进度和渐进分页。

A/B 翻页；短按 A+B 打开或返回，长按两秒关机。书库双击翻整屏。

[安装与升级](docs/upgrading.zh-CN.md) · [操作说明](docs/usage.zh-CN.md) · [Markdown 与公式](docs/markdown.zh-CN.md) · [架构与实现](docs/architecture-and-implementation.zh-CN.md)

## 构建

Python 3.12+；ESP32-S3，16 MiB Flash、8 MiB OPI PSRAM。

```sh
python -m venv .venv
. .venv/bin/activate
pip install -r requirements-dev.txt
pio run
```

源码附带开源字库子集。测试方式见[开发文档](docs/development.zh-CN.md)，已发布功能见[版本说明](docs/releases/v0.3.0.zh-CN.md)。当前源码中的功能可能尚未发布。

公开版本不包含个人图片、书籍、阅读记录或 Wi-Fi 信息。

应用采用 [MIT](LICENSE)；字库与依赖保留[各自的许可](THIRD_PARTY_NOTICES.md)。
