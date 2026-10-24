# Download and upgrade

[English](upgrading.md) | [简体中文](upgrading.zh-CN.md)

Releases target **M5Stack PaperDIY**, ESP32-S3, 16 MiB flash and 8 MiB OPI PSRAM.
Use the firmware archive attached to the matching [GitHub release](https://github.com/nenipresotReta/paperdiy-reader/releases).
It contains only the public application binary, checksums, instructions and licenses.
The public build has no shutdown artwork, books, bookmarks, device backup or diagnostic commands.

## Upgrade an existing reader

The `*-app.bin` is an **application-only upgrade**, for this reader with the
unchanged partition table in [`partitions.csv`](../partitions.csv).
For stock firmware or a different partition layout, follow the source-build
instructions in the README instead.

1. Unzip the release archive and verify its contents with `shasum -a 256 -c SHA256SUMS`
   (Linux: `sha256sum -c SHA256SUMS`).
2. Connect the reader by USB. Install `esptool==5.6.0` in your Python environment and put
   the board into download mode if automatic reset cannot connect.
3. Select this reader's serial port and write only its application partition:

```sh
python -m esptool --chip esp32s3 --port PORT --baud 460800 \
  write-flash 0x10000 paperdiy-reader-v0.3.0-app.bin
```

Do not erase flash or overwrite NVS. Keep the SD card's book paths unchanged.
The upgrade retains books and per-book progress, and migrates supported old
cache positions when text extraction or layout changes. A page number may change
when pagination is corrected; the reader preserves the corresponding text location.
The first open can rebuild caches. SD book contents are never rewritten.

Native USB storage builds use a different runtime serial port
from the ROM downloader. Safely eject the SD volume before entering download
mode. SDK serial reset shortcuts are disabled while the computer owns the card.
Reidentify the reader after reset; do not reuse an unrelated serial port or
reset the host USB bus. Native CDC terminal access requires DTR and RTS asserted.
See [development](development.md) for the tested native-USB recovery procedure.

## Rebuild

Use the source archive for the same tag and the README build commands.
The source archive includes the generated OFL bitmap font subset. PlatformIO,
the framework/library revisions and the font source are pinned. Regenerating
the subset uses a fixed source checksum; no installed system font or personal
file is used.
Third-party source, licenses and revisions are listed in
[THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).
