# EPUB 排版

[English](epub-typesetting.md)

生产路径保留规范提取的 UTF-8 原文，将样式、书写方向、注音、MathML 和简单表格区间另存于有界 `epub_document::Document`。书签始终对应这些未改变的字节。本实现是文本阅读器的有限子集，不是浏览器或完整 EPUB/CSS 引擎。实机证据另见[架构报告](architecture-and-implementation.zh-CN.md)。

## 语义数据与兼容

`epub::structure::Collector` 在原提取器输出原文时记录块、样式、语言、资源及对象关系。它使用一次受检查分配：最多 8,192 条记录、2,048 个资源、128 KiB 路径、32 个语言槽，总预算 512 KiB。MathML 翻译最多使用其中 256 个描述符和 32 KiB 仅供显示的 TeX。源文件或导入失败使整次导入无效；可选语义分配失败则丢弃旁路数据，允许保留可读的扁平提取。临时内存回退不能覆盖较丰富的缓存，也不能成为永久 resident 命中。

章节恢复另用内部 `ECS1` Collector 快照，不能由扁平 PDS 文档反推。快照保留记录树、资源和路径池、语言驻留表、MathML TeX、计数及语义已丢弃的状态；只在章节完整关闭或提取完成后使用。重启测试逐边界比较原文和最终 PDS1/2/3 字节。它不保存活动 XML 调用栈或 deflate 流；提取日志已有端到端主机故障/恢复测试，实机验收另行记录。

文档编解码器读取三个版本：

| 格式 | 内容 |
| --- | --- |
| `PDS1` | 连续的标题、引用、强调/粗体、代码/预格式和语言 run；16 B 头、每 run 16 B |
| `PDS2` | 增加 run 的书写/纵中横标志和原文 ruby 区间；24 B 头、每 ruby 描述符 20 B |
| `PDS3` | 增加有界 MathML 翻译和表/行/单元格区间；40 B 头，公式/表/行各 16 B，单元格 8 B |

没有新对象的横排书仍输出 PDS1；声明竖排或 ruby 使用 PDS2；数学/表格使用 PDS3。最多 16,384 个 run、2,048 组 ruby、256 个公式、32 个表格、256 行、1,024 个单元格及 32 KiB TeX，共用 **352,808 B 序列化上限**。内存 run 和几何索引另行受检分配。普通解码在发布前检查数值顺序、覆盖、标志、原文长度和 UTF-8 边界。仅校验数值的入口只允许用于已验证 cache manifest；布局读取前必须 pin 并验证相应正文区间。

当前缓存版式族为 `0x2026a000`，EPUB `semanticRevision=2`，原文 `TEXT_REVISION=1` 不变。旧语义版本仅提供恢复锚点，不能命中当前页表；必须重新导入源文件恢复缺失注释。可选预处理仍为 **160 B 的 `PDT1` envelope，extractor ID 2**，没有 PDT2 magic。旧 extractor-1 载荷被拒绝并退回源 EPUB。中间版式 `0x20268000` 只用于锚点迁移，不能提供当前页或直接缓存正文重排。投递与信任边界见[预处理](prepared-content.zh-CN.md)。

## 横排正文

TXT、EPUB 与 Markdown 正文共用默认开启的相邻标点压缩和汉字/ASCII 间距，均由 AP WebUI 配置，不增加排版设置书。旧 NVS `cnStyle` 原样保留且忽略。

| 功能 | 规则 | 边界 |
| --- | --- | --- |
| 标点压缩 | 根据原生墨迹 side bearing，最多压缩 14 px，保留 2 px 墨迹间隔及正 advance | 不缩放位图，不统一改成半宽 |
| 汉字/ASCII 间距 | 汉字与 ASCII 字母/数字之间增加 0、2 或 4 px | 显式空格、代码/公式/规则线及不支持的边界阻止上下文调整 |
| 中日文断行 | 区分禁行首/禁行尾、有界悬挂和成对破折号/省略号 | 不是完整 Unicode 换行或 JLREQ |
| 拉丁字形位置 | 原生字体导出的分数 advance/字偶距，以 1/64 px 累加 | 绘制边界才取整；有界前瞻不优化整个段落 |

阅读预设选择原生 24/28 px 正文及独立页边距/行距。横排日文仍使用既有 SC 派生字库；下述日文区域字形属于竖排路径。测量与绘制共用决策；重排保留精确锚点，只有实际翻页才采用新页起点。

## 源文件明确声明的日文竖排

只有受支持的显式 `writing-mode: vertical-rl` 声明启用竖排。汉字、`lang=ja`、RTL 翻页顺序及阅读器的 180° 显示旋转都不会单独启用竖排。列内从上到下、列间从右到左；书写模式切换另起一页。页脚始终在正文框外。

保守 CSS 子集读取章节 `<style>`、内联 `style` 和本地 `<link rel="stylesheet">`：最多八个链接样式表，每表 64 KiB，合计 128 KiB，最多 128 条规则。支持简单标签、`.class`、`#id`、通配及逗号列表选择器、优先级/源顺序、继承和内联覆盖。属性包括 `writing-mode`、`text-combine-upright` 及解析器列出的旧 EPUB/WebKit 别名。不支持后代/复合选择器、`@import`、媒体规则、`!important`、内嵌字体选择或一般 CSS 布局；不下载远端资源。

原生 **PaperDIY JP** 来自校验固定的开放 Noto 源，烘焙日文 `JAN` 的 `locl`、`vrt2`、`vmtx` 与 `VORG`。24/28 px 表覆盖固定 JIS X 0208 集合所需的日文差异，以及 `髙`/`𠮷` 两个姓名字形；集合内所需汉字覆盖均保留。相同轮廓复用既有 bitmap。Unicode 17.0.0 `Vertical_Orientation` 区分直立、侧倒和变体；变体只在所选原生字号存在时使用。其他拉丁字母/数字保留侧倒 advance。侧倒正文墨迹最多允许额外 3 px，advance、列距与左边界同时预留，`j`、`W`、`|` 保留完整原生墨迹。不整页旋转 90°，也不在运行时重采样位图。

这不保证全部日文或韩文字形。固定集合以外可能使用 SC 区域字形或可见缺字框。所有非空格可打印 ASCII 均适合当前原生边界；固定 JIS 中，全宽 `ｊ` 在两个正文字号、全宽 `＄` 在 28 px 仍超过直立单元，显示缺字框。双 em 竖排重复符号、不常见组合/复杂 shaping、完整方向覆盖及全部 JLREQ 仍未支持。

### 可量化的字体覆盖

离线审计比较固定 generator 字符集合、pinned cmap/JAN GSUB 和已提交 bitmap records。数字表示 codepoint records 与源轮廓身份，不等于全语言 shaping 或物理显示质量。Noto 源 SHA-256 为 `2a2eae2628df83556c54018c41e20fa532c1b862c5256ae8b3f23feb918d12ca`，见[字体来源](../licenses/PaperDIY-JP-source.txt)。

| 审计集合 | 结果 |
| --- | --- |
| SC 原生24/28 px 每字号正文 records | 11,428个codepoints，其中10,407位于Han区间 |
| 固定双字节`euc_jp`枚举 | 6,879个codepoints；6,878有正文glyph；`≒` U+2252不在源cmap |
| 该枚举中位于Han区间的codepoints | 6,356个全有；这是Unicode范围统计，不是标准中“汉字类别”数量 |
| 这些Han codepoints中的JAN差异 | 两字号均保留全部3,778个所需override，另2,578复用相同源轮廓 |
| JP正文override总数 | 24px为4,016，28px为4,018；包括有源差异时的有界`髙`/`𠮷`补充 |
| 原生ruby records | 12/14px均439个，含全部189个映射kana、十个ASCII数字及受支持Latin descenders |
| 固定JP集合以外、已含于正文的Han | 1,168个有不同JAN源轮廓但仍保留SC形；可读fallback不代表日文区域字形保证 |

其他字符仍可能没有glyph，例如U+20000。双em重复符U+3031–U+3035明确排除。上述全宽`ｊ`/`＄`在对应字号的直立边界仍以缺字框回退。横排日文仍用SC；这些数字均不声称通用日文字形或韩文排版覆盖。

Ruby 是原文中的一个原子组：只有一个末尾 `<rt>`，最多八个 base 字符和十六个 kana/Latin-1 注音字符。可选 `<rb>`/`<rp>` wrapper 保留原始区间。原生 12/14 px 注音占正文列右侧独立 gutter；descender 最多增加 2 px 垂直空间，每字真实 advance 累计组高度。两个字号均保留全部 439 个映射候选，含 `g/j/p/q/y`。排版前检查当前字号是否真有 glyph；不可用时显示完整 canonical 组为普通文字。嵌套/交错 ruby、注音后的 base、其他字符集及超预算组全部保留原文回退，不实现完整 CSS Ruby 分配/悬挂。受支持的显式 combine 区间只允许一或两个 ASCII 数字组成原生纵中横单元；更长数字串仍侧倒。

## 有界 MathML 与表格

支持 presentation MathML 的 `math`、`mrow`、`mi`、`mn`、`mo`、`mtext`、`mfrac`、`msqrt`、`mroot`、`msub`、`msup`、`msubsup`、`mfenced`，以及 `semantics` 的首个 presentation child。有界转换器只将 TeX 放入语义 metadata，不改原文。每公式最多 256 个节点、16 层、4,096 B TeX；原文公式区间也最多 4,096 B。既有 `math_layout` 和原生 STIX 字形复用 128 KiB workspace 及八个各 1,024 条命令的槽；admission 后重算/绘制不分配。公式一次消费完整原文区间；display 公式各占一行，即使源码同段相邻。

不支持的语法/字形、几何超限或临时 workspace 不足时显示完整原文区间。历史提取本已扁平化 MathML，因此回退可能是相邻操作数，不能称为保真数学记号。MathML CSS、全部属性、任意宏及竖排数学不在子集内。表格中含 MathML 时不接纳其 grid，交给普通正文/数学路径保留内容。

表格只接纳规则的二至四列矩形，不含嵌套表、row/column span 或 cell 外 caption。列等宽，原生文本换行、表头强调并绘制网格。行不跨页拆开；剩余页高够整行就接在横排正文后，两个 composer 交换精确原文 cursor 与已用 bottom。最后一行后的正文可继续同页。每行原文最多 8 KiB；整页能放下、剩余高度放不下的行移到下一页；独立仍无法绘制或过高的行保留完整原文回退。不自动重复表头、不自动算列宽，也不是完整 CSS table 算法。不支持的结构保留完整原文，历史提取可能已把相邻 cell 文字连接。

## 验证与剩余范围

Native sanitizer 测试覆盖 flat/semantic 原文字节一致、三个 codec、损坏/截断 metadata、UTF-8 偏移、ruby/math/table 原文边界、每公式一份原生命令、display 独行、表格首面适配和整行续页、真实墨迹、测量/绘制一致、分配失败、无分配分页、页脚及 180° 像素对称。Poisoned lazy-window 用实际竖排/混合方向 reader 对照 full-RAM 基线。字体测试从固定源再生，校验单元边界、区域/竖排替换。

这些主机检查不能单独建立物理 e-paper 质量、完整 EPUB/CSS 一致性、SD 断电耐久或无线投递结论。剩余语义范围包括生成列表编号、一般上下标几何、图像替代、更广 ruby、非矩形表、复杂 shaping 和竖排数学。扩展必须保持原文偏移、缓存版本、资源上限及确定性的完整原文回退。

主要设计依据：[CSS Writing Modes](https://www.w3.org/TR/css-writing-modes-3/)、[CSS Ruby](https://www.w3.org/TR/css-ruby-1/)、[Unicode UAX #50](https://www.unicode.org/reports/tr50/)、[JLREQ](https://www.w3.org/TR/jlreq/)、[MathML Core](https://www.w3.org/TR/mathml-core/)、[HTML table model](https://html.spec.whatwg.org/multipage/tables.html#table-model)。它们覆盖的系统远比本实现子集广。
