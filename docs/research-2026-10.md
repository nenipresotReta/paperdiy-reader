# Reader research — 2026-10-23

Baseline: the pre-change source. This round prioritizes less waiting, fewer unnecessary
refreshes and stable reading positions. No new settings page, background
network service, default typography or hardware power policy is introduced.
The three small changes below are integrated and tested in source; they are source-level results, not device acceptance.

## Integrated changes

### Preserve the exact reading anchor

A restored offset can fall inside a page after layout changes. Previously,
opening or shutting down immediately saved the page start over that precise
offset. Repeated reflow could move the bookmark backward without a page turn.
The reader now retains the source anchor, content key and containing page;
only actual page navigation adopts a new page start. Validated cache loads
publish the same anchor. NVS keys, book contents and cache layout IDs are unchanged.

The new exact-offset assertion fails on the baseline. Regression covers current
and legacy keys, context migration, cache hits, eight alternating Markdown/plain
layouts with no extra writes, real page changes, persistence failure and retry.
Diagnostic `offset` remains the page start; `anchor_offset` exposes the precise
position separately, and shutdown reports the offset actually saved.

### Refresh only changed battery pixels

The battery interior is 18 pixels wide. Percentages 86, 87 and 88 all produce
the same fill, and charging has no separate on-screen mark. Comparing raw
percentage/charging state caused identical images to be sent to the panel.
The UI now compares fill width and draws the already-sampled value once.
The sampling interval and last-discharge history remain unchanged.

A deterministic real-UI host trace, covering both rotations, unknown values,
charge changes, jitter and a full discharge sequence, produced:

| Observation | Baseline | Candidate |
| --- | ---: | ---: |
| Display requests | 427 | 49 |
| Actual battery-pixel changes | 49 | 49 |
| Quality refresh requests | 10 | 0 |
| Battery-pixel trace hash | f4da230e0c0796fb | f4da230e0c0796fb |

This proves redundant software/panel requests were removed for that trace.
It is not a measurement of current draw, battery life or visible ghosting.
The pinned M5GFX panel worker still performs work for an accepted unchanged
fast request; avoiding the request also avoids consuming the app's periodic
quality-refresh allowance. No waveform or cadence was changed.

### Cache Markdown ASCII metrics

Preparation stores native body/code advances and body-glyph presence for 128
ASCII characters (1152 bytes of fixed storage). Repeated layout queries use
those values; non-ASCII, styles, math and drawing still use their original paths.

Host measurements: macOS arm64, Apple Clang 21, `-O2`, three full-index runs,
median; production font/layout code with original synthetic documents:

| Document | Pages | Baseline index | Candidate index |
| --- | ---: | ---: | ---: |
| 1,046,294 B Markdown prose | 1525 | 199.24 ms | 43.71 ms |
| 528,080 B Markdown, 600 formulas | 808 | 129.15 ms | 48.72 ms |

These are approximately 4.56× and 2.65× host CPU improvements, not device
opening-time predictions. Existing EPUB/TXT already cache ASCII advances and
are unaffected. All benchmark page-offset hashes match. A native-font regression
also fixes the two-page offsets `[0, 807]` and full framebuffer hash
`5d7e6107fc773c9a` across body/code/italic/bold/link/CJK/Greek/math content.

## Findings retained as prototypes

**EPUB semantic capability records.** The independent prototype preserves ruby
roles, table cell boundaries and spans, and marks MathML or omitted objects as
requiring dedicated rendering. It deliberately preserves current extracted text:
MathML fractions and adjacent table cells can otherwise become ambiguous strings.
A completed metadata collector is not proof of faithful rendering.

The prototype passes 85 existing EPUB cases, 57 byte-equality comparisons,
22 new synthetic documents and four budget/allocation failures. Records grow
24→28 bytes; the optional 8192-record arena grows by 32 KiB. It is not merged
or enabled in the reader. Simple ruby and table rendering still need contracts;
MathML trees, alternatives, image resources and CSS remain unimplemented.
The distinctions follow the HTML [ruby](https://html.spec.whatwg.org/multipage/text-level-semantics.html#the-ruby-element)
and [table](https://html.spec.whatwg.org/multipage/tables.html#attributes-common-to-td-and-th-elements)
models and EPUB's [embedded MathML](https://www.w3.org/TR/epub-33/#sec-xhtml-mathml).

**Incremental indexing.** An isolated one/four-page-budget prototype eventually
produces the same complete index. It can reduce work before a page near the start,
but a saved position at 90% still needs most of the prefix. Integrating it requires
an explicit unknown page count, complete-cache publication rules and yielding to
the app loop without replaying stale buttons. `delay()` alone is not that design.

## Next work, in order

1. Reduce avoidable SD validation work while preserving integrity. A cache hit
   still reads the complete source. The 64 MiB synthetic source case reads
   67,108,864 source bytes plus 4,210,844 cache bytes. At the configured maximum
   4 MHz SPI, serial bits alone imply about 142.6 seconds; this is a calculated
   lower bound for that maximum-size case, not an observation on a user's book.
   The follow-up below now rejects already-different size/mtime before hashing;
   matching stamps still undergo digest verification. Do not remove same-size/same-time
   replacement detection to claim faster loads.
2. Evaluate session-scoped validated fingerprints only after proving mount
   generation invalidation. Reboot, removal/remount, read/media error, all writes
   and future USB MSC transitions must invalidate them. A new cached timestamp
   alone is insufficient. Then consider deferred cache writes/partial indexing.
3. Connect semantic EPUB blocks with stable source anchors and bounded memory
   fallback: titles with body text, local-language breaking, then narrowly
   supported ruby/tables/math. Continue independent A/B choices for whitespace.
4. Investigate PMIC polling separately. The current awake loop may poll the
   PMIC about 100 times/s, while battery light sleep already reduces maintenance
   to about once/s. Do not throttle all `M5.update()` calls, which also service
   buttons, or lengthen sleep without testing power-key and USB wake behavior.

## ## Validation boundary

Merged source passes all 23 ASan/UBSan CTest suites and the pinned ESP32-S3 firmware build: 3,644,893 flash bytes and 56,060 static RAM bytes.

A follow-up diagnostic build passes 24/24 and compiles the read probe as C++11 without exceptions, using 3,644,889 flash bytes / 56,060 static RAM bytes.

Synthetic fixtures cover a 706-byte mixed-math note at source boundaries 0/540/706 and a 131,054-byte Markdown fixture that indexes to 213 pages. These are in-memory fixture results, not SD-open latency or a device speed comparison.

A follow-up adds the source-stamp rejection prefilter and opt-in bounded SD/synthetic Markdown diagnostics. Regression covers current and backup caches, missing sources, equal-size/equal-time replacements, validation-stage changes, save failure and exact restored anchors. The diagnostic build disables DEVICE_TESTS and NETWORK_TESTS while retaining local DIAGNOSTICS. Public regressions live in `reader_integration`, `cache_adapter` and `ui_simulation`.

No private books, reading records, device images, credentials or NVS contents are part of the public evidence. Device-specific observations and operation logs are intentionally omitted.
