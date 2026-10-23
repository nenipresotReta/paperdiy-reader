# EPUB typesetting

Status: optional import metadata and host-only spacing experiments. The device
still uses the existing plain-text EPUB renderer. No new EPUB layout, cache
format or bookmark migration is enabled by this work.

## Structure before style

The deployed importer normalizes XHTML into UTF-8 text. Paragraph separators
survive, but headings, lists, quotations and local language do not reach the
renderer. Some losses change meaning: superscripts become ordinary text,
MathML fractions become consecutive operands, ruby annotations join the base
text, table cells run together, and image alternatives disappear. A successful
text import is not evidence that these structures are faithfully displayed.

`epub::structure::Collector` is an optional sidecar over that exact extracted
text. Set `Importer::structure` before `run()` and use records only when
`ready()` is true. Normal import leaves the pointer null. The collector records:

- title, paragraph, heading level, quotation, list/container, preformatted,
  definition and table-row boundaries, including empty blocks;
- parent and resource references, original spine order and auxiliary resources;
- strong/emphasis/code/preserve/superscript/subscript flags on text runs;
- inherited metadata, XHTML and inline `xml:lang` / `lang` tags.

Offsets are UTF-8 **extracted-text** byte offsets, not XHTML or ZIP offsets.
Records are in XML preorder. Nested block intervals overlap; Text/Break runs
are disjoint. Separating newlines need not belong to a text run. Normalization
may remove trailing spaces, so final ranges are clipped to their parents and
resources and checked at UTF-8 boundaries.

The collector uses one checked allocation: at most 8192 records, 2048 resources,
128 KiB of paths, 32 language slots, and a 512 KiB total allocation budget.
A collector allocation/capacity failure discards the complete sidecar while
flat import continues. A source/XML/import failure also discards the sidecar,
but still fails the whole import. A successfully allocated collector can reduce
XML headroom; production integration must budget both together and, for a
recoverable memory failure, retry with a fresh importer without the collector.
There is no automatic whole-import retry yet. Callers can inject a paired
allocator/free function for PSRAM. `ready()` means the metadata transaction completed; it does
not claim support for CSS, ruby, MathML, images, table cells or list numbering.
Nothing is serialized to SD yet.

## Two independent experiments

`experimental_typography.h` is used only by `tests/typography_experiment.cpp`.
It shares the production line breaker, font metrics and source offsets, with a
separate page adapter for contextual advances. It is not a reader setting. Future production controls must follow the
[settings extension contract](development.md#settings-extension-contract): a
new virtual settings book beneath existing ones, with one mutually exclusive
state group per physical button and logical-symbol status.
The two options are off by default and tested separately:

| Candidate | Rule | Deliberate limits |
|---|---|---|
| Adjacent punctuation | Remove up to 14 px of blank between selected opening/closing/stop pairs, using actual glyph bearings; retain at least 2 px of ink gap and positive advance | No glyph scaling; no trimming of ellipsis or em dash; no blanket half-width conversion |
| Han/Latin spacing | Add 4 px between Han and ASCII letters/digits | Explicit spaces, punctuation, combining marks and variation selectors are barriers; no inferred grapheme shaping |

Both reset at each line start. Measurement and painting use the same delta.
The baseline stays at native 28 px, 40 px line height, left 24/right 510 and
30 px maximum hanging allowance. No new indent, paragraph spacing,
justification or font selection is bundled into the comparison.

Run `typography_experiment` from the build directory to generate three 540×960
PGM previews (`typography-current`, `typography-punctuation`,
`typography-autospace`). They use original synthetic prose and the public native
font, with no personal books or system fonts. The disabled experiment must
match the deployed page offsets and every glyph position. Tests also cover
UTF-8 boundaries, source coverage, explicit-space barriers, invalid geometry,
automatic-hyphenation literals and bounded work on long punctuation runs.
These are host previews, not measurements of actual e-paper ghosting.

## Next integration steps

1. Add explicit semantic support/unsupported markers for structures that can
   change meaning. Table cell boundaries, ruby base/annotation relationships,
   bounded MathML objects and image alternatives need their own representation.
   Zero-text objects also need stable anchors. Do not call a flat-text fallback
   faithful rendering of a fraction or table.
2. Connect a bounded block composer: keep a heading with up to two following
   body lines where feasible, use local language for hyphenation, and preserve
   list `start`/`reversed`/`value` before generating display-only numbering.
   Add quotation/preformatted layout and superscript/subscript. A too-large
   block must have a deterministic fallback that still advances the page.
3. Separate line-start prohibition, line-end prohibition, hanging and
   compression classes. Current code can start a Japanese line with `ー` and
   split a double em dash. Do not fix these by adding kana to `closing()`, which
   also controls hanging. Locale-specific boundaries need bounded backtracking.
4. Evaluate punctuation spacing, mixed-script spacing, indentation, paragraph
   gaps and CJK justification independently. A two-line widow/orphan policy is
   a candidate for a later block composer, not a universal reading preference.
5. Integrate versioned structure/page caches and bookmark migration before
   enabling any production style. EPUB should share layout primitives with
   Markdown, not be converted into Markdown with a second lossy parser.

## Cache and progress contract

A layout change must not redefine content identity. Preserve the exact saved
text offset; repagination finds its containing page. Merely opening/reindexing
must not overwrite that precise anchor with the new page start, which could
cause gradual backward drift when styles alternate. Layout IDs must include
semantic version, mode and all parameters that affect pagination.

Existing cache hits bypass the EPUB importer. A future semantic mode must load
a validated sidecar or rebuild it; it cannot reconstruct headings from plain
text. New CSS/resource dependencies must enter the content fingerprint. Only
cache files may be replaced; book files remain unchanged.

If later semantic fixes change extracted text, recover a context anchor from
the validated old cache, map it deterministically to the new text, verify and
persist progress, and only then atomically replace the cache. On failure retain
the old state; never reset to zero or guess a position by percentage.

## Research basis

[CLREQ](https://www.w3.org/TR/clreq/) describes punctuation blank adjustment,
line breaking and paragraph layout; it does not prescribe a universal 42 px
line-height minimum or forbid every left-aligned implementation.
[JLREQ](https://www.w3.org/TR/jlreq/) describes Japanese character classes and
multiple constraint choices; its policies should follow language, not be
unconditionally applied to Chinese books.

[CSS Text 4](https://www.w3.org/TR/css-text-4/) describes `text-spacing-trim`
and a 1/8 ic autospace convention. Four pixels at our 28 px size is a rounded
candidate, not a conformance claim. [KOReader/crengine's formatter](https://github.com/koreader/crengine/blob/master/crengine/src/lvtextfm.cpp)
provides a useful implementation comparison, including a different mixed-script
spacing convention and a distinction between glyph width and shrinkable space.

[CSS Fragmentation](https://www.w3.org/TR/css-break-3/) provides keep/break and
widow/orphan concepts. Their policies still need adaptation to a small fixed
screen and a bounded renderer. A video's percentages or old design suggestions
are not substitutes for the actual font's ink bounds or an A/B comparison.
