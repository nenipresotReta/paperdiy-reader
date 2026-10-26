# 下载与升级

[English](upgrading.md) | [简体中文](upgrading.zh-CN.md)

发布固件面向 **M5Stack PaperDIY**：ESP32-S3、16 MiB Flash、8 MiB OPI PSRAM。下载对应 [GitHub release](https://github.com/nenipresotReta/paperdiy-reader/releases) 附件。归档只包含公开应用固件、校验和、说明与许可证，不含关机图、书籍、书签、设备备份或诊断命令。

## 升级已有阅读器

`*-app.bin` 是**仅应用分区升级**，适用于已使用仓库 [`partitions.csv`](../partitions.csv) 且未改分区表的阅读器。原厂固件或其他分区布局请按 README 从源码构建。

1. 解压并运行 `shasum -a 256 -c SHA256SUMS` 校验（Linux 使用 `sha256sum -c SHA256SUMS`）。
2. USB 连接阅读器，在 Python 环境安装 `esptool==5.6.0`；自动复位无法连接时进入下载模式。
3. 确认属于本阅读器的串口，只写应用分区：

```sh
python -m esptool --chip esp32s3 --port PORT --baud 460800 \
  write-flash 0x10000 paperdiy-reader-v0.3.0-app.bin
```

不要擦除整片 Flash 或覆盖 NVS。保留 SD 书籍原路径。升级保留书籍及逐书进度，并在提取或版式变化时迁移受支持旧缓存中的位置。修正分页后页码可能变化，恢复依据是对应文本位置。首次打开可能重建缓存；SD 原书不被改写。

运行时串口（原生 USB CDC）和 ROM 下载器是两个不同的 USB 设备。以 1200 波特率打开运行时串口即可进入下载器，不需要 BOOT 键。每次复位后重新辨认设备，不复用无关串口，也不要复位主机 USB 总线。原生 CDC 终端需同时置位 DTR、RTS。已验证的恢复流程见[开发文档](development.zh-CN.md)。

## 重新构建

使用同一标签的源码归档和 README 构建命令。源码包含已生成的 OFL 点阵字库子集。PlatformIO、框架/库版本与源字体均固定；重新生成时检查固定字体校验和，不使用系统字体或个人文件。第三方源码、许可及版本见 [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.zh-CN.md)。
