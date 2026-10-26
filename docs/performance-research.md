# Opening performance, preprocessing and multilingual typography

[简体中文](performance-research.zh-CN.md)

This document separates implemented behavior, engineering deductions and proposals. It covers opening costs, cache trust, chapter-boundary recovery, browser preparation and bounded multilingual typography. Benchmarks use synthetic or publicly redistributable fixtures; personal device operations and reading records do not belong here. The source map is in [architecture and implementation](architecture-and-implementation.md).

## 1. Optimize the delay the reader actually experiences

“Opening time” comprises several different workloads:

| Case | Required work | Unnecessary work to eliminate |
| --- | --- | --- |
| First open of a new EPUB | Validate source, decode relevant resources, extract content, build layout, show a page | Per-glyph painting during measurement; repeated whole-archive reads |
| Same layout, unchanged book | Validate reusable identities, read cache, restore anchor, draw page | ZIP/XML parsing and full pagination |
| Changed font or margins | Reuse verified text/semantics, calculate new page geometry | Re-importing the source merely because layout differs |
| Resume after reboot | Re-establish source trust, load content/layout and exact anchor | Rewriting unchanged state or redoing valid extraction |
| Session reopen | Reuse source validation within a controlled SD ownership epoch | Rehashing the same unchanged source repeatedly |
| Interrupted preparation | Recover a complete prior generation or valid checkpoint | Publishing half-built content or forgetting a reading anchor |

Record at least `source_verify_ms`, `cache_read_ms`, `extract_ms`, `semantic_ms`, `paginate_ms`, `cache_write_ms`, `first_paint_ms`, total time, bytes read/written, and cache outcome. A cache-hit count without byte counters can hide a full multi-megabyte source reread. Host timing and e-paper refresh timing are separate measurements.

The first milestone reduced complete-open work without changing progress semantics. Progressive pagination displays a measured page near the saved source anchor before computing every later EPUB page. Unknown page numbers/totals are explicitly displayed as `…`; the footer indicates ongoing index/cache work. Complete-index time and first-readable-page time must now be measured separately.

## 2. Cost model and achievable limits

Let `A` be archive bytes, `X` the decoded XHTML bytes visited, `T` extracted UTF-8 text bytes, `E` ZIP entries, `S` spine entries, `M` manifest entries, `G` glyphs, and `P` pages. Let `R`/`W` denote measured sequential SD throughput, and `F` display refresh duration.

A strict cold open with all work completed before first display has these component costs:

```text
source validation:  A / R                     if all source bytes must be hashed
import:             relevant ZIP reads + inflate(X) + XML(X)
index:              at least Ω(G)              for an exact full page count
cache publication:  cacheBytes / W + readback / R
first display:      F
```

The current progressive EPUB path moves complete indexing and checked cache
publication after first display. Its critical path is still full source/content
validation or extraction, anchor resolution, one local page measurement and
refresh. It does not make `Ω(G)` work disappear: that work continues in bounded
foreground-loop slices. Early display therefore reduces perceived delay without
claiming reduced total CPU or SD work.

The baseline SD SPI configuration used 4 MHz. Even an impossible zero-overhead transfer at that clock can carry only 500,000 bytes/second: one full read of a 64 MiB archive has a lower bound of roughly 134 seconds. At 20 MHz the corresponding ideal bound is still about 27 seconds. FAT lookup, SPI framing, card latency and hashing only add time. These are physical upper-throughput calculations, **not** measured transfer rates. Software cannot obtain subsecond strict validation of such a file over either link. The working tree now tries 20/10/4/1 MHz and 400 kHz, validates the root directory at mount, and lowers the next-mount ceiling after a detected read failure. Higher clocks still need real-card tests; card capacity is not a performance guarantee.

The Arduino [PaperDIY microSD example](https://docs.m5stack.com/en/arduino/paperdiy/microsd) inherits the general 4 MHz `SD.begin()` default. That is not a documented board speed ceiling. The official [PaperDIY factory firmware](https://github.com/m5stack/M5PaperDIY-UserDemo/blob/main/main/hal/hal_tf_card.cpp) uses `SDSPI_HOST_DEFAULT()` without lowering its clock. Its pinned ESP-IDF 5.5.1 [host default is 20 MHz](https://github.com/espressif/esp-idf/blob/v5.5.1/components/esp_driver_sdspi/include/driver/sdspi_host.h). This supports a controlled 4/10/20 MHz read-and-hash comparison on the actual card, followed by conservative fallback. It does not establish a universal throughput or reliability result for every card or the Arduino transport.

Full correctness also constrains cache shortcuts. A same-size, same-mtime file can have changed bytes. Sampling the beginning/end or hashing only the ZIP directory cannot prove that all compressed data is unchanged. A CRC is useful for accidental corruption detection, but does not make uninspected content trustworthy. A fast path can instead rely on an explicit, currently owned storage session: validate once, then reuse only until an event that could change the source invalidates that trust.

A layout-only change should approach `O(T + P)` read/measure/index work, with no archive decompression. Complete EPUB caches now optionally include an authenticated block manifest: metadata and page offsets remain global, while only the current page and bounded context blocks are read into the existing absolute-offset address space. A cold open still validates the entire source, and the 4 MiB text reservation remains. This reduces cache text I/O; it does not establish constant cold-open time or streaming ZIP extraction.

### Bound every potentially superlinear path

The baseline importer searches ZIP entries by name and searches the manifest for every spine reference. At worst those are `O(S × E)` and `O(S × M)` string comparisons. A sorted index (`O(E log E)` build, `O(log E)` lookup) or bounded hash index removes repeated scans. This matters especially when a book has thousands of images or small chapters. Index memory and allocation failures need explicit limits.

A glyph loop that repeatedly searches backward to the start of its line can turn a linear pass into quadratic work for long lines. Context should be carried forward or computed once per line. Bounded lookahead must use a fixed candidate budget rather than recursively explore every break sequence. Zero-width runs, repeated punctuation, unbreakable identifiers and deeply nested markup need adversarial fixtures, not only ordinary prose.

Yields should follow time/work budgets. A one-tick delay per ZIP entry or small chunk can accumulate seconds even when computation is cheap. Conversely, removing all yields can starve input, network or watchdog tasks. Instrument maximum uninterrupted work and yield counts alongside wall-clock time.

## 3. Cache layering and integrity contract

Use independent identities for these products:

```text
source bytes + extractor version
    -> normalized text + semantic spans
       text/semantics digest + typography/font/geometry identity
           -> page/line index
              current page + display state
                  -> pixels
```

A layout change invalidates only layout products. An extractor change invalidates content and requires progress migration if normalized text changes. A font change can invalidate page metrics without changing source text. Temporary fallback must not overwrite richer previously validated content.

A verification memo may be scoped to one source-validation/storage epoch. It must bind path, size, mtime and complete digest, with invalidation on remount, card change, read/write error, source mutation and every USB block-device handoff. The current sleep guard also drops proofs before disabling the SD card edge ISR: a remove/reinsert pair during that interval cannot be ruled out. The loaded page survives, but reopening after sleep revalidates source bytes. Modification timestamps can reject a candidate cheaply, but they do not replace an initial digest. Do not persist such a memo across reboot as if the card had remained exclusively owned.

Cache admission still verifies sizes, checksums, UTF-8/source boundaries, semantic record ordering and references, and page offsets before exposing state. Write a complete temporary product, read it back, and publish it after validation. Keep the previous generation until progress migration succeeds. A content cache and a layout cache need independent commit/fallback paths so a pagination failure cannot destroy the only copy needed to restore a bookmark.

The local visible window and canonical prefix remain separate vectors; only a
fully complete canonical index can enter the complete-cache schema. A separate
two-slot partial journal now persists a bounded canonical prefix and its next
cursor. Its identity binds layout, extraction/semantic revisions, source path,
the freshly validated dependency graph, complete text and serialized semantics.
Recovery extends background work without moving the visible page or exact
anchor. An incomplete prefix never supplies a whole-book total.

Each checkpoint slot holds at most 65,536 offsets; both slots together use at
most 524,624 bytes per book. The first save begins after 32 pages, and later
saves require at least 128 more pages and five seconds since a successful save.
Reads, writes and readback advance in at most 8 KiB steps. Cancellation leaves
the prior valid slot and complete-cache generations intact. Truncated, torn,
wrong-identity and malformed-prefix slots fail closed; a fresh restart must
validate the source again before considering either slot.

### Implemented chapter extraction recovery

`Importer::begin()` constructs the validated chapter plan; `stepChapter()` imports
one complete chapter, and synchronous `run()` follows the same path. The normal
`sd_open::Job` advances one chapter or one journal I/O step per poll. Byte-chunk,
parser and tree-walk observers service cooperation and latch cancellation;
they do not suspend a DOM or make a blocking filesystem call preemptible.
Cancellation becomes terminal after the current chapter and required commit.
Boot and preparation tools retain the synchronous wrapper.

A separate `.exttext` spool holds at most 4 MiB of canonical prefix bytes.
Two `.ext0`/`.ext1` `PXE1` commits each have a 192-byte header and at most 1 MiB
of metadata, for at most 6 MiB plus 384 bytes per book. A commit binds full
source/dependency proof, the framed chapter plan, revisions, cursor, prefix
length/digest and explicit `ECS1` Collector state. ECS1 has a 96-byte header,
a 525,536-byte codec ceiling and a 398,560-byte maximum at default capacities.
It preserves records, resources/paths, language interns, math/TeX and status;
flattened PDS alone cannot reconstruct the mutable Collector tail.

Only a closed chapter is a restart frontier. Text appends cannot alter its
committed prefix, while final semantic clipping can update earlier records,
so every commit stores the complete Collector snapshot. The first nonempty
prefix is eligible, then at least 256 KiB or 64 additional chapters and one
second are required; cancellation and completion request a save. New suffix,
metadata and readback advance in at most 8 KiB transfers. Open, flush and rename
remain synchronous. Source and every already-read directory dependency are
revalidated before resume; size/mtime are not proof. An invalid newer commit
may fall back to an older valid slot. If neither survives, setup/title/Collector
state is recreated before fresh append.

The SD policy admits Collecting, Complete and deterministic Budget/Invalid
fallbacks. Allocation or whole-import flat retries remain volatile and cannot
replace a richer semantic product. No partial extraction publishes an ordinary
book key, bookmark or NVS progress. A completed extraction can survive later
pagination interruption independently of page-index checkpoints. Every poll
and publication requires the same nonzero storage lease, even if the optional
journal is unavailable. Current tests cover chapter-by-chapter restart and
byte-identical canonical text/PDS1/PDS2/PDS3; hardware cold-import and interruption
measurements require a separate reproducible benchmark.

### Implemented first-page and deferred-publication path

New/imported EPUBs and EPUB reflows measure one page around the exact saved
source offset. The display start may look back through UTF-8/Latin word context,
while the original offset remains the persistence authority. The visible window
holds at most 32 pages. Forward movement measures only the next needed page;
backtracking before a deep local window uses at most 16 page measurements from
a seed 4 KiB earlier, followed by bounded word/UTF-8 adjustment, rather than
scanning from the book's beginning.
These provisional pages can overlap slightly when the layout changes, but must
not skip the next unread byte.

The canonical index starts at byte zero and advances at most two pages per
application-loop visit, with a 12 ms soft time budget. One page can exceed that
budget; existing layout cooperation still services the watchdog. Work only
runs between foreground actions in Reading/Library while buttons are idle.
Completion does not replace the visible page: a footer-only update announces
the exact total, and the next deliberate navigation adopts canonical boundaries.
The session's document pointer, text length, bookmark key and layout/style
identity prevent stale jobs from applying to a replaced book.

After a checked progress save, `DeferredCacheWrite` snapshots complete metadata
and page offsets, borrows stable text and writes at most 8 KiB per step. Digest
readback and validation of old primary/backup generations are incremental too.
It checks the storage lease before each step; cancellation cannot clean a
replacement card. Temporary output is published only after full checksum
validation. Open, flush and rename remain indivisible filesystem calls, so the
byte limit does not promise a hard response-time bound.

This implementation applies to EPUB. Compatible complete page caches retain
the direct path; TXT/Markdown still use synchronous indexing. It does not
stream XHTML or bypass cold whole-source validation.
Pending background work keeps Reading/Library active, so reduced first-page
latency is not itself evidence of lower energy consumption. Entering Settings
pauses the job. Revoking storage ownership cancels SD publication but allows
indexing of the still-owned RAM document to finish; replacing the book/card or
layout and shutdown cancel the corresponding job explicitly. Measurement boundaries and the synthetic regression matrix are in [section 9](#9-benchmark-and-regression-plan).

### Implemented cache text windows

`DeferredCacheWrite` computes 8 KiB text-block SHA-256 digests while publishing
the complete cache, then optionally publishes a checked `.chunks` manifest.
The manifest binds the complete-cache digest, global metadata/page prefix and
each text block; it is at most 16,504 bytes for a 4 MiB document. A missing,
stale or damaged optional manifest uses the independent complete-cache path.
Source trust is still re-established before a window is exposed.

Page navigation pins authenticated blocks before moving the anchor. Each block
is verified in scratch storage before copying, and the exposed span contains
only complete, validated UTF-8. Semantic metadata can be decoded without
touching unloaded text, then text-dependent boundaries are checked inside the
pinned span. Horizontal, vertical and mixed-mode renderers honor its end.
An exceptional semantic span can fall back to verified full materialization;
a failed pin preserves the previous readable span and exact location.
Lease loss permits already verified RAM bytes but no new SD reads.

Synthetic host regression demonstrates a roughly 1 MiB EPUB cache opening at a
deep saved page with fewer than 128 KiB of cache reads and a span below 64 KiB,
while still hashing the complete source. Actual reader tests poison every
unpinned byte under ASan and compare page ends and drawing geometry against
full-resident plain, semantic horizontal, vertical and mixed-mode baselines.
These are correctness/byte-work results, not device timing measurements.

EPUB semantic revision 2 is explicit in the unchanged-size v3 extension.
Older revisions 0/1 are read only for bookmark migration, even if their layout
family matches. They cannot supply normal pages or reflow text. Reimport still
prefers the current content key's exact offset when canonical bytes agree.
TXT/Markdown retain semantic revision 0 and audited text-family reuse.
The final native-ruby geometry uses layout family `0x2026a000`. Intermediate
`0x20268000` caches are validated only for exact-anchor migration; they supply
neither current pages nor reflow text. Semantic revision 2 and prepared
extractor revision 2 are unchanged.

## 4. What other reading engines demonstrate

[CrossPoint's documented cache layout](https://github.com/crosspoint-reader/crosspoint-reader/blob/develop/docs/file-formats.md) separates book metadata from section/page products. [Its section implementation](https://github.com/crosspoint-reader/crosspoint-reader/blob/develop/lib/Epub/Epub/Section.cpp) uses versioned complete/incomplete/partial states and stores completed pages during a suspended build. The useful pattern for this project is section-local work with explicit publication state. Its memory target, formats and UI differ from ours; its timings are not a PaperDIY benchmark.

[KOReader's CREngine document implementation](https://github.com/koreader/crengine/blob/master/crengine/src/lvtinydom.cpp) maintains separate DOM, cache-format and formatting versions, and contains bookmark/XPointer compatibility handling. [The document view](https://github.com/koreader/crengine/blob/master/crengine/src/lvdocview.cpp) also separates rendering invalidation from document state. The useful lesson is explicit compatibility identity at each layer. Replacing this project wholesale with that larger engine would be a porting, memory and licensing project, not a small optimization.

These primary sources inform architecture. This project should implement its own bounded mechanisms and review licenses before adopting any source. Repository branch links can change; a production dependency or copied algorithm implementation needs a pinned revision and provenance record.

## 5. Optional host and phone preprocessing

The [prepared-content guide](prepared-content.md) contains the implemented native and browser preparation builds and AP delivery workflow.

### What can be moved off the reader

The expensive, source-dependent work is ZIP traversal, inflate, XML validation, normalization and semantic extraction. A desktop agent can run that work once during delivery. A phone can run it while its browser page is open. Device-dependent layout can also be precomputed, but only with the exact font metrics, typography version, geometry and language policy used by firmware.

The strongest first contract is **verified prepared content**, not pre-rendered screenshots:

- Preserve the original EPUB as the authoritative source and keep it independently readable.
- Generate text and bounded semantic metadata with the same C++ importer used by the device.
- Bind the prepared payload to the original full source SHA-256 and an extractor ID.
- Bind metadata and text together with a payload digest; use explicit little-endian fields and byte counts.
- Treat prepared content as a disposable optimization. An absent, stale or invalid sidecar falls back to the on-device importer.
- Keep page offsets out of this first format. Device font/settings remain authoritative for pagination.
- Preserve the source book's path/content identity for bookmarks; the sidecar must not become a second unrelated book.

A source hash inside a bundle is not a proof that an arbitrary sender produced truthful extracted text. Hashes detect mismatches/corruption and identify an expected source; accepting prepared content relies on the user's explicit local preparation/delivery trust, plus bounded validation. An authenticated transport authorizes that sender; it does not mathematically prove equivalence of arbitrary extracted text to EPUB contents. Re-running the entire importer would defeat the offload benefit.

### Prepared-content envelope

The initial `PDT1` interchange uses a 160-byte header, followed by exact text bytes and serialized metadata. This is an interchange envelope, separate from internal page-cache structs:

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 4 | ASCII `PDT1` |
| 4 | 4 | Format version, little-endian |
| 8 | 4 | Header bytes, exactly 160 |
| 12 | 4 | Extractor identity |
| 16 | 8 | Original EPUB byte size |
| 24 | 4 | Text byte size |
| 28 | 4 | Metadata byte size |
| 32 | 32 | SHA-256 of original source bytes |
| 64 | 32 | SHA-256 of text followed by metadata |
| 96 | 4 | Language enum: 0 none, 1 English US, 2 English GB |
| 100 | 60 | Reserved zero bytes |

Unknown versions, extractor IDs, language values, nonzero reserved bytes, oversized lengths, inconsistent lengths, invalid UTF-8, bad metadata and wrong digests are rejected. The source remains limited to 64 MiB and extracted text to 4 MiB. Current metadata is limited to 352,808 bytes through the shared `epub_metadata_limits` definition and uses the same bounded serializer and validator as the device, not host pointer layouts. A zero-length metadata section means explicit flat fallback, not invented semantic completeness.

The current extractor ID is 2; older prepared payloads must be regenerated. The sidecar is named after the original source, for example `example.epub.pdt`, and is not listed as a book. `src/prepared_epub.h` is the authority for the envelope. `src/epub_document.h` supplies the bounded PDS metadata codecs. Metadata preserves local run language in addition to the package language in the header.

### Desktop/agent path

The implemented `prepare_epub` C++ CLI prepares a source into a `.pdt` sidecar with the production importer and semantic encoder, checks source SHA-256 before and after preparation, and reports machine-readable status without book text. It validates generated semantic metadata and refuses to replace an existing output. Temporary output is published atomically within its destination directory.

```sh
cmake -S . -B build -DSANITIZE=ON
cmake --build build --target prepare_epub
build/prepare_epub example.epub example.epub.pdt
```

Install the pinned PlatformIO dependencies first so CMake can find M5GFX/miniz, and provide OpenSSL development headers. The host tool currently targets POSIX hosts (Linux/macOS). Copy the original and sidecar together, or upload the source followed by its prepared sidecar through the separately implemented admission route. `--flat` explicitly omits semantic metadata for diagnostics; ordinary preparation retains it when within budget.

Native tests compare every prepared text byte against a direct production import, verify US/GB/non-English metadata, exercise malformed envelopes and bounded semantic fallback, and assert that original and existing output files remain unchanged. A separate build of the same importer powers the browser worker. An agent skill should invoke this deterministic tool, rather than have an LLM rewrite book text. Source files, original archive ordering, paragraph bytes and mathematical content should not be transformed by a language-model summary step. The sender may archive originals; the device only needs the current source and valid acceleration data.

The CLI should expose distinct outcomes: prepared, copied/uploaded, accepted by device, and displayed. Local preparation success does not mean the reader has received the file. Interrupted transfer cannot publish a complete sidecar. Prefer an owned temporary file, validation, then rename, with original-book nonoverwrite rules maintained.

### Phone WebUI path

The implemented browser path compiles the production C++ importer, semantic
encoder and PDT1 envelope with [Emscripten modularized output](https://emscripten.org/docs/compiling/Modularized-Output.html).
`tools/browser_prepare.cpp` is a memory adapter, not an independent JavaScript
extractor. It bounds input at 64 MiB, text at 4 MiB and metadata at 352,808 bytes.
A portable SHA-256 implementation avoids `crypto.subtle`, so the AP's HTTP
origin does not require a browser secure-context crypto service. The original
EPUB is never rewritten.

The AP WebUI passes a selected `File` to `prepare_worker.h`; it does not read the
entire archive on the page's UI thread. The worker loads authenticated
same-origin `/prepare-core.js` and `/prepare-core.wasm`, reports phases, copies
the file into the bounded WASM heap, and transfers a copied result buffer back.
Cancellation terminates the worker, because messages cannot interrupt its
synchronous native call. The page must remain open: no resident/background
phone service is promised. A failed preparation offers original-only upload;
a successful source upload followed by a failed sidecar offers sidecar-only
retry using the actual source filename returned by the device.

The production module starts with a 16 MiB heap, can grow to at most 128 MiB,
and has a 256 KiB stack. File buffers, JS/engine and UI memory sit outside that
ceiling. A phone can exhaust its own memory earlier, so 64 MiB is a format limit,
not a universal phone-memory guarantee. Source storage is freed after import
and before result packing. No CDN, network converter, SharedArrayBuffer or
remote JavaScript dependency is required.

A recorded Emscripten 4.0.18 integration build produced 13,483 JS bytes and 91,067 WASM bytes;
deterministic gzip reduces these to 3,991 and 42,449 bytes respectively. The
46,440-byte core asset footprint excludes the small worker wrapper. Firmware
streams these immutable resources directly from flash in 2 KiB poll increments,
with authenticated routes, correct MIME/gzip headers and a separate 1 MiB
asset-response ceiling. It does not allocate a complete asset-sized RAM response.
The committed artifact is a text C++ header; the raw build products remain local.
A separate-directory `--check` rebuild reproduced the header exactly.

Actual generated WASM passed 108 whole-PDT byte-identical successful cases and
94 expected rejections against the native tool, including an exact 64 MiB
archive. The corpus reuses production EPUB fixtures in semantic and flat modes.
The real worker wrapper also ran against real WASM with native-byte parity;
hard cancellation and a separately compiled 16 MiB heap's source/text allocation
failures and recovery passed. These are host execution results. Physical phone
browser timing, browser background behavior and final device AP delivery remain
separate acceptance work; no phone/device speedup is inferred from Node tests.
A later real-browser localhost check ran the production worker/assets under
production CSP and completed correctly paired source/prepared uploads after
server-side renaming. Its scope is detailed in section 9; it did not use an
ESP32 AP or modify the host network.

Do not automatically transform source archives to “optimize” them by removing fonts, illustrations, styles or chapters. A separately selected output conversion can be useful, but it changes the source, identity and supported semantics. Sidecar preparation is reversible and preserves the source.

## 6. English typography: four improvements with explicit bounds

The current working tree implements these four bounded mechanisms. The following sections explain their rationale and limits; they do not claim complete browser/TeX layout. `line_quality.h` caps local measurement work, generated `latin_metrics.cpp` supplies a single copy of native font tables through `latin_metrics.h`, `epub_document.h` preserves supported semantic spans, and `text_language.h` selects US/GB patterns. Physical-device validation is separate from these host and source checks.

### 6.1 Bounded paragraph lookahead

Greedy line filling optimizes only the current line. A bounded two/three-line candidate search can avoid one very loose line or an unnecessary hyphen when moving one word yields a better sequence. Score candidates by normalized space stretch, hyphen penalty, consecutive-hyphen penalty, short final-line penalty and page-edge constraints. Keep a fixed candidate/word/byte budget; fall back deterministically when exhausted.

The reference is [Knuth and Plass, “Breaking paragraphs into lines”](https://doi.org/10.1002/spe.4380111102), which models paragraph breaking as a global optimization problem. A bounded embedded lookahead is a different implementation with weaker optimality guarantees. It should not be described as complete TeX paragraph composition. Require measurable improvement on synthetic uneven-word and long-paragraph fixtures, and cap the regression in complete-index time.

### 6.2 Font-derived kerning and precise advances

Kerning must come from the selected font, not a list of hand-guessed `AV` offsets. Extract bounded supported Latin pair adjustments from the pinned open font, preserve fractional advances in fixed-point units, and round accumulated drawing positions rather than every glyph independently. Measurement and paint must consume the same pair state. Reset it across style changes, line starts, explicit spaces, source barriers and mathematical atoms.

A bitmap can stay at native resolution while placement accumulates fractions; this does not imply scaling or antialiasing. Ligatures require a separate mapping from source character clusters to glyphs. [HarfBuzz's shaping documentation](https://harfbuzz.github.io/shaping-and-shape-plans.html) describes glyph advances/offsets and clusters; it is useful for a future host shaper, but linking a full shaper into firmware should follow a memory and correctness review. Pair kerning alone is not full shaping or grapheme support.

### 6.3 Preserve book structure

Use importer-provided semantic spans for headings, quotations, lists, emphasis and preformatted text. A heading can be kept with the following body lines; quotes can use bounded inset; code should preserve source spacing. Preserve paragraph identity instead of inferring it from repeated newlines after the fact. For native-size-only rendering, use deliberate native emphasis variants or restrained styling; do not rescale text to simulate any requested CSS size.

The practical scope is a bounded supported subset with faithful source-offset mapping. Simple horizontal tables and a limited presentation-MathML subset now have explicit semantic objects; their exact admission, source fallback and remaining-page row flow are documented in [EPUB typesetting](epub-typesetting.md). Full CSS cascade, floats, multi-column layout, image sizing, general tables, SVG and complete MathML remain outside that subset. A flat fallback must not be advertised as preserving a fraction, table or ruby annotation semantically.

### 6.4 Language and page-edge policy

Retain BCP-47 language tags at book/resource/span levels. English regional pattern data can differ; enable the appropriate bounded set only when metadata supports it. An unknown language is not evidence that English hyphenation is valid. Soft hyphens supplied by a publisher remain a separate discretionary mechanism.

A block-aware page composer can keep a heading plus a small following fragment and avoid a one-line paragraph fragment where feasible. It must relax the rule for oversized blocks and narrow geometry so progress is guaranteed. The constraints should not create blank pages, endless repagination or discard text. [CSS Fragmentation](https://www.w3.org/TR/css-break-3/) defines keep/break and widow/orphan concepts; our small-screen policy is an adaptation, not a complete implementation of that specification.

Useful later details include font-supported ligatures, optical margin handling with measured overhang, and language-aware quotation spacing. Each requires source-preserving regression and a cache identity change. Do not trade a long first open for invisible typography work without a cached or incremental path.

## 7. Chinese and Japanese horizontal layout

The project enables punctuation compression and Han/ASCII spacing by default. The working tree now separates prohibited line-start characters from hanging punctuation in `pagination.h`; small kana and iteration/prolongation marks are kept with the preceding context without hanging into the margin. Matching repeated em dashes and ellipses are protected in the strict break path. Prohibited line end and compressible side bearing remain separate decisions. This bounded policy improves horizontal Chinese/Japanese text but is not a complete implementation of JLREQ or Unicode line breaking.

[CLREQ](https://www.w3.org/TR/clreq/) discusses Chinese punctuation, spacing and layout requirements; it does not prescribe one universal line height for this hardware. Chinese punctuation placement can depend on locale and typeface. The current SC-derived font is not evidence of region-specific Japanese or Traditional Chinese glyph forms. A language profile should select explicit rules and available font data, with a visible fallback policy for missing glyphs.

Use bounded backtracking at legal boundaries, preserve paired dashes/ellipsis where the selected policy requires it, and keep combining/variation sequences intact. Mixed-script spacing should respect explicit spaces and code/math barriers. The implementation should not insert spacing before every non-ASCII character or interpret all Han text as Chinese. Test simplified/traditional punctuation, kana, narrow-width edge cases, mixed Latin numbers and the same page drawn/indexed through both code paths.

## 8. Japanese vertical layout: correct scope

Japanese vertical layout is a distinct layout mode, not a 90° screenshot transform. The implemented path requires an explicit supported EPUB `vertical-rl` declaration; Han characters, Japanese language and RTL page progression alone cannot select it. Plain text does not infer vertical layout. Japanese horizontal technical books remain horizontal when the source requests that.

[CSS Writing Modes](https://www.w3.org/TR/css-writing-modes-3/) separates block flow, inline direction, glyph orientation and text-combine behavior. [EPUB 3.3](https://www.w3.org/TR/epub-33/) specifies publication structure and page progression. These are different axes: right-to-left page progression alone does not prove vertical text. The current reader's display rotation setting is also independent of writing mode.

### Required data and algorithms

- Lay columns top-to-bottom and progress columns right-to-left for `vertical-rl`; keep source order and bookmarks independent of physical axes.
- Use native vertical advances/origins and suitable vertical glyph alternates for brackets, punctuation and kana. The [OpenType registered features](https://learn.microsoft.com/en-us/typography/opentype/spec/features_uz) specify `vert`/`vrt2`; a font subset needs to retain or bake the relevant substitutions and metrics.
- Classify upright versus sideways glyphs with versioned [Unicode Vertical_Orientation data](https://www.unicode.org/reports/tr50/). A simple “ASCII rotates, everything else upright” rule misses punctuation and transformed forms.
- Represent short horizontal digit runs (`tate-chu-yoko`) as bounded atoms with explicit policy; handle longer Latin runs consistently.
- Keep ruby base/annotation relationships instead of concatenating readings into prose. Ruby needs grouping, fitting and edge behavior in both axes; [CSS Ruby](https://www.w3.org/TR/css-ruby-1/) is the relevant formatting model.
- Apply Japanese line-start/end rules, indentation, punctuation spacing and page/column fragmentation to the vertical measure. [JLREQ](https://www.w3.org/TR/jlreq/) describes these requirements and the variation among acceptable policies.
- Preserve native glyph clarity, source anchors and monotonic page progress. A vertical layout profile needs its own cache identity and regression corpus.

These bullets are this project's implementation decomposition, not a statement
that all cited specifications are implemented. The current working tree has a
bounded vertical composer, native JP glyphs and vertical alternates, ruby and
explicit one/two-digit text combination. It preserves canonical text anchors;
horizontal Japanese stays horizontal. Unsupported complex CSS, ruby and glyph
coverage remain limits. See [the implemented scope](epub-typesetting.md), rather
than treating the original decomposition below as the current completion state.

### Original implementation sequence

1. Preserve writing-mode, language and ruby relationships in semantic metadata and test serialization without enabling vertical rendering.
2. Build a host preview using synthetic Japanese prose and a pinned open JP font. Compare native vertical alternates, small kana, paired marks, Latin runs, ruby and two-digit combinations.
3. Add an axis-independent `InlineMeasure`/`BlockMeasure` abstraction and a separate vertical page composer. Share source/semantic records and bounds, not horizontal coordinate assumptions.
4. Bake the required open-font vertical glyph subset; quantify flash increase and largest glyph dimensions before firmware admission.
5. Add device draw/index parity, rotation, exact-anchor migration, footer exclusion and page-edge tests. Then assess the real panel at native resolution.
6. Enable source-declared supported vertical content with readable fallback for unsupported structures. Broader automatic defaults need real-book coverage and explicit policy.

The first two stages provide an engineering prototype. They must not be called production vertical Japanese support. No original user book is needed for the synthetic corpus or public screenshots.

## 9. Benchmark and regression plan

Use public synthetic fixtures with fixed source hashes and documented generation parameters. Native/WASM byte parity and host fault injection test correctness; they do not establish physical SD throughput, wireless performance or power consumption. Keep first-page, canonical-index and cache-publication boundaries separate.

USB book transfer was removed; the experimental MSC implementation never established raw SD reliability.

### Continuing regression matrix

Use deterministic synthetic EPUBs or redistributable public-domain inputs. Generate inputs varying archive size, text size, chapter count, compression ratio, language mixture, XML node count, long words, repetitive punctuation and large unused assets. Keep source hashes fixed per fixture.

For each profile measure:

1. Cold first open with no prepared/cache data.
2. Reopen after leaving the book within the same mount.
3. Reopen after restart/remount.
4. One typography change with unchanged text.
5. Return to a previous typography profile.
6. Open with valid prepared data, then stale/corrupt/oversized prepared data.
7. Interrupted cache write and SD removal.
8. Saved anchor far into the document, including a position inside a page after reflow.
9. First page versus canonical completion versus deferred publication, including navigation during all three.
10. Real browser preparation/AP delivery, cancellation before source publication and retry after source-only success.

Report median and worst observation from a small stated repetition count, source/text size, active SPI clock, CPU frequency, firmware identity and cache outcome. Do not time a warmed run against a cold baseline and attribute the entire change to one algorithm. Byte counters and stage logs are the strongest explanation of a speedup.

Correctness gates include complete text coverage, monotonic UTF-8 offsets, identical index/paint breaks, exact reading-anchor retention, no writes to source books, no NVS credential changes, bounded memory, graceful allocation failure, and no extra queued button action after a long operation. A performance result without those invariants is not an acceptable trade.

## 10. Recommended sequence and explicit deferred scope

The implemented sequence now includes measurement-work reduction, verified content reuse across layouts, source-validation memoization within controlled storage ownership, semantic caching, native/browser production-importer preparation, bounded first-page EPUB indexing, independent closed-chapter extraction and partial-page-index recovery, deferred cache publication and authenticated cache-text windows. Host correctness and hardware performance require separate validation. Typography improvements consume the same metrics and cache identities rather than create a parallel uncached renderer.

Closed-chapter recovery is implemented; suspending within deflate/XML, streaming chapter DOM allocation and reducing the 4 MiB reservation remain further performance work. Full-source hashing and individual filesystem calls still impose cold-open/cancellation latency. A full browser CSS engine, arbitrary downloaded fonts and complete MathML are outside the bounded reader implementation. Japanese, semantic-object and LAN support have their own bounded contracts. USB book transfer remains disabled by default; explicit experimental compilation does not imply hardware reliability.
