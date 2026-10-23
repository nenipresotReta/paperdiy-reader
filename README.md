# PaperDIY Reader

M5Stack PaperDIY 的 EPUB / TXT / Markdown 阅读器。中英文排版、离线数学公式、独立阅读进度与分页缓存。

A/B 翻页；短按 A+B 打开或返回，长按 2 秒关机。书库双击翻整屏。热点网页支持传书、配网、阅读设置与关机图片。

[下载与升级](docs/upgrading.md) · [操作说明](docs/usage.md) · [Markdown 与公式](docs/markdown.md) · [版本说明](docs/releases/v0.3.0.md)

## 构建

Python 3.12+；ESP32-S3，16 MiB Flash、8 MiB OPI PSRAM。

```sh
python -m venv .venv
. .venv/bin/activate
pip install -r requirements-dev.txt
pio run
```

字库采用 Noto Serif CJK 与 STIX Two Math 的开源子集（SIL OFL 1.1），已随源码提供。测试与架构见[开发文档](docs/development.md)。公开源码和固件不包含个人图片、书籍、阅读记录或 Wi-Fi 配置。

应用为 [MIT](LICENSE)；字库与依赖见[第三方许可](THIRD_PARTY_NOTICES.md)。
