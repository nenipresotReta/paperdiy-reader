# 操作说明

## 书库与文件

使用 FAT32 microSD 卡。把文件复制到卡上并安全弹出，再插入阅读器；AP 网页也可以传书。书库统一列出文件，每屏 22 行，首屏包含一行设置入口及 21 本书，最多 256 本。当前没有文件夹导航或网页文件夹管理。

| 格式 | 单文件上限 | 说明 |
|---|---|---|
| EPUB | 64 MiB | 提取后的正文 ≤4 MiB；也支持解包的 `.epub` 目录 |
| TXT | 4 MiB | UTF-8 |
| `.md` / `.markdown` | 1 MiB | UTF-8；格式与公式范围见 [Markdown](markdown.md) |

扩展名不区分大小写。书籍各自记住阅读位置；首次打开或版式变更时需要分页，后续使用经过校验的缓存。换文件或改路径可能影响旧进度匹配。请保留 SD 卡文件与 NVS，不要为升级格式化卡或擦除整片 Flash。

## 按键

| 场景 | A | B | 短按 A+B |
|---|---|---|---|
| 书库 | 上一本；双击上一屏；长按连续移动 | 下一本；双击下一屏；长按连续移动 | 打开选中项 |
| 阅读 | 上一页；长按连续翻页 | 下一页；长按连续翻页 | 返回书库 |
| 网络设置 | 循环 N → A → U → 关闭 | 切换 R（180°） | 返回书库 |

书库同键两次松开的间隔默认不超过 200 ms 时判为双击翻整屏，超过则按两个单击处理；单击最多等待配置的双击窗口（默认 200 ms） 以区分双击。长按约 600 ms 后开始连续移动。加载或刷新期间的导航操作会丢弃，松开按键后再接受新的操作；底边图标表示忙碌状态。

R 会保存显示方向。翻转后书库与阅读页的 A/B 导航交换，设置页仍保持原物理按键对应；N/A/U 保持同一排，R 位于按键侧。A+B 与电源键不变。

## 中文排版

连续标点压缩和中西文间距默认开启，可在热点网页的 Advanced typography 中调整；设备端没有单独设置页。根据字形实际空白压缩支持的相邻标点，并在相邻汉字与英文字母/数字之间留出少量间距。显式空格不叠加间距；Markdown 代码和公式不参与这两项调整。

## 热点传书与配网

1. 在书库选中首行设置入口，短按 A+B 进入，再用 A 切到热点模式 A。
2. 手机扫描屏幕上的 Wi-Fi 二维码。每次进入 A 都生成新的 SSID 和密码，最多连接一个客户端。
3. 连接成功后，屏幕上的同一个二维码自动变为网页地址。再次扫码打开页面，选择一本书并上传。
4. 页面显示成功后，返回书库打开文件。上传保留中文文件名；同名文件另存，不覆盖已有书。

屏幕始终保留热点 SSID 和密码；手机断开后，二维码回到连接热点的状态。手机可能提示热点没有互联网，请保持连接。

传输分块进行，只有完整提交后才加入书库。同一会话中的丢响应会自动有限重试。若仍提示失败，请先查看网页书目再重试；不要假定关闭网页、断开热点或设备重启后仍能续传。未完成的传输会清理，已有书籍不受影响。

在网页的 Home Wi-Fi 输入家庭网络名称与密码并保存，再切到 N 模式连接。保存失败不会改写旧配置；密码不会回显或写入日志。热点凭据只保留在当前会话，家庭 Wi-Fi 凭据保存在 NVS（明文）。

AP 在 5 分钟无操作后关闭。后台书目轮询不续期；传输只要持续取得进展就保持活动，45 秒无进展会取消。网页仅在本地 AP 内网提供 HTTP，没有 TLS；二维码内的会话凭据仅在当前热点有效。

## N / A / U

- **N**：连接已保存的家庭 Wi-Fi，显示 SSID、连接状态和 IP；不提供网页、消息推送或出站订阅。连接中断后重新给予 20 秒连接窗口。
- **A**：临时热点、网页传书与家庭 Wi-Fi 配置。
- **U**：预留入口，尚未实现 USB 大容量存储，不会向电脑挂载 SD 卡。

未激活项带 `¬`。离开设置页即关闭网络；重启回到设置时 N/A/U 均未激活。接着 USB 不会自动开启网络，也不会让 N 模式变成网络服务。阅读设置、关机图片管理同样仅在 A 热点网页提供。未来内网交互仅有[设计稿](lan-design.md)。

## 关机、开机与电量

同时按住 A+B 至少 2 秒，看到关机画面后再松开。关机先保存位置；如果提示保存失败，仍保持开机，重试即可。

开机恢复关机前的阅读页、书库位置或设置页；首次没有记录时进入书库。阅读恢复的是保存的文本位置，版式改变后页码可能不同。公开固件没有私人关机图；私人构建可通过仓库外的 `PAPERDIY_SHUTDOWN_HEADER` 提供。

书库显示电池状态，底边图标表示设备状态。空闲时按条件休眠，首次按键应直接执行操作；网络工作期间暂停手动轻睡眠。充电指示灯由充电状态决定。功耗设计与验证范围见 [power.md](power.md)，实际电流仍需仪器测量。

## WebUI settings

All controls use English labels. Open the existing AP Web UI; no extra settings
book is added on the reader. Save explicitly; settings persist offline.

| Control | Options / default |
| --- | --- |
| Reading preset | Compact: native 24 px, 34 px line spacing, 18/24 px margins; Standard (default): 28/40 px, 24/30 px margins; Relaxed: 28/46 px, 36/42 px margins |
| Automatic English hyphenation | On by default; applies where the document supports English hyphenation |
| Alignment | Justified (default) / Left aligned |
| Compress adjacent Chinese punctuation | On by default, under Advanced typography |
| Chinese–Latin spacing | Off / Compact (2 px) / Standard (4 px, default) |
| Double-click interval | 100–400 ms, default 200 ms; measured between releases |
| Screen refresh | Clarity / Balanced (default) / Less flashing |
| Sleep background ink | 0–100%, default 50%; monochrome ordered dithering approximates intermediate values |

Presets affect prose, not interface labels or the native 24 px code font. Math
keeps its native 28/20/14 px sizes. Typography changes apply when a book is next
opened: pagination is rebuilt around the saved source position, not a page number.
Balanced allows 10 consecutive partial reading updates before a full refresh;
Clarity allows 4 and Less flashing 24. Scene changes still use a full refresh.

Upload a PNG, JPEG or WebP image up to 12 MiB under Sleep image. The browser
converts it locally into a bounded monochrome mask and shows a preview; white
areas are transparent over the dimmed page. The image keeps its aspect ratio,
fits within 540×960 without upscaling, and is aligned bottom right. The SD card
stores the custom mask; it is never included in public source or firmware.
Remove image disables the overlay and removes the two custom upload slots when
the SD card is available. Use firmware default restores the compiled fallback
(private artwork on a private build, no artwork on the public build). Missing or
invalid custom artwork falls back to that firmware default. Book files and
reading records are unaffected.
