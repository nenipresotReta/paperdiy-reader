# 开发指南

[English](development.md)

本指南记录开发约定和命令。[架构报告](architecture-and-implementation.zh-CN.md)集中维护模块索引和当前证据表；编译成功、主机正确性、设备验收和发布是不同的结果。

## 测试主机隔离

设备开发和刷写不授权修改主机网络。禁止切换 Wi-Fi、连接设备热点、修改 DHCP/静态 IP、DNS、网关、服务顺序或首选网络；清理函数也不能以“恢复”为由修改这些配置。设备检查使用 USB，离线测试或 HTTP/浏览器夹具只能显式绑定 `127.0.0.1` 的临时端口。不得停止或重新配置无关服务或进程。若本地检查无法覆盖手机/AP 实际传输，应明确标为未测试；本地 Worker 一致性不能证明无线传输成功。

## 构建与依赖

使用 Python 3.12+ 和锁定的开发依赖。目标是 ESP32-S3，配备 16 MiB flash、8 MiB OPI PSRAM，保持自定义分区表不变。PlatformIO 锁定 `espressif32@6.12.0`、Arduino-ESP32 2.0.17 / ESP-IDF 4.4.7、M5GFX `347a23274081a105cb9fc0fc3bbf0e15c78e79c3` 和 M5Unified `e125e73f28b6b83e621e48df432af426bb2ff935`。固件必须使用 GNU++11 编译，不能依赖异常恢复；主机测试使用 C++17。主机测试通过不能证明 Xtensa 编译兼容。

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r requirements-dev.txt
pio run
cmake -S . -B build -DSANITIZE=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

主机测试需要 CMake、OpenSSL 开发头文件和 Node.js 18+。先由 PlatformIO 安装 M5GFX/miniz；已有锁定版本可通过 `-DM5GFX_DIR=/path/to/M5GFX` 指定。Homebrew macOS 可能还需要 `-DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"`。仅用于主机的测试使用可移植 miniz 字节读取路径，不会暗中修改锁定依赖。计时应另建 `-DSANITIZE=OFF` 目录。

公开默认配置使用原生 USB MODE0，启动启用 CDC、不启用 MSC；应用仅通过 U 开放 MSC。公开构建不包含 `PAPERDIY_DIAGNOSTICS`、`PAPERDIY_EPUB_TESTS`、`PAPERDIY_DEVICE_TESTS`、`PAPERDIY_NETWORK_TESTS` 或外部私人图片。私人配置必须留在公开源码之外，不能将私人镜像改名冒充公开固件。当前大小、哈希和硬件状态以架构证据表为准，不应固化在容易过期的构建命令中。

## 所有权与源码坐标

`app` 管理场景和生命周期，`input` 管理按键时序，`ui` 绘制场景，`reader` 管理当前文档、精确锚点和页面状态。SD、目录、设置、NVS 和传输写入在应用任务上串行执行。协作式任务不允许并发访问 FAT。TinyUSB MSC 回调运行在 SDK 任务中，使用独立且有界的所有权/互斥边界。

`document_format.h` 统一限制：TXT 4 MiB、EPUB 源文件 64 MiB、Markdown 1 MiB；扩展名匹配不区分大小写。EPUB 提取文本最多 4 MiB。阅读渲染不会重写源文件或持久化偏好。

Markdown 使用原始 UTF-8 源偏移，包括开头 BOM。生成的列表标记、实体和数学原子具有单调的源码/渲染映射。EPUB 书签指向保持不变的规范提取字节；语义旁车不能向该身份中插入展示字节。恢复时保留精确锚点，即使其所在页起点更早；只有实际翻页才采用其他页起点。必要恢复数据无效时必须明确失败，不能跳到第零页或覆盖未保存进度。

`resumeV2` 原子保存场景、稳定设置 ID 和有界选中路径。没有 V2 时可以读取 V1；V2 损坏不能复活旧 V1。键与线格式版本必须匹配，旧字段只读。破坏性导航、关闭无线或关机前，必须先成功保存并回读。

## EPUB 提取与章节恢复

`epub::Importer::begin()` 验证初始化/包数据并建立稳定的主章节/辅助章节计划。`stepChapter()` 每次处理一个完整章节；`run()` 是同一路径的同步封装，保留原有规范字节顺序和文本归一化规则。调用之间不会挂起或序列化 XML/deflate 状态。

`sd_open_job.h` 中的 `sd_open::Job` 管理冷启动 EPUB 打开过程。仍需完整计算源哈希，单章仍需有界压缩输入/DOM 分配。字节块、解析器和树遍历观察回调用于协作调度和锁存输入。取消只在当前章节及必要检查点提交到达安全边界后成为终态。这些回调不能抢占阻塞文件系统调用，不能让完整源哈希成为常数时间，也不是可恢复的流式 XML/deflate 解析。

提取恢复使用独立文件：

| 产物 | 约定 |
| --- | --- |
| `.exttext` | 最多 4 MiB 的规范文本前缀；只写未提交后缀 |
| `.ext0` / `.ext1` | 两个独立校验的 `PXE1` 提交；各有 192 字节头和最多 1 MiB 元数据 |
| 提交内的 `ECS1` | 显式可移植 Collector 状态，不是原生结构体或展平的 PDS |

每本书的有界产物合计最多 6 MiB 加 384 字节头。ECS1 头为 96 字节；编解码上限为 525,536 字节，默认 Collector 容量全部用满时最多 398,560 字节。它保存记录/父关系、资源/路径、语言表、数学/TeX 池、计数器和状态；ruby 与表格关系保存在记录树中。非空章节关闭时至少以两个规范换行结尾，封住已提交文本前缀。最终 `finish()` 可能裁剪原始语义边界，因此每个提交保存完整 Collector 快照，不能只追加记录尾部。

提交绑定完整源/依赖证明、带长度边界的章节计划、提取器、文本/语义版本、章节游标、前缀长度/摘要和元数据。重启会重新验证源和所有已读目录依赖，包括可选资源缺失标记。大小/mtime 相同不足以证明一致。损坏、撕裂或较新的无效槽可回退到较旧的已校验提交。若没有可用提交，追加前必须重新建立包/标题和 Collector 初始状态；失败校验可能已覆盖暂存字节。

只有关闭的章节才能成为检查点。当前调度在第一个非空前缀后保存，此后要求新增至少 256 KiB 或 64 章，且距上次成功保存至少一秒；取消和完成会请求提交。每次 poll 的传输/回读最多 8 KiB。打开、flush、rename 和单次解析器/SDK 调用仍同步执行。

分配失败回退和整次导入的纯文本重试仅用于当前会话，不能覆盖更完整的语义缓存，也不能持久化为提取成功。确定性的预算回退保留显式状态。未完成前缀不会发布普通书籍身份、书签键或 NVS 进度。启用日志时，完整提取先提交并验证，再进入 `indexBook()` 和正常阅读发布。可选日志失败可允许完整 RAM 导入，但明确标为易失结果。每次 poll 和最终发布独立检查同一个非零 opening lease。

## 语义排版与字体

Collector 使用一次经检查的可选分配。失败会丢弃全部可观察旁车记录，纯文本提取仍必须完整。渲染器在相同规范字节上使用 PDS1/PDS2/PDS3。CSS、竖排/ruby/TCY、MathML/表格边界、回退及量化日文字体审计见 [EPUB 排版](epub-typesetting.zh-CN.md)。

Markdown 使用内置 MD4C 0.5.2；分配、源范围、让出和失败清理改动记录在 [PATCHES.md](../lib/md4c/PATCHES.md)。限制为源和渲染文本各 1 MiB、4,096 个 run、8,192 个映射和 512 KiB 解析预算。解析失败展示完整原始源码。

数学渲染支持有界 TeX 风格语法，不执行 TeX：最多 4,096 源字节、256 节点、深度 16、1,024 命令和 8 × 8 矩阵。准入预留 128 KiB 工作区和八个固定命令槽（目标上另约 128 KiB）。这些槽是按源范围替换的有界环，不是 LRU；准入后的重算不再分配。成功准入的公式仅消费一次完整源范围；独立公式单占一行。不支持语法、缺字或几何/资源失败时保留完整源码回退。

所有字形均为原生位图，不在运行时缩放：

| 头文件 | 来源 / 尺寸 |
| --- | --- |
| `native_font_data.h` | Noto Serif CJK SC 2.003；24/28 px，派生字体 NativeFont |
| `vertical_font_data.h` | 同一锁定源的 JAN/竖排字形 24/28 px，以及 ruby 12/14 px；派生字体 PaperDIY JP |
| `math_font_data.h` | STIX Two Math 2.13 b171；14/20/28 px 和选定 OpenType MATH 度量；派生字体 PaperDIY Math |

朝向数据来自 Unicode 17.0.0，按 Unicode License v3 使用。再生工具为 `tools/build_fonts.py`、`build_vertical_fonts.py`、`build_math_fonts.py`，使用固定源哈希，不使用已安装或私人字体。记录的 JP 派生再生环境为 Python 3.14.8、Pillow 12.2.0、FreeType 2.14.3、fontTools 4.62.1；实际生成环境应与开发依赖中的 Pillow 13.0.0 锁定分开记录。被忽略的固定源缓存不存在时，不得声称精确再生测试通过。

## 缓存、分页与信任

排版 family 为 `0x2026a000`；低十一位绑定相关几何、模式、语言和语义存在性。`TEXT_REVISION=1` 保持不变；EPUB 要求 `semanticRevision=2`。旧语义和中间 `0x20268000` 排版只提供已校验迁移锚点，不能直接复用页面或缓存文本重排。重新导入恢复缺失语义，规范文本不变则保留进度。TXT/Markdown 保留经审计的文本 family 复用。

完整缓存当前使用版本 3。暴露状态前验证源/依赖、长度、摘要、UTF-8、语义边界和严格递增页表。同大小/mtime 的源也可能字节不同。两个各 64 项的易失环只在独占存储 epoch 内记忆完整指纹和依赖图；重新挂载、源变更、I/O 错误、进入休眠和 USB 交接都会撤销。重启必须重新验证。

EPUB 首先测量精确保存锚点附近的页面。本地窗口最多 32 个页起点；回溯最多测量 16 页，从前移 4 KiB 的种子开始，再应用相同的有界词/UTF-8 边界调整。规范索引每次应用循环最多推进两页，软预算 12 ms；单页仍可能超过该预算。完成只更新页脚，不移动可见锚点。TXT/Markdown 仍同步索引。

部分分页使用 `.idx0`/`.idx1`，两者合计最多 524,624 字节，身份绑定排版、完整文本/语义和源/依赖。保存从 32 页开始，此后要求新增 128 页且经过五秒。恢复不能移动已显示锚点或虚构全书总页数。这些是页索引检查点，与章节提取提交分开。

`DeferredCacheWrite` 在完整索引和已校验进度保存后启动。它快照元数据/页表，借用稳定文本，每步最多读/写/验证 8 KiB。每步检查指针、长度、键、排版和 lease。只有验证后的临时文件可替换主文件，并保留已检查 `.bak`。Arduino `flush()`/FAT rename 不能证明断电耐久性。

可选 `.chunks` 清单绑定完整缓存前缀及每个 8 KiB 文本块的 SHA-256；最多 512 块、16,504 清单字节。缺失、过期或损坏时回退完整验证。仍预留 4 MiB 地址空间并保持全局偏移。固定窗口包含最多 256 字节回看、下一规范页和至少 1,024 字节前看，限于 EOF。排版不能读取未加载字节。ruby/数学/表格行范围必须完整驻留；异常大范围请求完整校验加载，失败则不追加 I/O、事务性恢复旧窗口和精确锚点。

## 设置与显示约定

设置页是紧靠普通书籍之前的虚拟书，使用相同行/导航；只有功能已实现才添加，不留空占位。稳定 ID 为 None=0、Network=1；退役 ID2 只读映射到 Network，不能复用。首屏有一行设置和 21 本普通书。旋转后各物理按键组仍独立。

标点压缩和汉字/ASCII 间距默认开启，只通过 AP WebUI 配置。忽略但保留旧 `cnStyle`。当前样式读取经验证的 `readerCfgV1`，它是带边界、保留位和 CRC 的 20 字节版本化记录。保存/回读成功后才应用。几何设置影响缓存身份；点击时序、刷新频率和图片不影响。

P/S 调整使用原生 ink，保留显式空格。code/pre/math/rule 边界阻断正文上下文调整。代码为原生 24 px 比例字体，不合成等宽；保留缩进，关闭普通两端对齐/断词。降低时钟或关机前等待面板完成。电源证据和 U 的 240 MHz 所有权门禁见[电源说明](power.zh-CN.md)。

关机图片为可选 `PDI1`：16 字节头、小端尺寸、保留字节为零、payload CRC32、逐行对齐且高位优先的像素，最多 540×960。浏览器本地适配/转换并预览，不放大。设备写入并验证非活动 SD 槽后才修改选择。删除前先持久化“none”；“firmware default”恢复编译内置回退。自定义图片缺失/无效时使用回退，不影响书籍/进度。

## AP 与预处理

A 提供 WPA2 单客户端 AP、英文设置/配网/上传 UI 和本地 HTTP，不宣称 TLS。API 使用精确会话 cookie 认证；二维码 query token 只用于根页面引导。路由不暴露密码，也不授予 LAN 接收器权限。`wifiCfgV1` 保存已校验 Wi-Fi 记录，旧 SSID/password 键只作只读回退。NVS 未加密。

HTTP 适配器限制头 3 KiB、请求体 17,000 字节、参数 16 个、缓冲响应 32 KiB。每次 poll 最多处理 2 KiB I/O，普通截止时间五秒。不可变编译资源可直接从 flash 流出，最多 1 MiB，独立响应截止时间 45 秒；不需要资源大小的 RAM 响应，但不能抢占单次阻塞 SDK 调用。

AP 准入检查名称、格式、大小、存储、目录容量、generation 和可选 128 位请求身份。块最多 4 KiB，偏移必须精确。四个 RAM 完成回执支持同会话重试；AP 重启、换卡或回执被淘汰后窗口结束。浏览器保持稳定请求 ID，fetch 截止 12 秒、最多三次尝试；结果不明时要求用户核查，不暗中再导入一份。活动传输 lease 为 45 秒进度窗口；AP 空闲五分钟关闭，后台轮询不延长它。

浏览器 Worker/WASM 和主机 CLI 使用生产导入器/PDS 序列化器及 PDT1 extractor2。先上传源文件，再用服务端返回的文件名上传旁车。预处理只提供文本/语义，不提供设备页表或书签。源 SHA-256 和 payload SHA-256 是完整性检查，不是发布者签名。命令、字段、信任、强制 Worker 取消和再生见[预处理内容](prepared-content.zh-CN.md)。

## LAN 身份与托管 Push

`lan_nvs::Store` 仅管理 `paperlan`。`nvs_get_blob` 区分 Absent、Present、Error；仅 NOT_FOUND 允许首次创建。身份私钥/证书共用一个已校验 blob，最多四个客户端 verifier 共用另一个。标签须为非空合法 UTF-8、最多 48 字节，不含 C0/C1。打开、类型、读取或提交错误均关闭准入。不确定的凭据修改使 registry 失效，直到验证重载，并在当前响应排空后关闭服务。目标上使用常数时间秘密比较和不可消除清零。

N 只在显式选择期间存在。设备专属 P-256 TLS 1.2 使用 ECDHE-ECDSA/AES-128-GCM；发送端在任何 HTTP、token 或配对秘密之前验证精确证书 DER 的 SHA-256。Bearer 授权独立于 AP cookie/Wi-Fi。锁定 SDK 上两个 16 KiB record 容量需要 33,434 字节内部缓冲，另加 TLS/密码上下文；PSRAM 不能替代这些内部堆。握手、空闲和绝对截止时间是协作式检查，不会抢占密码运算。

有界 store 同时一个事务，使用精确偏移 4 KiB 块、8 KiB 验证步骤和十六个客户端隔离的共享回执槽。普通文件不覆盖书籍。Push 为最多 16 KiB Markdown，使用两个自有 SD 槽；固定 `pushRead0`/`pushRead1` 进度记录绑定摘要/长度，避免每版本增加 NVS 键。Push 绕过普通页面缓存/驻留复用。回执重放先于版本检查；已验证且内容相同的 Push 是无操作，保留版本/锚点。发布结果不明则保持 Uncertain，靠保存字节恢复，不宣称 FAT exactly-once。协议、CLI 和实机范围见 [LAN 协议](lan-protocol.zh-CN.md)。

## 固定 USB DCD 修正

未修改的 MIT 控制器源固定于 lib-builder 提交 `62d849ae49ac9b71c408bcc35039a5e487802806`，精确校验值及声明见[第三方声明](../THIRD_PARTY_NOTICES.zh-CN.md)和[来源记录](../tools/vendor/arduino-esp32-2.0.17/README.md)。`patch_usb_dcd.py` 用同一个 task/ISR 自旋锁串行保护三个 FIFO-empty mask 更新。IN 完成先撤销旧 mask、确认 pending completion/timeout，再发布事件，并跳过旧传输后续处理。TXFEMP 是只读位。临界区只覆盖寄存器更新，不含事件、队列、日志或 FIFO 复制。

POST hook `pio_usb_dcd.py` 核对源/archive/对象身份，编译构建目录副本，将恰好一个 SDK 库 token 替换为已校验 archive。已安装 framework 不变，固件构建无需网络下载。除 `tests/usb_dcd_patch.py` 可移植回归外，还应验证最终 link map 的来源；传入 `--framework`、`--ar` 可检查已安装 archive。该修正有独立硬件验收，不能据此确定某次断连的根因。

## USB 所有权与固件恢复

默认 MODE0 构建提供 CDC 和应用控制的 MSC。U 先保存进度、停止存储/网络任务、关闭句柄、撤销 lease 并卸载 FAT。HostOwned、ExitRequested、Fault 都保持主机独占；只有接受主机弹出且原始驱动成功释放后才允许本地 FAT。PREVENT/ALLOW、同步和 START STOP UNIT 控制转换。断开、挂起、USB reset 和空闲均不等于安全弹出。

回调每次最多传输 4 KiB。最新源码在整个所有权期间保持 240 MHz，保留首次介质/驱动错误及 backend/卡检测细节。Fault 不能当成 owned=0。主机持有介质时，对关机/reset/1200 touch 均有门禁。一次性 RAM 盘测试不访问 SD。RAM 盘 MSC 写入/回读、正常弹出及重挂载通过。
实际 SD USB、新渐进分页夹具及 RGB565 修正后的最终叠图恢复仍未验收。范围见架构证据表，RAM 盘通过不能证明 raw SD USB 已修复。

只有安全释放所有权后，才可使用原生 CDC 恢复路径：1200 baud，同时拉起 DTR 和 RTS，然后使用新枚举的 ROM 端口。
仅写 app0 前，核实具体设备、分区布局和应用镜像。保留进度时，不擦 NVS、不写 bootloader/分区表、不改书籍路径。主机持有介质时禁止 reset 或刷写。

## 测试、诊断与发布

先运行相关回归，再执行要求的完整主机/锁定 SDK 检查。主机夹具覆盖损坏 XML/ZIP、UTF-8/源码映射、PDS/ECS/PDT 边界、章节/页面重启、撕裂写入、未固定文本投毒、精确锚点、分配失败、实际 ink 和公式 primitive 计数、几何/页脚/旋转、协议重放、持久化错误和 USB 所有权。draw/index 一致本身不能证明对象只绘制一次。

`PAPERDIY_DIAGNOSTICS` 提供有界本地命令，包括 STATUS、SCREEN、PAGE_TEXT、OPEN、导航和 CACHEINFO。偏移与精确锚点是独立字段。OPEN/NEXT/PREV 是写状态操作，不是只读探测。可选 EPUB/DEVICE/NETWORK 标志仅暴露合成验收助手，公开构建均不含。`SD_IO`、`SD_CACHE_IO` 用 8 KiB 块对最多 256 KiB 读取两遍，仅内部比较 SHA，不打印名称/内容/摘要；目录先于文件打开分类。前缀一致不等于全文件验证，`read_us` 也不是打开书籍耗时。截止时间为协作式，不能取消阻塞文件系统调用；探测不会修改挂载时钟。

设备日志、framebuffer、备份、凭据、原书和自定义图片留在公开源码之外。主机测试不能证明电流、电池寿命、卡耐久性、物理唤醒、二维码可读性或无线吞吐。预处理、传输、设备验收和显示应分别报告。硬件数据集中写入架构证据表，并注明实际构建范围。

使用 `python tools/check_public_source.py --index .` 检查全部暂存 blob，使用 `--tree DIR` 检查解包源码产物。白名单和私人标记不能替代人工 diff/待推历史审查或固件审计。运行 `python tests/public_source.py`。源码发布必须来自已审查提交；固件包只含公开应用镜像、校验和、升级说明及匹配许可证。不能打包工作树、交接文件、NVS/SD/flash dump、书籍、私人日志/源字体缓存或私人图片。测试/CI 成功不授权 commit、push、tag 或发布。
