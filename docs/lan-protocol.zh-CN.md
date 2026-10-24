# LAN 发送端与协议 v1

[English](lan-protocol.md) | [简体中文](lan-protocol.zh-CN.md)

本文是客户端与设备的集成契约。前台 Python 发送端、固件 TLS 接收端及有界持久存储已实现。主机测试覆盖协议、存储故障与所有权门控；[架构报告](architecture-and-implementation.zh-CN.md)说明实现边界；协议测试不能证明实体投递或无线性能。运行 `python3 tests/lan_client.py` 执行合成客户端测试：使用临时生成的证书，只在 `127.0.0.1` 的临时端口监听，不验证真实阅读器或 Wi-Fi 投递。

接收端仅在明确选择 N 的会话中存在。CLI 不安装守护进程、不启用网络、不修改主机网络、不扫描子网、不使用中继。双方必须同时在线。`queued` 表示发送端保存了内容，`delivered` 表示已验证匹配的提交回执；两者都不代表设备已显示内容或用户已阅读。

## 实体配对与 TLS

实体配对载荷是最多 4096 字节的 JSON：

```json
{"v":1,"deviceId":"example-device","host":"192.0.2.1","port":8443,"certSha256":"64 lowercase hexadecimal characters","secret":"32 lowercase hexadecimal characters"}
```

host 为 IPv4/IPv6 字面地址，尚无发现功能。128 位 secret 一次性使用，只在通过实体操作开启的有限配对窗口内有效。每台设备生成自己的密钥和证书；公开固件不得包含共用私钥。配对返回客户端 ID（16 个小写十六进制字符）、256 位 token（64 个小写十六进制字符）、设备 ID 与当前 `pushRevision`。客户端标签非空，最多 48 个 UTF-8 字节，不含 C0/C1 控制字符。最多配对四个客户端；设备只保留 token 校验值，可单独撤销客户端。

固件提供 TLS 1.2，发送端要求 TLS 1.2 或更新版本。在发送**任何 HTTP 字节、secret、token 或正文之前**，发送端对对端完整 DER 证书计算 SHA-256，与实体载荷比较。该精确证书是信任锚，不使用公网 CA、主机名或时钟校验来建立本地身份。更换证书需明确重新实体配对。不提供接受任意证书、HTTP、重定向、代理或其他指纹回退。端点地址可变，固定身份不变。

认证请求携带 `Authorization: Bearer <clientId>:<token>`。AP cookie、AP token 和 Wi-Fi 密码不授予 LAN 权限。回执和上传查询都限定在所属客户端。配对载荷从文件或标准输入读取，不把 secret 放在命令行参数中。配对响应丢失，或凭据持久化/读回失败时报告 `pair_uncertain`，不自动占用另一个配对槽。兼容服务端错误码 `pairing_not_saved` 不能证明失败前 NVS 未保存客户端。

配对结果不确定时，明确开启新的 N 会话并取得当前实体载荷。`clients --payload pairing.json` 通过固定证书的 TLS 列出最多四个客户端 ID 和标签，不需要本地配对记录或 manifest。读取不消耗也不延长五分钟 secret 窗口。检查后用 `revoke-client --payload pairing.json --client-id …` 明确撤销所选身份；撤销消耗该 secret，重新配对前应取得新载荷。这两个命令都不自动撤销或重试配对。清单不返回 token 或校验值。凭据记录本身无法读取或验证时，接收端保持不可用；清单接口不会修复损坏 NVS 或轮换设备身份。

## HTTP 操作

POST 使用 `application/x-www-form-urlencoded`，双方均限制编码后的正文最多 17000 字节，可容纳 4096 字节 base64 分块的最坏百分号编码。响应是带 Content-Length 的有界 JSON 对象。发送端拒绝 chunked 响应、重复头/JSON 键、重定向、超过 8192 字节的头和超过 16384 字节的 JSON。错误以稳定 snake_case `code` 为准，可选 `error` 文本不决定协议行为。每个请求有包含 TLS、头和正文的总期限。

| 操作 | 请求与结果 |
| --- | --- |
| `POST /lan/v1/pair` | 无 bearer，但须固定证书的 TLS；`secret`、`label`。返回 `deviceId`、`clientId`、`token`、`pushRevision`。 |
| `POST /lan/v1/clients` | 须当前实体 `secret`，普通 bearer 不足。返回 `deviceId` 及最多四个 `{clientId,label}` 的 `clients`。只读，不消耗 secret。 |
| `POST /lan/v1/clients/revoke` | 自我撤销使用认证后的 `clientId`；实体撤销使用 `secret` 加明确 `clientId`。成功返回 `revoked:true`，CLI 支持两种方式。 |
| `GET /lan/v1/capabilities` | 认证后返回 `v:1`、`deviceId`、当前 `pushRevision`、32 位十六进制 `epoch`、`receiptCapacity:16`、`receiptScope:"client"`，以及可选 `limits`。 |
| `GET /lan/v1/push` | 认证后返回当前 Push 元数据，默认不返回私密正文。 |
| `POST /lan/v1/uploads/start` | `requestId`、`fingerprint`、`kind`、`name`、`title`、`size`、`sha256`、`baseRevision`。相同重放返回同一事务或回执。 |
| `POST /lan/v1/uploads/chunk` | `requestId`、精确已接收字节 `offset`、`data`（base64，解码后最多 4096 字节）。响应丢失后不可盲目追加。 |
| `GET /lan/v1/uploads/status?requestId=…` | 返回接收/校验状态与已接收偏移，或完成回执；不要求发送端先知道独立上传句柄。 |
| `POST /lan/v1/uploads/commit` | `requestId`。校验并发布，可在设备有界轮询期间返回 `checking`。重放返回同一完成回执。 |
| `POST /lan/v1/uploads/cancel` | `requestId`。只取消本客户端未完成事务，不删除已发布书籍或 Push。 |
| `GET /lan/v1/receipts?requestId=…` | 完成回执或明确未知/过期结果；未知不能证明未应用请求。 |

`limits` 键为 `chunkBytes`、`pushBytes`、`txtBytes`、`epubBytes`、`markdownBytes`。客户端遵守更低上限，不提高本地限制。初始限制为 Push 原文 16384 字节、TXT 4 MiB、EPUB 64 MiB、Markdown 1 MiB，同时只接收一个事务。名称/标题最多 96 个 UTF-8 字节，不含 C0/C1 控制字符；名称还不得含路径分隔符、Windows 非法文件名字符、开头的点或末尾的点/空格。Push 传输名称是 `Push.md`，稳定书库身份不依赖该名称。普通文件不覆盖已有书。

接收/校验响应含 `state`、`requestId`、`fingerprint`、`size`、`offset`。`checking` 必须满足 `offset == size`，但尚不代表成功。完成响应是 `state:"complete"` 加 `receipt`，或直接回执字段。回执含 `requestId`、`fingerprint`、`kind`、`size`、`sha256`，以及 Push 的 `revision` 或文件的最终 `name`。发送端验证所有身份/内容字段后才报告投递成功。

## 请求身份与并发

`requestId` 是 128 位随机数，表示为 32 个小写十六进制字符。持久查询键为 `(clientId, requestId)`；同键的绑定元数据有任何变化即冲突。请求指纹是以下六个字段依次编码后的 SHA-256：

```text
kind, name, title, size, sha256, baseRevision
```

每个字段先写四字节无符号**大端 UTF-8 字节长度**，再写 UTF-8 字节。`kind` 只能是 `push` 或 `file`；`size` 是规范无符号 ASCII 十进制；内容 SHA-256 是 64 个小写十六进制字符；Push 基准版本是规范 uint32 十进制，文件的基准版本是空串。空标题也保留字段。请求/客户端 ID 构成查询键，不进入指纹。

测试向量：kind `push`、name `Push.md`、title `Synthetic`、size `3`、SHA-256 为 64 个零字符、基准版本 `0`，得到：
`7c5dc380b37e359158a305fc6aff88f3397a4c8a7d1b8a2d00c89c3fd0016d74`。

Push 是一本可替换的书。发送端入队时记录基准版本，来自明确指定值或最近一次认证响应。同步不会把新版本附到旧内容上；未知基准会阻塞，须明确以已知基准重新入队。版本冲突可见。回执重放优先于新版本/CAS 检查，因此成功提交但响应丢失后，仍可在后续 Push 发布后恢复结果。验证后正文与当前 Push 相同则为带回执的无操作：保留当前版本、原文、标题和阅读锚点，不发发布通知、不刷新屏幕。

设备有 16 个共享持久回执槽，查询按客户端隔离，并非每个客户端各 16 槽。保留的两份 Push 提交记录也可回答其已验证代次的回执查询。回执淘汰、恢复证据不可读、epoch 变化或未知状态都可能留下不确定结果。断电恢复须验证发布意图、指定最终文件和完整哈希；普通 rename 本身不能保证持久的恰好一次投递。未知结果不允许发送端私自更换请求 ID、再导入一个副本。

设备存储采用两个固定 Push 原文/提交槽、当前发布意图的两个固定副本和 16 个固定回执槽；接收使用一个固定暂存文件。哈希及启动恢复每次应用循环最多读 8192 字节，记录验证/发布与最后一次哈希读取分开执行。已有普通文件从不删除或覆盖。重启后，对意图指定的已有最终文件完整验证长度/哈希；缺失或不符则保持不确定，不改名重复导入。全程要求本地 FAT 所有权及存储租约，卡变化会撤销会话。读回/故障测试不证明实体断电耐久性。

## 发送端存储与命令

要求 macOS/Linux 上的 Python 3.9 或更新版本；传输与持久化仅用标准库。测试另用 OpenSSL 生成临时证书。状态默认存于 `~/.local/share/paperdiy`，使用用户专用目录、凭据/manifest/正文文件、排他发送锁、原子 manifest 替换及原文归档。原文件后续编辑不改变已入队内容。公开代码/夹具不含真实配对数据、文档正文、端点或凭据。

```sh
python3 tools/paperdiy_lan.py pair --device reader --payload pairing.json
python3 tools/paperdiy_lan.py push brief.md --device reader
python3 tools/paperdiy_lan.py send report.epub --device reader
python3 tools/paperdiy_lan.py sync --device reader --wait 60
python3 tools/paperdiy_lan.py status --device reader
```

`--queue-only` 只归档、不传输；全局 `--state-dir` 放在子命令前。`--budget` 限定本次前台同步总时长（默认 120 秒，最多 600）；`--wait` 可在同一预算内等待离线接收端（最多 600）。没有定时重试。status 读取本地证据，不代表接收端在线。实体 `clients` / `revoke-client` 只需 `--payload`，撤销另需明确 `--client-id`，不创建或读取本地状态。

发件箱最多 16 个普通文件/128 MiB，另加一份最新、从未尝试发送的 Push。替换该 Push 时旧原文留在归档。已尝试、接收中或不确定的 Push 须先解决或明确 abandon，才能替换队列记录。文件配额耗尽拒绝新任务。归档最多 256 个对象/512 MiB，包含未解决对象和本地写入中断留下的孤立对象；不静默删除历史。

`prune --device reader --delivered` 明确移除已投递归档。`abandon --device reader --request-id …` 释放未解决的本地队列槽并保留归档，不撤回或取消可能已发生的设备发布。`cancel --device reader --request-id …` 明确取消设备未完成事务；若已有完成回执仍报告 delivered，取消结果不明确则仍 uncertain。`endpoint --device reader --host … --port 8443` 只改保存地址。`revoke --device reader` 通过固定证书的 TLS 撤销自身 token；响应丢失时保留本地凭据，报告不确定，供明确恢复。

输出是有界 JSON，含请求 ID、字节数及真实状态，不含文档文本、token、标题或私密路径。明确请求的实体清单另含客户端 ID 和用户标签。`uncertain`、`blocked` 退出码为 2；离线 `queued` 是成功的本地入队。`delivered` 始终要求已验证回执。
