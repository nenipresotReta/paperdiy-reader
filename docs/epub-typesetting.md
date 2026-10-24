# EPUB typesetting

[简体中文](epub-typesetting.zh-CN.md)

The production EPUB path preserves canonical extracted UTF-8 and keeps styles,
writing mode, ruby, MathML and simple table ranges in a separate bounded
`epub_document::Document`. Bookmarks remain offsets in those unchanged bytes.
The current implementation is a text-reader subset, not a browser or a complete
EPUB/CSS renderer. Device acceptance is recorded separately in the
[architecture report](architecture-and-implementation.md).

## Semantic data and compatibility

`epub::structure::Collector` records block/style/language/resource ranges and
supported object relationships while the existing importer emits canonical
text. It has one checked allocation, with 8,192 records, 2,048 resources,
128 KiB paths, 32 language slots and a 512 KiB aggregate budget. Optional MathML
translations use at most 256 descriptors and 32 KiB of display-only TeX in that
budget. A source/import failure invalidates the whole import; a semantic
allocation failure discards the sidecar and permits readable flat extraction.
A transient memory fallback cannot replace a richer cache or become a permanent
resident-document hit.

Chapter-resume state uses a separate internal `ECS1` collector snapshot, not a
flattened PDS document. It preserves the record tree, resources/path pool,
language interns, MathML TeX, counters and discarded-semantic status. A snapshot
is valid only after a closed chapter or completed extraction. Restart tests
compare canonical text and final PDS1/2/3 bytes at every boundary. This does not
persist an active XML stack or deflate stream. End-to-end extraction-journal
host fault/restart tests have passed; device acceptance is recorded separately.

The document codec reads three versions:

| Codec | Meaning |
| --- | --- |
| `PDS1` | Contiguous heading, quote, emphasis/strong, code/preformatted and language runs; 16-byte header and 16-byte runs |
| `PDS2` | Adds per-run writing/combine flags and canonical ruby group spans; 24-byte header and 20-byte ruby descriptors |
| `PDS3` | Adds bounded MathML translations and table/row/cell spans; 40-byte header, 16-byte math/table/row descriptors and 8-byte cells |

Horizontal books without new objects still encode `PDS1`; declared vertical or
ruby books use `PDS2`, and math/table books use `PDS3`. Up to 16,384 runs,
2,048 ruby groups, 256 formulae, 32 tables, 256 rows, 1,024 cells and 32 KiB TeX
fit the shared maximum of **352,808 serialized bytes**. In-memory runs and
geometry indices require additional checked storage. Decode validates numeric
ordering, coverage, flags, source lengths and UTF-8 boundaries before publishing
an ordinary document. Numeric-only admission is limited to a previously
verified cache manifest; its text spans must be pinned and validated before
layout reads them.

The cache layout family is `0x2026a000`, with EPUB `semanticRevision=2` and
unchanged canonical `TEXT_REVISION=1`. Older semantic revisions are recovery
anchors, not current page/cache hits: source import must recover the missing
annotations. Optional preprocessing still uses the **`PDT1` 160-byte envelope,
extractor ID 2**; there is no `PDT2` magic. Extractor-1 prepared payloads are
rejected and fall back to the original EPUB. Intermediate layout `0x20268000`
is an anchor-migration input, never current pages or direct cached-text reflow. See
[prepared content](prepared-content.md) for delivery and trust boundaries.

## Horizontal prose

TXT, EPUB and Markdown prose share punctuation compression and Han/ASCII
spacing, enabled by default and configured in the AP WebUI. There is no extra
typography settings book. The old NVS `cnStyle` key remains ignored and intact.

| Feature | Rule | Limit |
| --- | --- | --- |
| Punctuation compression | Use native ink bearings; remove at most 14 px while retaining 2 px of ink separation and positive advance | No bitmap scaling or blanket half-width conversion |
| Han/ASCII spacing | Add 0, 2 or 4 px between Han and ASCII letters/digits | Explicit spaces, code/math/rules and unsupported boundaries stop contextual adjustments |
| CJK boundaries | Separate prohibited starts/ends, bounded hanging and paired dashes/ellipses | A bounded policy, not complete Unicode line breaking or JLREQ |
| Latin positioning | Native font-derived fractional advances and kerning, accumulated in 1/64 px | Rounding happens at draw boundaries; bounded lookahead does not optimize an entire paragraph |

Presets select native 24/28 px body fonts and independent margins/line advances.
Horizontal Japanese text currently uses the existing SC-derived body font;
Japanese regional outlines described below belong to the vertical path.
Measurement and drawing share decisions. Reflow keeps the precise saved anchor;
only an actual page turn adopts a new page start.

## Source-declared Japanese vertical layout

Only an explicit supported `writing-mode: vertical-rl` declaration enables
vertical columns. Han characters, `lang=ja`, RTL page progression and the
reader's 180-degree display rotation do not enable vertical layout by themselves.
Columns run top to bottom and advance right to left. A writing-mode transition
starts a new page. The footer remains outside the body box.

The conservative CSS subset reads chapter `<style>`, inline `style`, and local
`<link rel="stylesheet">` resources: at most eight linked sheets, 64 KiB per
sheet, 128 KiB total and 128 stored rules. It recognizes simple type, `.class`,
`#id`, universal and comma-list selectors, specificity/source order, inheritance
and inline overrides. Supported properties include `writing-mode` and
`text-combine-upright`, with documented legacy EPUB/WebKit aliases in the parser.
Descendant/compound selectors, `@import`, media rules, `!important`, embedded
font choice and general CSS layout are not implemented. No remote resource is
fetched.

Native **PaperDIY JP** glyphs are baked from the pinned open Noto source using
Japanese `JAN` `locl`, `vrt2`, `vmtx` and `VORG`. The 24/28 px tables cover the
Japanese differences in the fixed JIS X 0208 repertoire plus native `髙`/`𠮷`
name forms; every required Han override in that repertoire is retained. Unchanged Han outlines reuse the
identical existing bitmap. Unicode 17.0.0 `Vertical_Orientation` distinguishes
upright, sideways and transformed characters; a transformed alternate is used
only when available at the selected native size. Latin/digit runs otherwise
retain native sideways advances. Sideways body ink may use up to three extra
pixels, reserved in the source advance, column spacing and left boundary;
`j`, `W` and `|` keep their complete native ink. No whole-page 90-degree
transform or runtime bitmap resampling is used.

This is not universal Japanese or Korean font coverage. Outside the fixed
repertoire, an existing SC regional glyph or a visible missing-glyph box can
appear. All non-space printable ASCII glyphs fit the bounded native profile.
The fixed JIS set still exceeds the upright cell for fullwidth `ｊ` at both
body sizes and fullwidth `＄` at 28 px; those show a visible box. Two-em
vertical repeat marks, uncommon combining/complex shaping, complete orientation
overrides and the full JLREQ policy remain unsupported.

### Audited font coverage

The offline audit compares the fixed generator repertoires, pinned cmap/JAN
GSUB mappings and committed bitmap records. It counts codepoint records and
source-outline identity, not language-wide shaping or physical visual quality.
The Noto source SHA-256 is
`2a2eae2628df83556c54018c41e20fa532c1b862c5256ae8b3f23feb918d12ca`;
see [font provenance](../licenses/PaperDIY-JP-source.txt).

| Audited set | Result |
| --- | --- |
| SC body records at each native 24/28 px size | 11,428 codepoints, including 10,407 in Han ranges |
| Fixed two-byte `euc_jp` enumeration | 6,879 codepoints; 6,878 have body glyphs; `≒` U+2252 is absent from the source cmap |
| Han-range codepoints in that enumeration | All 6,356 present; this Unicode-range count is not the standard's category count of kanji |
| JAN changes within those Han codepoints | All 3,778 required overrides present at both sizes; 2,578 reuse the identical source outline |
| Total JP body override records | 4,016 at 24 px; 4,018 at 28 px, including the bounded `髙`/`𠮷` supplement where a source difference exists |
| Native ruby records | 439 at both 12/14 px; all 189 mapped kana, ten ASCII digits and supported Latin descenders |
| Already included Han beyond the fixed JP repertoire | 1,168 have different JAN source outlines but retain SC forms; readable fallback is not a Japanese regional guarantee |

Other characters remain absent, such as U+20000. Two-em repeat marks
U+3031–U+3035 are deliberately excluded. The upright fullwidth `ｊ`/`＄` size
limits above remain visible-box fallbacks. Horizontal Japanese still uses SC;
none of these counts claims universal Japanese or Korean typography.

Ruby is an atomic canonical group with one terminal `<rt>`: at most eight base
characters and sixteen kana/Latin-1 annotation characters. Optional `<rb>` and
`<rp>` wrappers preserve their original source spans. Native 12/14 px readings
occupy a reserved gutter on the right of the base column; native descenders
can use up to two extra vertical pixels. Each native advance contributes to
the group extent. All 439 mapped annotation candidates, including `g/j/p/q/y`,
are retained at both sizes. The composer checks actual glyph availability at
the selected size; unavailable readings retain the complete canonical group as
ordinary text. Nested/interleaved ruby, trailing base text,
unsupported annotation repertoires and over-budget groups retain complete
canonical prose. This does not implement all CSS Ruby distribution/overhang
rules. Explicit supported combine spans admit one or two ASCII digits as one
native tate-chu-yoko cell; longer digit runs remain ordinary sideways text.

## Bounded MathML and tables

Supported presentation MathML structures are `math`, `mrow`, `mi`, `mn`, `mo`,
`mtext`, `mfrac`, `msqrt`, `mroot`, `msub`, `msup`, `msubsup`, `mfenced` and the
first presentation child of `semantics`. A bounded converter produces TeX only
in semantic metadata, never in canonical book text. It admits at most 256 nodes,
16 nesting levels and 4,096 TeX bytes per formula; a canonical math span is also
at most 4,096 bytes. The existing `math_layout` and native STIX math fonts render
admitted formulae, reusing the 128 KiB workspace and eight 1,024-command slots.
Recomputation and drawing after admission do not allocate. Each admitted
formula consumes its complete canonical span once; display formulas each use a
separate row even when they were adjacent within one source paragraph.

Unsupported syntax/glyphs, geometry limits or a temporary workspace shortage
show the entire canonical span as readable text. Since historical extraction
already flattened MathML, that fallback can display consecutive operands; it
must not be described as faithful mathematical notation. MathML CSS, the full
attribute model, arbitrary macros and vertical mathematical composition are
outside this subset. MathML inside a table prevents admission of that table's
grid, allowing the ordinary prose/math path to retain the content.

Tables admit ordinary rectangular two-to-four-column rows, without nested
tables, row/column spans or captions outside cells. Columns have equal bounded
widths, native text wrapping and grid lines; header rows are emphasized. Rows
are atomic across pages. A table starts in the remaining horizontal page
space when a whole row fits; prose and grid layout exchange the exact canonical
cursor and occupied bottom edge. Following prose may share the final table
page. A row is at most 8 KiB of canonical source. A row that fits a full body
but not the remaining space continues on the next page; an individually
unrenderable or overheight row falls back to its complete canonical prose. There is no repeated-header, automatic column-width or
full CSS table algorithm. Unsupported structures keep the complete canonical
text, whose original extraction may have joined adjacent cell text.

## Verification and remaining work

Native sanitizer tests cover unchanged flat/semantic extraction, all three
codecs, malformed/torn metadata, UTF-8 offsets, canonical ruby/math/table
boundaries, one native draw-command sequence per formula, separate display
rows, table first-page fit and whole-row continuation, real formula/grid ink,
draw/measure equality, allocation failure,
allocation-free pagination, footer bounds and 180-degree pixel symmetry.
Poisoned lazy-window tests compare actual vertical/mixed-mode reader output
with the full-RAM baseline. The font tests regenerate pinned native glyphs and
verify cell bounds and regional/vertical substitutions.

Those checks do not establish physical e-paper quality, complete EPUB/CSS
conformance, SD power-loss durability or wireless delivery. Remaining semantic
work includes generated list numbering, general superscript/subscript geometry,
image alternatives, broader ruby, nonrectangular tables, complex shaping and
vertical mathematics. New features must retain canonical offsets, cache
versioning, bounded resource use and deterministic complete-text fallback.

Primary design references are [CSS Writing Modes](https://www.w3.org/TR/css-writing-modes-3/),
[CSS Ruby](https://www.w3.org/TR/css-ruby-1/), [Unicode UAX #50](https://www.unicode.org/reports/tr50/),
[JLREQ](https://www.w3.org/TR/jlreq/), [MathML Core](https://www.w3.org/TR/mathml-core/)
and the [HTML table model](https://html.spec.whatwg.org/multipage/tables.html#table-model).
They describe substantially broader systems than the subset implemented here.
