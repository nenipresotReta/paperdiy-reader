# LAN 发送端与协议 v2

[English](lan-protocol.md) | [简体中文](lan-protocol.zh-CN.md)

本文是客户端与设备的集成契约。前台 Python 发送端、固件 TLS 接收端及有界持久存储已实现。主机测试覆盖协议、存储故障与所有权门控；[架构报告](architecture-and-implementation.zh-CN.md)说明实现边界；协议测试不能证明实体投递或无线性能。

运行 `python3 tests/lan_client.py` 执行发送端测试：使用模拟阅读器、临时证书，只在 `127.0.0.1` 监听。设置 `PAPERDIY_LAN_WIRE` 指向 `lan_service_wire` 构建目标（CMake 会自动设置）时，同一发送端还会以真实请求字节驱动真实固件处理器。

## 模型

```
发送端                               阅读器（设置页 + N）
发现 / 解析               ───────▶  mDNS 应答
固定证书 TLS（首次联系带验证码）
提交一批（1–8 项）        ───────▶  逐项预检
                                    提示 "Hermes · Receive 3 files?"
                          ◀───────  短按 A+B → 接受这一批
逐项上传 + 提交           ───────▶  同一时间一个传输
                          ◀───────  每项一个回执 → "Done · 3 files"
```

- **只在设置页接收。** 接收端、其路由与 mDNS 通告仅在设置页且 N 激活时存在。
- **每批都须在设备上同意。** 一次短按 A+B（2 秒内松开）接受整批。A 保持原义（下一模式），因此会关闭 N 并取消；B 旋转并重绘；离开设置页取消；60 秒无操作过期；按住 A+B 2 秒关机，永不接受。未经接受，卡上不写任何内容。
- **首次投递即配对。** 未知发送端的第一批带有承诺保护的短验证码（SAS）；一次 A+B 同时配对并接受。单独配对就是不带条目的同一请求。

CLI 不安装守护进程、不启用网络、不修改主机网络、不扫描子网、不使用中继。`queued` 表示发送端保存了内容，`delivered` 表示已验证匹配的提交回执；两者都不代表设备已显示内容或用户已阅读。

## TLS 身份

每台阅读器生成自己的密钥和自签名证书。`F` 为证书 DER 的 SHA-256；**设备 ID 是 `F` 的前 16 个十六进制字符**，因此通告的 ID 与出示它的证书绑定。固件提供 TLS 1.2，发送端要求 TLS 1.2 或更新。

发送端在**写出任何 HTTP 字节之前**比较实时证书与固定值。唯一不固定证书的请求是 `GET /lan/v2/hello`，不带凭据和正文，仅用于首次联系与 `--host` 识别阅读器。配对后固定值就是被接受的首次联系中观察到的证书。没有接受任意证书、HTTP、重定向或备用指纹回退。地址可变，固定身份不变。

认证请求携带 `Authorization: Bearer <clientId>:<token>`。AP cookie、AP token 和 Wi-Fi 密码不授予 LAN 权限。回执和上传查询都限定在所属客户端。最多配对四个发送端。

## 首次联系

```
GET  /lan/v2/hello                                    （不固定证书，无凭据）
→ { v:2, deviceId, pushRevision, batchEntries:8 }     发送端检查 deviceId == F[0:16]

POST /lan/v2/batch/offer   label, commit = hex SHA-256(Ns), e0..e7（0–8 项）
→ { batchId, state:"awaiting_reveal", pairingId, deviceNonce = Nd, deviceId, entries }

POST /lan/v2/pair/reveal   pairingId, senderNonce = Ns
→ { batchId, state:"awaiting_acceptance" }            验证码只显示在阅读器上，不在响应中

GET  /lan/v2/batch/status?batchId=…&proof=HMAC-SHA256(Ns, "paperdiy-batch-status-v2" ‖ batchId)
→ awaiting_acceptance | expired | accepted + deviceId + clientId + token + pushRevision
```

```
H   = SHA-256( "paperdiy-pair-v2\0" ‖ F ‖ Ns ‖ Nd ‖ pairingId ‖ len16(label) ‖ label )
SAS = big-endian uint32(H[0:4]) mod 1 000 000，显示为 "482 913"
```

`F`、`Ns`、`Nd` 为 32 字节原值；`pairingId` 为其 16 个 ASCII 十六进制字符；`len16` 为大端 UTF-8 字节长度。发送端用自己观察到的证书计算 `F`，阅读器用自己的证书。持有其他证书的中继会让两端得出不同数字；先承诺 `Ns` 再得知 `Nd` 防止碰撞搜索（每次约 10⁻⁶）。

- offer、reveal、status 都固定为 `hello` 观察到的 `F`；证书不同的连接永远拿不到 token。
- 阅读器只在 A+B 后接纳客户端。status 在发送端第一次使用 bearer 前返回 token，之后遗忘。
- 标签为 1–48 个 UTF-8 字节，不含 C0/C1 控制字符。已配对发送端按注册表标签显示；自报标签只与 `New sender` 一起出现。
- reveal 失败即结束本次尝试。未认证的 offer、hello 以及失败的 reveal/proof 计入限速（10 秒 8 次）。

共享测试向量（`tests/lan_batch.cpp`、`tests/lan_client.py`）：`F = 0x11×32`、`Ns = 0x22×32`、`Nd = 0x33×32`、配对 ID `0123456789abcdef`、标签 `Hermes 書` → SAS `506 740`；批次 `fedcba9876543210` 的 proof → `4b19bde7b8dd1e27aab707265889ba8c7a1b22e0bba59770d2f6dc3ebe6533e4`；commit → `9f72ea0cf49536e3c66c787f705186df9a4378083753ae9536d65b3ad7fcddc4`。

## 批次

```
POST /lan/v2/batch/offer   （Bearer）e0..e7
→ { batchId, state:"awaiting_acceptance", entries:[{requestId, admission}] }
→ { state:"blocked", entries:[…] }                    无可接受项：不提示、不留状态

GET  /lan/v2/batch/status?batchId=…   （Bearer）
→ { batchId, state: awaiting_acceptance | accepted | expired, entries }
```

- 1–8 项（仅首次联系配对可为 0），`e0` 起连续，每批最多接受一个 Push。未知或重复字段被拒绝。
- 每个 `eN` 含 8 个字段，均为 `<UTF-8 字节长度>:<字节>`，顺序为 `requestId fingerprint kind name title size sha256 baseRevision`。长度为最多三位的规范十进制，按字段限长（32、64、4、96、96、10、64、10）；文件的 `baseRevision` 为空。首次联系时该解析器在认证前运行，有界且经过模糊测试。
- 预检代码：`ok`、`invalid_metadata`、`size_limit`、`unsupported_format`、`already_received`、`duplicate_push`、`revision_conflict`、`library_full`、`no_space`。预检只是建议，上传和提交时的检查才是权威。
- 每台阅读器同时只有一个待处理批次（`409 batch_busy`），直到每个已接受项都有回执。发送端可以替换自己中途停止上传的已接受批次；替换批次需要新的 A+B。
- 接受后，`uploads/start` 只接纳已接受批次中的条目（否则 `409 not_accepted`）；已完成请求的重放返回其回执。已接受集合保存在 RAM 中，N 结束即清除。批次不是原子的：每项各自上传、提交、取得回执。

## 上传操作

POST 使用 `application/x-www-form-urlencoded`，双方均限制最多 16 个字段、编码后 17000 字节，可容纳 4096 字节 base64 分块及 8 个最坏条目的百分号编码。响应是带 Content-Length 的有界 JSON。发送端拒绝 chunked 响应、重复头/JSON 键、重定向、超过 8192 字节的头和超过 16384 字节的 JSON。错误以稳定 snake_case `code` 为准。

| 操作 | 请求与结果 |
| --- | --- |
| `GET /lan/v1/capabilities` | `v:2`、`deviceId`、`pushRevision`、32 位十六进制 `epoch`、`receiptCapacity:16`、`receiptScope:"client"`、`batchEntries:8`、`limits`。 |
| `GET /lan/v1/push` | 当前 Push 元数据，不含正文。 |
| `POST /lan/v1/uploads/start` | `requestId`、`fingerprint`、`kind`、`name`、`title`、`size`、`sha256`、`baseRevision`。相同重放返回同一传输或回执。 |
| `POST /lan/v1/uploads/chunk` | `requestId`、已接受的精确 `offset`、`data`（base64，≤ 4096 字节）。响应丢失后不盲目追加。 |
| `GET /lan/v1/uploads/status?requestId=…` | 接收/校验状态与已接受偏移，或已完成回执。 |
| `POST /lan/v1/uploads/commit` | 校验并发布；阅读器计算哈希时可能返回 `checking`。重放返回同一回执。 |
| `POST /lan/v1/uploads/cancel` | 仅取消本客户端未完成的传输。 |
| `GET /lan/v1/receipts?requestId=…` | 已完成回执，或未知。未知绝不证明请求未生效。 |
| `POST /lan/v1/clients/revoke` | Bearer 自我撤销，`clientId` 必须是自己的。 |

`limits` 键为 `chunkBytes`、`pushBytes`、`txtBytes`、`epubBytes`、`markdownBytes`：Push 16384 字节源文、TXT 4 MiB、EPUB 64 MiB、Markdown 1 MiB。名称/标题最多 96 个 UTF-8 字节，不含 C0/C1 控制字符；名称不得含路径分隔符、Windows 非法字符、前导点或结尾点/空格。Push 名称为 `Push.md`。文件不会覆盖已有书。

接收/校验回复含 `state`、`requestId`、`fingerprint`、`size`、`offset`；`checking` 要求 `offset == size`。完成回复为 `state:"complete"` 加 `receipt`，含 `requestId`、`fingerprint`、`kind`、`size`、`sha256` 以及 `revision`（Push）或最终 `name`（文件）。发送端验证全部身份与内容字段后才报告投递成功。

v2 移除：`POST /lan/v1/pair`、凭实体 secret 的 `clients` 与 `clients/revoke`、配对二维码。已有 v1 客户端继续有效。

## 请求身份与并发

`requestId` 为 128 位随机数（32 个小写十六进制字符）；持久查询键为 `(clientId, requestId)`。请求指纹是对 `kind, name, title, size, sha256, baseRevision` 的 SHA-256，每个字段先写 4 字节大端 UTF-8 长度再写字节；文件的 `baseRevision` 为空。测试向量：`push`、`Push.md`、`Synthetic`、`3`、64 个 0、`0` → `7c5dc380b37e359158a305fc6aff88f3397a4c8a7d1b8a2d00c89c3fd0016d74`。

Push 是一本可替换的书。其基准修订是排队时已知的阅读器修订；对尚未认识的阅读器的第一个 Push，则取提交批次时阅读器报告的修订。排队内容永不改基到更新的修订；冲突为 `blocked` 加 `revision_conflict`。回执重放先于新的修订检查，因此响应丢失的提交在之后修订变化时仍可恢复。相同的 Push 正文是有回执的空操作。

阅读器共有 16 个回执槽（按客户端查询）以及两个 Push 提交记录。两整批即可写满，因此发送端在向某台阅读器提交新内容前，先解决该阅读器所有 `uncertain` 条目。断电后的发布恢复会核对意图、选定的最终文件及其完整哈希；已有文件从不删除或覆盖。

## 发现

N 激活期间，阅读器通告 `paperdiy-<deviceId>.local` 和 `_paperdiy._tcp`（端口 8443，TXT `id=<deviceId>`）。发送端向 `224.0.0.251:5353` 发送 RFC 6762 一次性查询（QU 位、临时端口，两次各 1 秒），只接受所有者名称完全匹配的 IPv4 A 记录，忽略 AAAA。不依赖 Bonjour、nss-mdns 或 Windows mDNS；多播无法到达局域网时（如 NAT 后的容器）会干净地失败，此时使用 `--host`，并在路由器为阅读器保留固定地址。

- **已配对：** 先用保存的地址与固定证书；该地址网络失败或证书不符时，解析 `paperdiy-<deviceId>.local` 并对每个 IPv4 结果用固定证书尝试。只有固定证书成功后才保存地址。其他阅读器不会让已配对发送端产生歧义。
- **未配对：** 一次服务查询。一台 → 首次联系；多台 → `multiple_devices` 并给出 `{deviceId, host}` 候选（从不猜测）；没有 → `device_not_found`。`--host` 跳过发现。

mDNS 只提供候选地址；身份始终由证书决定。

## 发送端

Python 3.9+ 标准库，支持 macOS、Linux（含容器）与 Windows。每次运行一个前台命令，无守护进程。

```sh
python3 tools/paperdiy_lan.py push brief.md --title "Brief" --label Hermes --json --wait 5
python3 tools/paperdiy_lan.py send a.epub b.md --json --wait 5
python3 tools/paperdiy_lan.py sync --batch-id 0123456789abcdef --json --wait 300
python3 tools/paperdiy_lan.py pair --label Hermes
python3 tools/paperdiy_lan.py status
```

| 命令 | 用途 |
| --- | --- |
| `push SOURCE [--title T]` | 排队替换 Push，然后提交。 |
| `send SOURCE…` | 排队普通文件，然后提交。 |
| `sync [--batch-id ID] [--reoffer]` | 继续某批次、解决不确定条目、提交已排队内容。`--reoffer` 包含曾 `not_accepted` 的条目。 |
| `pair --label NAME [--host IP]` | 不带条目的首次联系（也用于重新配对）。 |
| `status` | 本地阅读器、待完成的首次联系与条目。 |
| `endpoint --host IP [--device D]` | 在该地址验证固定证书的阅读器并保存。 |
| `revoke [--device D]` | 在阅读器上撤销本发送端，然后本地遗忘。 |
| `cancel` / `abandon --request-id ID` | 取消未完成传输 / 释放本地条目（不撤销可能已发生的发布）。 |
| `prune --delivered` | 删除已投递的归档。 |

通用选项：`--device D` 或 `--host IP`；`--label NAME`（首次联系）；`--wait N`（默认 120）限制发现和等待接受的时间；`--budget N`（默认 600）限制包括上传在内的整次运行；`--wait ≤ --budget ≤ 600`；`--wait 0` 仅排队；`--json`。

- 只有一台已配对阅读器时 `--device` 可省略；多台 → `ambiguous_device`；没有 → 首次联系（需 `--label`）。
- 批次按排队顺序取条目，最多 8 项、最多一个 Push；第二个 Push 等下一批。`blocked` 与 `not_accepted` 条目在用户决定前不再提交，同一次运行中任何条目最多提交一次。
- 首次联系在 `--json` 下提前返回 `awaiting_pairing_confirmation` 与 `sas`（AI 须转告数字）；人类输出打印数字后继续等待。

### 条目状态

在 `uploads/commit` 之前立即持久化 `commitSent`。

| 情况 | 状态 | 代码 | 退出码 |
| --- | --- | --- | --- |
| 找不到 / 无法连接阅读器 | `queued` | `device_not_found` / `device_unreachable` | 0 |
| 等待结束时仍待 A+B | `queued` | `awaiting_acceptance` / `awaiting_pairing_confirmation` | 0 |
| 未被接受（取消、过期、连接中断） | `queued` | `not_accepted` | 0 |
| `commit` 之前中断 | `queued` | `transfer_interrupted` / `budget_exhausted` | 0 |
| 已发送提交但无确定答复 | `uncertain` | `commit_unconfirmed` / `receipt_unknown` | 2 |
| 匹配的回执 | `delivered` | — | 0 |
| 预检或上传时拒绝 | `blocked` | 具体原因 | 2 |

其他状态：`cancelled`、`abandoned`、`superseded`。本地与用法错误为 `{"state":"error","code":…}`，退出码 2：`label_required`、`no_paired_device`、`ambiguous_device`、`unknown_device`、`multiple_devices`、`unauthorized`、`pin_mismatch`、`source_unreadable`、`invalid_utf8`、`size_limit`、`state_dir_unavailable`、`state_dir_permissions`、`lock_held`、`store_corrupt`。

JSON 输出为 ASCII（`ensure_ascii`），包含 `state`（`done`、`pending` 或 `error`）、`next`、`summary`、逐项 `items`，必要时含 `batchId`/`sas`；从不包含 token、随机数、正文或本地路径。人类输出为 UTF-8，在其他编码的控制台上替换无法编码的字符。

### 存储

| | POSIX（macOS、Linux） | Windows |
| --- | --- | --- |
| 根目录 | `$XDG_DATA_HOME/paperdiy` 或 `~/.local/share/paperdiy` | `%LOCALAPPDATA%\paperdiy` |
| 锁 | `fcntl.flock`，非阻塞 | `msvcrt.locking`，非阻塞 |
| 隐私 | 属主、模式 `0700`/`0600`、拒绝符号链接 | 用户配置文件 ACL；拒绝符号链接与目录联接（较弱） |
| 替换 | `os.replace` + 文件与目录 `fsync` | `os.replace` + 文件 `fsync` |

清单保存阅读器（固定证书、地址、客户端 ID、token）、条目以及待完成的首次联系（`Ns` 保存到终态，永不复用）。源字节已归档，之后修改原文件不影响已排队条目。配额：发件箱 16 个文件 / 128 MiB，归档 256 个对象 / 512 MiB。v1 清单原地迁移；没有确定结果的 v1 条目变为 `uncertain`，先通过回执解决。

阅读器侧的发送端移除在热点网页中（Senders：列表、移除、全部遗忘），见[使用说明](usage.zh-CN.md)。
