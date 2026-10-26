# LAN 文档与消息投递

[English](lan-design.md) | [简体中文](lan-design.zh-CN.md)

状态：2026-10-23 历史提案。保留设计背景，不作为当前协议或实现清单。开发实现见 [LAN 协议](lan-protocol.zh-CN.md)与[架构报告](architecture-and-implementation.md)：设置页内明确的 N 会话、逐批在设备上确认（短按 A+B）、以短验证码代替二维码的首次配对、mDNS 发现、证书固定、有界发送队列、单份覆盖的 Push 书籍及普通文件投递。多消息收件箱和阅读中接收不在已实现范围内。下文提案的上限和生命周期可能与最终实现不同，集成以协议文档为准。

## 范围与建议

先实现用户明确选择 N 时由本地 CLI 发送一份文档，复用 A 的有界上传/存储事务及设备阅读器，在外层增加认证发现与 TLS。N 不暴露 AP 页面、Wi-Fi 设置、文件系统浏览、书签、任意删除、固件更新或命令执行。

网络继续服从设置生命周期：离开设置、选 Off/A/P、关机和重启都停止 N，USB 供电不自动启动。第一版打开收到的文档因而会停止 N。阅读中接收需另行审查；不能简单删除导航处 `networkStop()`。

阅读期间接收后来作为单独审查的变更实现为 P（见[协议：P](lan-protocol.zh-CN.md#p实时-push)）。P 只为 Push 在导航时保持 STA 与接收端；配对和文件仍属于 N，离开设置页仍会停止 N 与 A。它的连接与电池限制放在独立且有主机测试的策略（`live_policy.h`）中，而不是写进导航代码。

文档投递是首个里程碑。短消息之后可复用 UTF-8 Markdown 摄入路径，增加更小配额和明确收件箱。不能替换当前页面、改进度或模拟按键。可显示时只用页脚通知；收件箱导航、未读处理须独立 UI 审查。不要求常驻桌面守护进程、外部中继或账号。

## 当时的行为与可复用边界

实现位于 [network.cpp](../src/network.cpp)、[bounded_web_server.h](../src/bounded_web_server.h)、[bounded_http_request.h](../src/bounded_http_request.h)、[app.cpp](../src/app.cpp)。下列为当时已实现行为，不是 LAN 承诺：

- A 接收一个文件事务，分块为解码后 4 KiB base64。准入使用 generation 与可选客户端 `requestId`。status 报已接收偏移，commit 重放返回原结果而不再次导入。
- 最近四份完成回执只在当前 AP/SD 会话的 RAM 中，离开 A 或换存储即清除。客户端断开会终止未完成 AP 上传，没有重启恢复。
- 上传有 45 秒进展租约。A 五分钟空闲后关闭，后台 status/文件轮询不续期。
- HTTP 适配器上限：头 3 KiB、正文 17000 字节、响应 32 KiB、每次 poll I/O 2 KiB、请求/响应五秒。绕开固定 SDK 的 multipart 解析器，使用 connection-close HTTP。
- [document_format.h](../src/document_format.h) 统一格式和上限：TXT 4 MiB、EPUB 64 MiB、Markdown 1 MiB；书库最多 256 本。临时文件归导入器所有，提交不覆盖已有书。
- N 已建立连接后掉线，会获得新的 20 秒重连宽限期。活动网络禁止手动轻睡眠，Off 恢复离线功耗策略。

当时建议在实现 LAN 前从网络回调提取 `UploadStore`，负责校验、准入、偏移、存储租约、提交结果和清理，接收明确的存储/网络生命周期事件。AP/LAN 适配器只提供已认证所有者和有界请求正文，不通过存储层调用 UI、修改书签或写 Wi-Fi 配置。SD、书目和回执由应用所有者任务串行修改；TLS worker 可排入有界工作，不能直接修改这些状态。

## 发现不等于认证

只在 N 已有地址且接收端就绪时广播。提议 DNS-SD 类型 `_paperdiy._tcp`，主机名类似 `paperdiy-<random-id>.local`。这是私有服务名称提案，不声称已在 IANA 注册。随机设备 ID 稳定且独立于 MAC、SSID 和账号；重置配对时生成新 ID。TXT 只放少量协议主版本、TLS 要求、内容类型，不广播书名、进度、客户端名、Wi-Fi 凭据、配对 secret 或 bearer token。稳定 ID 本身仍对同 LAN 成员可见。

mDNS 用于本地主机名/服务发现；DNS-SD 以 PTR、SRV、TXT 定位和描述服务，不能替代设备认证。地址丢失/服务停止即撤销广播，地址变化后重新发布。多播或客户端互访受限时可用屏幕 IP，不扫描无关子网。参考 [ESP-IDF mDNS API](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32s3/api-reference/protocols/mdns.html)和 [DNS-SD 规范](https://www.rfc-editor.org/rfc/rfc6763.html)。

## 配对、凭据与传输

配对是独立于发现的实体操作。未来配对屏可显示含协议版本、设备 ID、公钥指纹和一次性随机 secret 的 QR；当时提议两分钟窗口、至少 128 位熵。CLI 在本地读取载荷，在任何 secret 交换前固定 TLS 公钥，再接收设备生成的 256 位客户端 token。远程请求不能静默开启配对窗口。两键配对/撤销 UI 仍是当时实现前提。

设备本地生成独立密钥，不在公开固件嵌入私钥。每次连接都验证固定身份，即使 mDNS 报告 IP 变化也一样；不提供禁用证书验证的回退。明确配置的指纹是带外信任锚，不是任意接受自签名证书。HTTP 允许客户端配置预期证书等替代身份，见 [HTTP 服务端身份验证](https://www.rfc-editor.org/rfc/rfc9110.html#section-4.3.4)。所选客户端须明确处理时钟初始化及证书续期，本地传输不依赖公网 NTP 或外部 CA。

初期最多四个客户端，各有独立可撤销 token。设备只存校验值，客户端存入 OS 凭据库或用户专用本地配置。通过 TLS `Authorization` 头发送，不能放查询参数、浏览器 cookie、日志或 mDNS TXT。所有写入/status 认证并限定客户端。AP 会话 cookie、家庭 Wi-Fi 凭据对 N 无权限。传输与存储须保护 bearer，原则见 [RFC 6750](https://www.rfc-editor.org/rfc/rfc6750.html#section-5)，不要求本地协议另建 OAuth 服务。

AP 响应写入器使用原始 TCP 描述符，换成 443 端口不会变成 TLS。应评估独立有界 TLS 适配器或 ESP-IDF HTTPS server，保留限制、取消、看门狗响应。初期只一个 TLS 会话，限定握手，活动有界传输后关闭。SDK 文档说明首次 TLS 建立成本较高、同安全连接后续请求较低；这些是参考数据，不是本设备测量。可从 [ESP-IDF HTTPS server](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32s3/api-reference/protocols/esp_https_server.html) 开始验证。

## 当时提议的协议 v1

这些路径/字段是历史审查契约，不是当前已实现 API。既有 AP `/api/...` 保持兼容，LAN 使用独立 `/lan/v1/...` 且无 HTML 路由。线协议、文档格式、缓存版式和固件发布独立版本化。拒绝不支持的主版本；v1 内新增响应字段可为可选。

| 请求 | 目的与结果 |
| --- | --- |
| `GET /lan/v1/capabilities` | 认证后返回版本、格式、字节/分块上限、当前随机 `epoch`、准入 generation 和回执容量，不列书库。 |
| `POST /lan/v1/uploads/start` | `epoch`、`generation`、128 位随机 `requestId`、名称、类型、大小、SHA-256；返回 ID、已接收偏移、进展租约；相同重放返回已有事务。 |
| `GET /lan/v1/uploads/status?id=...` | 所有者限定的 `receiving` / `complete`、已接收偏移、大小、最终回执；未知/过期明确。 |
| `POST /lan/v1/uploads/chunk` | ID、精确偏移、有界 base64；返回下一偏移，初期复用 4 KiB 块。 |
| `POST /lan/v1/uploads/commit` | 发布前验证完整大小/哈希，重复提交返回同一回执。 |
| `POST /lan/v1/uploads/cancel` | 只取消该客户端未完成事务，不移除已完成文档。 |

接收端启动、存储变化或重启产生新随机 epoch，避免 generation 重置后旧请求被准入。ID 绑定客户端、epoch 和完整原元数据；不同元数据复用 ID 为冲突。只有新准入增加 generation，status、start/commit 成功重放不增加。其他客户端获得 busy，不进入隐式队列。限制/错误码须机器可读，说明文本可选。

初期建议一个活动事务及四个完成回执，与当时有界内存模型一致。回执含最终文件名、源哈希、字节数和文档 ID。无法证明恢复时返回 `unknown_upload` / `epoch_changed` 等，客户端不能据此静默创建另一事务。用户可明确新建请求，但旧导入的不确定性必须保留可见。

start 响应丢失时用原 ID/元数据重试；chunk 响应丢失时查已接收偏移，不假定 POST 超时即未应用；commit 响应丢失时用同一 ID 重试或查回执。重试有次数限制、退避和抖动，认证/元数据冲突/大小/存储错误即停。重试安全来自应用语义，不是 HTTP 自动使 POST 幂等，见 [RFC 9110 §9.2.2](https://www.rfc-editor.org/rfc/rfc9110.html#section-9.2.2)。

当时建议 N 的短暂 TCP/Wi-Fi 断线可保留事务到进展租约到期，与 AP 断开即取消不同。拔 SD、离开 N、关机或到期终止，status 探测不续租。该提案的字节/结果仍限会话，不承诺重启续传、持久回执或跨断电恰好一次。FAT rename 只避免普通运行中发布半份，不能保证断电耐久。持久回执/日志和恢复检查点应先独立审查磨损及损坏，再宣传保证。

## 文档、公式与消息

A、N 或直接 SD 导入都保留原 UTF-8 `.md` / `.markdown` 为权威内容。书签使用已验证源身份下的原字节偏移。文档/布局身份独立，使 Markdown 改动不使旧 TXT/EPUB 进度失效。各导入器统一大小、UTF-8、名称及不覆盖规则。

设备本地解析受支持的 Markdown 块/行内结构，以宽、上伸、下伸盒模型排数学。分式、根式、上下标、算子、希腊字母、基本矩阵不依赖 LAN 预渲染服务。不支持内容应可读地保留原文，不静默删除。HTML、代码围栏、链接、公式均为数据，不执行代码、不取外部图/字体、不解析网络 include。

以后可选 `DocBundle` 携带预处理公式资源，但须保留全部原文及位置映射。有界、带版本 manifest 应含源哈希、公式源哈希/范围、字体/渲染配置哈希、基线/上伸/下伸。资源只是缓存，不符即丢弃并本地原文回退。分配前验证类型、字节数、尺寸，只接受导入器分配路径，不接受任意归档解压路径。普通 Markdown/本地公式离线验收前暂缓 bundle。

后续收件箱曾提议 `kind=note`，每条最多 16 KiB UTF-8、专属目录保留八条，并有总配额；满时报告 inbox-full，不能静默删除未读。复用事务，不另建无界消息队列。明确用户操作前，消息不成为书籍、不改变选中项。TTL、删除、已读回执和双键收件箱 UI 定案后才能在 capabilities 广告此类型。

## 交互与功耗策略

| 机制 | 行为 | 建议 |
| --- | --- | --- |
| 明确前台发送 | 本地 CLI 联系活动 N，发送一项后关闭；只在选择的窗口需要无线电。 | 首个里程碑，无需桌面服务。 |
| 按需拉取 | 设备仅在明确请求时联系用户选择的本地伴随端。 | 可选，适合本地发件箱；增加配置与出站认证。 |
| 周期轮询 | 重复检查本地伴随端，以周期权衡延迟与无线电唤醒时间。 | 暂缓，无默认定时器/后台云端点。 |
| 持久 SSE/WebSocket | 会话/Wi-Fi 存活时低延迟，需重连、心跳、有界背压。 | 暂缓到存在明确连续流用途；USB 不自动启用。 |

当时提议第一版电池 N 会话五分钟无活动关闭，只有准入传输进展续期，发现/status 不续期。外部供电时，明确选择 N 后可保持到用户离开。USB 移除时启动新的有限电池窗口。暂停事务仍受更短进展租约约束，断线发送端不能无限阻止空闲关闭。应使用既有稳定供电检测，不只依赖 CDC 枚举。

网络轮询不重画电子纸、不重置 UI 空闲计时。只刷新变化的连接/传输/收件箱指示，沿用页脚和输入松开保护。网络/存储工作有界，使关机、取消、实体键继续响应。摄入释放存储后再渲染，远程投递不能排入重复导航动作。

手动轻/深睡眠不维持 Wi-Fi；维持连接须 modem sleep 和受支持的自动轻睡眠，见 [ESP-IDF sleep modes](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32s3/api-reference/system/sleep_modes.html)。固定 ESP32-S3 Arduino SDK 为 IDF 4.4.7，其已检查 `dio_opi` 未启用 `CONFIG_PM_ENABLE` / `CONFIG_FREERTOS_USE_TICKLESS_IDLE`，运行时调用 `esp_pm_configure()` 无法补上未编译支持，见[电源管理](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32s3/api-reference/system/power_management.html)。

任何服务活动时保留手动睡眠门控。modem sleep、重构 SDK 自动轻睡眠须作为单独测量变化。无线电关闭是离线电池基线。无线电关闭的阅读器不能立即收 LAN Push，伴随端可留有界队列，但必须报告等待下次明确接收窗口。无电气测量不宣称电流收益或待机寿命。

## 隐私与验收门槛

LAN 不是授权边界：邻居可发包、伪造发现、拖延连接。TLS/固定身份保护传输，客户端 token 授权操作。本提案不含 UPnP 映射、公网监听、遥测、互联网可达性检查、外部 DNS 依赖或自动更新，拒绝跨主机重定向。设备仍可能经路由 LAN 可达，因此认证和明确接收期限不可省。

实体 Flash/SD 访问是另一威胁。家庭 Wi-Fi 凭据在未加密 NVS；token 校验值不加密书籍，也不保护静态 TLS 私钥。Flash 加密/安全启动需明确烧录与恢复设计，不能由网络工作暗示具备。日志只记结果/错误码/字节数，不记 token、密码、正文、书名或私密主机路径。测试用合成文件与临时凭据；实机测试须保留真实配置和进度。

实现 P3 前应一起审查配对/撤销交互、N 生命周期、TLS 资源预算和协议。发布启用前要求：

- AP 兼容及 N 独占生命周期测试；Off/导航/关机不留下监听、mDNS、传输或活动无线电。
- 伪造发现、错指纹、撤销 token、跨客户端 ID 测试；无降级、凭据回显、未认证元数据端点。
- 用生产事务实现测试 start/chunk/commit 响应丢失、重放、回执淘汰、epoch 变化、拔 SD、flush/读回/rename 失败、磁盘满、书库满。
- 固定 SDK 上的慢端/停滞 TLS，验证内存、并发、期限及关机/按键响应有界。
- SD/AP/LAN 的 Markdown 原字节、书签、公式和不支持内容回退一致；打开已收文件不依赖网络。
- 真实传输哈希及 USB/电池空闲/唤醒测试；电气测量先于续航声明。

AP 浏览器回归可用 Node.js 18+ 单独运行 `node tests/web_ui.js`，提取生产内联脚本，测试正常上传及五种模糊/失败网络结果；不能代替原生 handler、真实浏览器/QR 或传输时序测量。
