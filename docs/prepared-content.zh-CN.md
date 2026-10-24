# 在手机或电脑预处理 EPUB

[English](prepared-content.md)

AP WebUI 可以先在手机浏览器本地预处理 EPUB，再上传；电脑上的 `prepare_epub` 提供相同操作。两者都使用固件的生产 importer，在不改动 `example.epub` 的前提下生成可选的 `example.epub.pdt`。阅读器仍校验源文件和载荷，按当前字体/设置分页，再保存自己的缓存与阅读位置。

这减少首次导入的工作，不消除 SD 读取或首次分页，也不保证特定打开时间。已有有效页表缓存仍是优先路径。

## 在 AP WebUI 预处理

用阅读器当前二维码打开热点网页，选择 EPUB。浏览器支持 WebAssembly 和 Web Workers 时，默认启用 **Prepare EPUB for faster opening**。只需按一次 **Upload**：浏览器本地提取，先上传原书，再按设备返回的实际文件名上传对应预处理文件。

阅读器直接提供 `/prepare-worker.js`、`/prepare-core.js`、`/prepare-core.wasm`，均要求当前 AP session。无需 CDN、互联网、浏览器 secure-context crypto 或伴随服务。源 `File` 传给 worker，UI 线程不先读取整个 archive；worker 执行生产 C++ importer、语义 serializer、PDT1 codec 和 portable SHA-256。

准备中按 **Cancel** 会终止 worker 并阻止后续上传。失败/超时时仍可 **Upload original**。若原书已保存但 sidecar 失败，**Retry faster-opening file** 只重试 sidecar。关闭页面会丢失未发送的临时结果，不会删除已保存的原书。同名选择不会覆盖已有书籍或其进度。

WASM heap 从16MiB增长，硬上限128MiB，stack为256KiB；源EPUB仍受下述64MiB限制。浏览器还需heap外的源buffer/引擎/UI内存，手机可能更早因自身资源不足而拒绝。不能预处理的浏览器仍可上传原书。预处理只做提取，不输出页面；页表由设备自己的字体和设置决定。

## 构建电脑工具

支持Linux/macOS，需要C++17 compiler、CMake3.16+、Python3.12+、OpenSSL开发包和Node.js18+。当前CMake即使只构建工具，也要求Node。

在仓库根目录执行：

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r requirements-dev.txt
pio pkg install -e paperdiy
cmake -S . -B build -DSANITIZE=ON
cmake --build build --target prepare_epub epub_core prepared_content epub_document --parallel 2
ctest --test-dir build -R '^(prepared_content|prepared_epub|epub_document)$' --output-on-failure
```

PlatformIO安装固定的M5GFX，供共用解压器使用；host build不刷机。若已有依赖位于别处：

```sh
cmake -S . -B build -DM5GFX_DIR=/path/to/M5GFX -DSANITIZE=ON
```

macOS/Homebrew OpenSSL可能还需：

```sh
cmake -S . -B build -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)" -DSANITIZE=ON
```

host性能测量应另建`-DSANITIZE=OFF`目录，不能将sanitizer和release耗时直接比较。

## 预处理本地文件

原书和sidecar放在源码工作区外：

```sh
build/prepare_epub /path/to/example.epub /path/to/prepared/example.epub.pdt
```

输出目录须已存在。工具使用hard-link实现原子且不覆盖的发布，只能在APFS/ext4等支持hard-link的本地文件系统生成，不能直接写到FAT32。完成后再复制或上传至SD卡。

成功退出码为0，并输出一个JSON对象，含格式、extractor版本、字节数、包语言及SHA-256，不输出书文。非零表示未发布输出；已有输出不替换，重新生成须另选路径。输入只读打开，提取前后都核对fingerprint。

普通预处理保留受支持的标题、引用、强调、代码、局部语言、显式竖排、有限ruby、MathML和简单表格。精确子集见[EPUB排版](epub-typesetting.zh-CN.md)。可选结构超预算时可以输出可读flat text，`metadataBytes: 0`；诊断时可显式选择：

```sh
build/prepare_epub /path/to/example.epub /path/to/prepared/example-flat.epub.pdt --flat
```

投递前仍需改成源文件的精确名称加`.pdt`。正常阅读推荐默认语义形式。

## 经AP WebUI投递

1. 打开设置书并选 **A**，连接屏幕上的热点并打开当前QR网页；认证仅针对该AP session。
2. 在 **Upload book** 先上传原`.epub`，等 **Uploaded: …**，保留返回的实际文件名。
3. 将sidecar命名为该精确文件名加`.pdt`，用同一表单上传。`.epub.pdt`后缀选择prepared类型。
4. 离开设置后打开原书。sidecar不显示在书库，设备先校验再使用。

若`example.epub`已存在，设备会另分配名称，不覆盖已有书。例如返回`example-2.epub`时，sidecar必须为`example-2.epub.pdt`。改名不改源hash，配对原书的字节仍须完全一致，不能假定请求名就是保存名。

原书已在卡上时，可只传匹配sidecar。AP route要求其leaf filename唯一对应一个普通EPUB；不同目录中两个同名`example.epub`存在歧义，会被拒绝。直接复制至目标源旁边则没有该歧义。

AP接纳的完整`.epub.pdt`名称最多96个UTF-8字节，因此计划成对上传时原书名最多92B；匹配时保留精确大小写。

AP session有idle timeout，上传完成前保持页面打开。丢响应由status/retry协议处理，不要盲目重传原书造成重复副本。**Uploaded**仅证明投递，不证明设备已显示或接受其中语义。

### AP客户端集成

自动化客户端可在AP session开启时使用同一事务协议。从设备QR URL取得当前page session，之后使用`diy_token` cookie。不要将AP token写入脚本、公共skill或源码。POST使用`application/x-www-form-urlencoded`，不是multipart。

| 请求 | 字段/结果 |
| --- | --- |
| `GET /api/status` | `sd`、`busy`、`free`、`uploadGeneration` |
| `POST /api/upload/start` | `name`、`size`、`generation`、新32字符小写hex `requestId`；sidecar加`kind=prepared` |
| `POST /api/upload/chunk` | 返回的`id`、精确下一`offset`、规范base64 `data`；遵守`chunkSize` |
| `GET /api/upload/status?id=…` | receiving与offset，或complete与最终name |
| `POST /api/upload/commit` | `id`；返回`ok`及实际`name` |
| `POST /api/upload/cancel` | `id`；只取消当前拥有的事务 |

原书不带kind；之后用新generation发起sidecar事务，`kind=prepared`、name为返回源名加`.pdt`。它们是两次独立发布，sidecar失败不会回滚或替换原书。

超时后复用相同request identity和metadata，或先查询已有事务。已确认chunk不得再次append，完成commit可按receipt重试。AP session内保留四个RAM completion receipts；重启或session退出结束恢复窗口。预处理、投递、设备接受须分开报告。

## 经SD读卡器复制

阅读器关机并取出卡后，将两个文件放入同目录：

```text
/Books/example.epub
/Books/example.epub.pdt
```

安全弹出后再放回设备。固件持有已挂载文件系统时不要修改卡。USB 导书暂停并默认关闭，请用 AP 上传或安全取出的卡；CDC 保留，MSC 须显式实验编译才可启用。所有权契约和当前硬件验收限制见[USB与恢复](development.zh-CN.md#usb-所有权与固件恢复)。

## 上限、校验与回退

| 项目 | 当前上限/规则 |
| --- | --- |
| 输入 | 一个普通ZIP EPUB，22B至64MiB |
| 提取正文 | 非空UTF-8，最多4MiB |
| 可选语义metadata | 最多352,808B，PDS1/2/3，包括最多16,384个run |
| prepared文件 | 160B头+text+metadata，最多4,547,272B |
| extractor身份 | PDT1 version1、extractor ID2；旧sidecar须重新生成 |
| 语言字段 | 0无英文patterns，1US，2GB |
| 设备页表 | 设备本地计算，不在PDT中 |
| 解包目录EPUB | 阅读器支持，但两个preparer和prepared上传route不支持 |

普通importer的章节/XML/path/archive-entry限制仍适用；不能转换DRM、不支持的加密、坏ZIP、过大章节或任意布局语义。独立限额见[实现报告](architecture-and-implementation.zh-CN.md#5-准入与-epub-提取)。

AP commit校验envelope、transfer payload digest和原文件size。打开时重新校验SD上的prepared字节、原EPUB完整SHA-256、UTF-8和语义区间。未知版本、错配源、损坏或非法metadata不能成为可见prepared内容。优先保留可用缓存，否则普通导入；新有效sidecar可以升级旧flat cache，同时保留anchor。

SHA-256能检查错配/损坏，不证明任意sender生成的text忠实于EPUB。只用可信来源的bundled工具，只接收可信本地sender的prepared文件。AP是显式本地session，不是普通LAN Push服务；native CLI和browser使用相同生产importer，digest是完整性校验而非出版者签名。

PDT含可读书文和metadata，应当作私人书籍数据，不进入公共源码、release或诊断报告。没有Wi-Fi凭据/设备进度不等于内容可公开；源EPUB仍是恢复依据。wire schema和研究见[性能研究](performance-research.zh-CN.md#5-可选主机手机预处理)。

## 重新生成浏览器资源

已提交的`src/prepared_web_assets.h`含ASCII C++形式的gzip JS/WASM数组。固件构建直接消费它们，不运行Emscripten或再生成；再生成是独立的固定工具链步骤。importer、semantic codec、prepared envelope、XML parser或browser adapter改变时须重生成。普通CI用native执行对照已提交WASM；再生成另作pinned检查。

在源码外按[官方emsdk流程](https://emscripten.org/docs/getting_started/downloads.html)安装精确版本：

```sh
git clone https://github.com/emscripten-core/emsdk.git /path/to/emsdk
cd /path/to/emsdk
git checkout eb5eb4b3b2b792f44ded198bfe615c985da5be2c
./emsdk install 4.0.18
./emsdk activate 4.0.18
```

安装固定M5GFX后回到仓库根目录：

```sh
python3 tools/build_browser_prepare.py \
  --emsdk /path/to/emsdk \
  --m5gfx .pio/libdeps/paperdiy/M5GFX \
  --build-dir /path/to/browser-build
```

recipe验证compiler和decompressor源码hash，分别编译C解压器/C++生产代码，再链接优化的modularized worker。gzip时间戳/OS字段固定。公共header不含机器路径或书籍输入；原始JS/WASM、objects和JSON receipt留在指定build目录，不加入公共源码。压缩core资源超过1MiB即失败。记录的integration资源为gzip loader3,991B、WASM42,449B，共46,440B，不含小worker wrapper。

不改已提交header的精确复现检查：

```sh
python3 tools/build_browser_prepare.py \
  --emsdk /path/to/emsdk \
  --m5gfx .pio/libdeps/paperdiy/M5GFX \
  --build-dir /path/to/independent-browser-build --check
```

用Node运行真实committed WASM对照native工具：

```sh
python3 tests/browser_prepare.py --node node \
  --native build/prepare_epub --importer build/epub_core
```

回归从生产synthetic EPUB fixtures生成输入，比完整semantic/flat PDT、对照Node SHA-256，含精确64MiB源、超限拒绝和CPU准备期间hard worker cancel。当前为108成功字节相等、94拒绝，明确含external CSS竖排/ruby/TCY和MathML/simple-table。目录EPUB和host-only故障注入不是browser输入。

真实内存不足测试使用单独的16MiB上限模块：

```sh
python3 tools/build_browser_prepare.py \
  --emsdk /path/to/emsdk \
  --m5gfx .pio/libdeps/paperdiy/M5GFX \
  --build-dir /path/to/browser-oom --max-memory-mib 16 \
  --header /path/to/browser-oom/assets.h
python3 tests/browser_prepare.py --node node \
  --native build/prepare_epub --importer build/epub_core \
  --oom-module /path/to/browser-oom/prepare-core.js
```

它验证source/text allocation exhaustion及后续恢复；测试heap不能覆盖生产header。Node证明native/WASM数据和worker取消，不单独证明实体手机性能或AP成功上传；compiler/runtime许可见[第三方声明](../THIRD_PARTY_NOTICES.md)。

### 浏览器集成证据

实际桌面浏览器在仅绑定`127.0.0.1`临时端口的HTTP服务、生产CSP下运行生产worker/WASM，完成合成原书→sidecar上传，并正确采用服务端改名结果。9,184B原书与706,858B PDT匹配独立参考；无console error/warning。它验证真实页面/worker/assets与本地transport，不证明ESP32 AP、实体手机或后台行为。该试验未向设备上传fixture。遵守[测试主机隔离](development.zh-CN.md#测试主机隔离)，不能为集成测试切换开发电脑网络。
