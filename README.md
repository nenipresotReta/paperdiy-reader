# PaperDIY Reader

M5Stack PaperDIY 的离线 EPUB / TXT 阅读器。基于 ESP32-S3、540×960 电子纸和 A/B 两键，运行时不使用 Wi-Fi 或蓝牙。

当前为 **0.2.1-dev 测试快照**。

## 特性

- 离线阅读 EPUB / UTF-8 TXT，支持解包的 `.epub` 目录
- 书库首行 Settings，首屏 1 行 Settings + 21 本书，后续每页 22 本
- 独立阅读进度、分页缓存、局部刷新、空闲休眠
- 关机界面恢复：在阅读 / 书库 / Settings 关机，开机会回到原界面
- 24 / 28 px 中文位图字库，英文断字，中文禁则与行末标点处理
- 电池图标、低电量显示，无 Wi-Fi / 蓝牙后台
- Settings 中的 N 是网络功能占位，当前版本不实现 AP / Web UI

## 按键

| 场景 | A / 上 | B / 下 | A + B |
|---|---|---|---|
| 书库 | 上一本；双击切上一页；长按连续移动 | 下一本；双击切下一页；长按连续移动 | 打开选中项 |
| 阅读 | 上一页；长按连续翻页 | 下一页；长按连续翻页 | 返回书库 |
| Settings | 切换 N（网络功能占位） | 切换 R（180°） | 返回书库 |

说明：

- 书库单击有约 300ms 的等待，用来判断是否双击；双击只在同一按键、300ms 内生效。
- 长按约 600ms 后开始连续移动选中项；书库长按不再切页。
- Settings 页只有四个字形：`¬N`、`¬R`。
- **N 目前只是网络功能的占位，网络功能尚未添加：**
  - 不启动 AP / 热点
  - 不启动 Web UI / HTTP 服务
  - 不连接 Wi-Fi，不保存网络凭据
  - 切换 N 只改变 `¬` 前缀，重启后回到 `¬N`
- R 是 180° 显示方向，持久化；开启后 A / B 全局交换，A+B 和电源键不变。

## 关机与开机

关机时持久化当前界面：

- 阅读页：同一本书、同一保存页
- 书库：同一本书、同一页
- Settings：Settings 页面；N 回到 `¬N`，R 保持
- Error / Loading：回到书库

开机先显示原有的底部加载提示，再绘制目标界面。首次没有历史状态时进入书库，只高亮上次阅读的那本书，不自动打开。

关机图是关机时绘制的一帧；公开版不内置关机图片。需要私人关机图时，在仓库外提供 `PAPERDIY_SHUTDOWN_HEADER` 指向的头文件。

## 字体与开放授权

正文字库是 **Noto Serif CJK SC 2.003**，SIL Open Font License 1.1。

- 仓库提交的是 24 / 28 px 单色位图子集 `src/native_font_data.h`
- 子集内部名为 `NativeFont`，未使用保留字体名 `Noto`
- 原始 OTF 不提交；只有重新生成时才由 `tools/build_fonts.py` 按固定 SHA-256 下载
- 传统字补充表是固定码位集，不扫描书籍，不包含书名、正文或路径
- 许可证：`licenses/Noto-OFL.txt`，第三方说明见 `THIRD_PARTY_NOTICES.md`

## 隐私与发布

公开源码和公开固件不包含：

- 书籍、书摘、书签或阅读进度
- 设备 NVS / app 备份
- 私人关机图、效果图或截图
- 任何系统字体或私人字体

当前版本没有网络服务；N 只是界面占位，不会启动 Wi-Fi、AP、Web UI 或保存任何网络凭据。诊断模式和设备测试默认关闭；普通构建不启用 `PAPERDIY_DIAGNOSTICS`。

## 构建

要求：Python 3.12+，ESP32-S3，16 MB Flash，8 MB OPI PSRAM。

```sh
python -m venv .venv
. .venv/bin/activate
pip install -r requirements-dev.txt

# 可选：仅在需要重新生成字库时运行
# python tools/build_fonts.py

pio run
pio run -t upload --upload-port PORT
```

`src/native_font_data.h` 已提交，普通固件构建不需要下载字体。运行字库相关测试前需要先执行一次 `tools/build_fonts.py`，把原始 OTF 缓存到 `.cache/`。

## 测试

```sh
# 先生成一次 M5GFX 依赖
pio run

# 字体测试需要原始 OTF 缓存
python tools/build_fonts.py

cmake -S . -B build -DSANITIZE=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

当前测试套件 11 项全部通过，覆盖：

- 单击 / 双击 / 长按 / 阅读页输入 / Settings
- A+B、release guard、32 位 millis 回绕
- 书库选中、Settings 首行、第二页布局
- Settings 四字形、N 局刷、R 旋转补偿
- 阅读页悬挂标点、局部刷新、残影清除
- 缓存、书签迁移、NVS 失败、EPUB 解析、字体生成

## 升级

应用分区升级，只写 `0x10000`：

```sh
python -m esptool --chip esp32s3 --port PORT --baud 921600 \
  write-flash 0x10000 paperdiy-reader-app.bin
```

不要擦除整片 Flash，不要覆盖 NVS 或分区表。SD 卡中的书籍内容不会被改写；升级保留书籍、书签和阅读进度。公开固件不包含关机图片。

## 文档

- [开发与测试](docs/development.md)
- [下载与升级](docs/upgrading.md)
- [空闲功耗设计](docs/power.md)

## 限制

- 不支持 DRM、插图、复杂排版
- 最多 256 本；单本 EPUB ≤64 MiB；提取正文 ≤4 MiB
- 未覆盖字符显示占位符
- N 的网络功能（AP / Web UI）尚未实现，当前仅为占位开关
- 电子纸残影和功耗表现需要真机验证

## License

应用源码为 MIT，见 [LICENSE](LICENSE)。第三方组件、字体和框架许可见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

GitHub: <https://github.com/nenipresotReta/paperdiy-reader>
