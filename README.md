# PaperDIY Reader

M5Stack PaperDIY 的离线 EPUB / TXT 阅读器。

22 行书库 · 独立阅读进度 · 分页缓存 · 局部刷新 · 开机续读 · 空闲休眠

- **A / B**：上一本 / 下一本，上一页 / 下一页；长按连续移动，书库继续长按翻整屏。
- **A + B**：短按后松开，打开书籍 / 返回书库；同时按住 2 秒关机。
- FAT32 microSD：直接放入 EPUB 或 UTF-8 TXT。支持解包的 `.epub` 目录。

关机保留淡化书页，公开版不内置关机图片。运行时不使用 Wi-Fi 或蓝牙。

## 构建

Python 3.12+，ESP32-S3 / 16 MB Flash / 8 MB OPI PSRAM。

```sh
python -m venv .venv
. .venv/bin/activate
pip install -r requirements-dev.txt
python tools/build_fonts.py
pio run
pio run -t upload --upload-port PORT
```

首次打开会建立缓存。最多 256 本；单本 EPUB ≤64 MiB，提取正文 ≤4 MiB。
不支持 DRM、插图与复杂排版。字库为开放授权的思源宋体风格 Noto Serif CJK，覆盖 GB2312 / JIS X 0208 及常用西文，未覆盖字符显示占位符。

[版本](https://github.com/nenipresotReta/paperdiy-reader/releases) · [下载与升级](docs/upgrading.md) · [开发与测试](docs/development.md) · [MIT](LICENSE) · [第三方许可](THIRD_PARTY_NOTICES.md)
