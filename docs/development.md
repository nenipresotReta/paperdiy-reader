# Development

## Ownership and rendering

`app` owns navigation and lifecycle; `input` owns button events; `ui` draws scenes.
`reader` shares pagination and drawing metrics, `sd_reader` discovers/imports
books, `epub_core` extracts EPUB text, and `sd_cache` validates stored source
fingerprints, text and page offsets. `document_format` is the shared,
case-insensitive format/size policy for discovery and AP admission: TXT 4 MiB,
EPUB 64 MiB, Markdown 1 MiB. Extracted EPUB text remains limited to 4 MiB.

`markdown_document` builds bounded runs and a monotonic source/render offset
map from the unchanged UTF-8 source. Lists may generate presentation bytes,
entities may collapse several source bytes, and a supported formula becomes
one layout atom. Bookmarks always refer to original source byte offsets,
including a leading BOM. Raw table text and rejected formulas retain their
source, delimiters and line breaks. Parser failure discards the IR and permits
plain-source fallback; it must never publish a partially parsed document.

`markdown_pagination` reuses the English/CJK line breaker and computes each
row's ascent, descent and height from its contents. Measurement and painting
use the same path. Page boundaries must advance monotonically and resume
without dropping generated markers, text or formula atoms. Individually
admissible inline formulas may still need separate rows when their combined
baseline extents exceed a page.

`math_layout` provides a bounded TeX subset, not a TeX interpreter.
It emits glyph/rule commands with width, ascent and descent; glyph command
coordinates use a baseline relative to the formula's top edge, while rules
use a rectangle top. Limits are 4096 source bytes, 256 nodes, 1024 commands,
16 levels, 8×8 matrix cells and 128 KiB scratch storage per layout.
The reader prepares one reusable workspace and eight fixed output slots
before document admission. Recomputing an admitted formula does not allocate.
The slots form a bounded replacement ring keyed by source span, not an LRU.
A failed formula must remain readable as its whole original source, never a
zero-sized or partial success. See [supported syntax](markdown.md).

The vendored MD4C 0.5.2 parser has checked allocation, source-range and
cooperative-tick extensions, plus allocation-failure cleanup fixes.
[PATCHES.md](../lib/md4c/PATCHES.md) records the upstream checksums and local
changes. The document limits are 1 MiB source/render text, 4096 runs,
8192 mappings and a 512 KiB transient parser allocation budget.

## Settings extension contract

Each additional settings page is another virtual book in the library, appended
immediately below the existing settings books. Keep this ordered group before
ordinary books and use the same row styling, selection, opening and return
interaction. Add a page only when it has implemented settings; no placeholder
books or extra menu hierarchy.

Each physical button owns one group of mutually exclusive states and cycles
only that group. Express the states with fixed-position logical symbols, as
with `N / A / U` and `¬R / R`: inactive alternatives carry `¬`, an active
alternative does not. A group with an Off state can have all alternatives
inactive. The two button groups remain independent. Keep symbols equidistant,
preserve physical-button correspondence after rotation, and avoid explanatory
page titles or instructions. Required operational content, such as AP codes
and credentials, still has a separate non-overlapping area.

`settings_registry.h` keeps stable IDs `None=0` and `Network=1`. Only the
network/rotation book is listed, leaving 21 ordinary books on the first screen.
Wire ID 2 is retired and must not be reused. Existing V2 snapshots selecting
that page restore Network read-only, preserving the selected book path.

Punctuation compression and Han/ASCII spacing are always enabled through a
const production style. No page or button toggles them. The legacy `cnStyle`
NVS key is ignored and preserved; startup does not write or delete it.

`reader_typography.h` applies these options to TXT/EPUB and Markdown prose.
Compression uses native glyph ink bounds, capped at 14 px while keeping 2 px of
ink separation. Spacing adds 4 px only between adjacent Han and ASCII letters
or digits; explicit spaces and unsupported boundaries are left alone. Markdown
code/preformatted runs, math objects and rules block contextual adjustments on
both sides. Code uses each native 24 px glyph's actual advance, without the old
24 px minimum character cell; this is a proportional font, not a monospaced
font. Preserve source spaces, indentation and newlines, with no code
justification or hyphenation. No font regeneration is required. Page geometry
and formula internals retain their own rules.
The fixed enabled layout keeps the existing enabled cache identity and exact
source anchors. Other historical layout variants remain read-only migration inputs.

## Progress and caches

NVS `resumeV2` stores the boot scene, stable settings ID and selected library
path in one versioned atomic record (8-byte header; byte 5 is the settings ID).
When V2 is absent, read `resumeV1`: its boolean maps to Network or None. A corrupt
V2 must not resurrect an old V1 snapshot, and a key must match its wire version.
V1 and the older scalar resume fields remain read-only fallbacks; opening the
new firmware does not rewrite them. Rotation writes and shutdown persistence
are checked before applying
the new state or stopping a live network session. Per-book progress retains
the deployed NVS namespace and supported legacy keys for recovery; failures
must not silently become page zero or permit replacement of an unsaved book.
Rendering never writes preferences. A restored source offset remains the reading
anchor even when reflow places it inside a different page. Opening, redraw,
shutdown and a successful cache load must preserve that exact offset; only a
real page change adopts the new page's start. The in-memory anchor is tied to
the content key and the containing page, including persistence failure/retry.

The layout family `0x20261050` encodes the P/S flags and text mode in its low
bits: hyphenated text, non-hyphenated text, or Markdown. A cache hit must match
both the current mode and P/S flags. Other layouts can provide verified text
for read-only anchor migration, but cannot supply current page boundaries or
change the runtime style. Other geometry or extraction changes require a new
layout family. A temporary raw Markdown fallback must not replace a valid
semantic cache.

Caches hash source bytes, verify a temporary file before replacement and keep
one previous generation as `.bak`. A different size/mtime rejects a source before
hashing, including backup candidates and save validation. Equal stamps are only
a prefilter; the complete digest and before/after stamps remain required.
Cache hits still read sources to detect
same-size, same-timestamp replacements. Only the cache directory is written
by the reader; AP uploads create new books separately. Cache I/O uses an 8 KiB
buffer and yields after 128 KiB or 50 ms.

TXT/EPUB layout changes must bump both `reader_cache::LAYOUT` and
`LAYOUT_NO_HYPHENS`. Markdown uses its independent `LAYOUT_MARKDOWN`:
adding or changing its renderer must not invalidate unrelated books.
Its cached text is the original source, with source offsets as page starts.
A whole-document plain-text fallback must not replace a valid Markdown cache.

Migration reads validated v1 or superseded v2 caches without modifying them,
captures a context anchor, imports the current text, maps the saved position,
persists it, then replaces the cache. Unchanged text identities can retain
offsets across font/layout changes. Reject unreadable or out-of-range
bookmarks instead of silently restarting at the beginning.

EPUB XML is validated before each TinyXML2 parse: at most 32 element levels,
1 MiB decoded text and 2 MiB of conservatively estimated DOM pools.
Pool costs use target object sizes; the device also checks available PSRAM and
the largest free block. UTF-8/UTF-16, whitespace, numeric and fixed XHTML named
entities are normalized without DTD loading. Auxiliary spine items follow the
main reading order. An optional `epub::structure::Collector` preserves block/style/language ranges
over unchanged extracted text; the production reader does not yet use it.
Its bounded allocation and host-only spacing experiments are documented in
[EPUB typesetting](epub-typesetting.md). No layout/cache ID changes with these
experiments.

The loop task has a 16 KiB stack; device boundary tests
require at least 4 KiB remaining after parsing and recursive destruction.

## AP transport and retries

`network` owns on-demand AP/STA and uploads. `bounded_http_request` and
`bounded_web_server` use the pinned SDK's routing/response API, bypassing its
unbounded multipart parser. Receive/transmit work is polled in 2 KiB budgets,
with 5-second deadlines and explicit header/body/response memory limits.
The service exists only in A mode. N connects saved Wi-Fi and shows status/IP;
it has no HTTP server, document push, WebSocket or subscription.

Authenticated, URL-encoded AP requests use these endpoints:

| Request | Contract |
|---|---|
| `POST /api/upload/start` | `name`, `size`, `generation`; optional `requestId` is 32 lowercase hex characters |
| `POST /api/upload/chunk` | `id`, exact current `offset`, base64 `data`; at most 4096 decoded bytes |
| `GET /api/upload/status?id=…` | Active `receiving` offset/size or retained `complete` receipt |
| `POST /api/upload/commit` | `id`; finish only after all expected bytes are saved |
| `POST /api/upload/cancel` | `id`; only cancels the matching active transaction |

One transaction holds one file. Start advances the admission generation.
Replaying the same request identity with its original name, size and generation
returns the existing transaction or receipt; conflicting metadata is rejected.
Clients omitting `requestId` retain the earlier protocol behavior.
Chunks require exact offsets: after losing a response, query status before
retrying bytes rather than blindly appending again.

Commit verifies saved size and atomically renames the owned temporary file,
then updates the library. Existing book files are never overwritten.
Four bounded RAM completion receipts make repeated commits return the original
result without importing again. Receipts are cleared when AP stops or storage
changes; physical client disconnect cancels an unfinished transfer.
The progress lease expires after 45 seconds without new bytes. Status polling
does not extend it. There is no durable receipt or resume guarantee across a
reboot, AP restart, card change or receipt eviction.

The page uses a stable random request identity, a 12-second fetch timeout and
at most three attempts per retryable operation. It checks offsets after lost
chunk responses and checks completion after lost commit responses. It never
silently creates a fresh transaction to resolve an ambiguous response; if
status cannot establish the outcome, it asks the user to check the book list.

Cookie authentication matches the exact session token; the QR query token
bootstraps only the root page. AP is WPA2, one client and local HTTP without
TLS. Family Wi-Fi configuration is saved by explicit POST into one versioned
NVS blob; legacy SSID/password keys are read-only fallbacks. Passwords are
neither echoed nor logged. A fresh 20-second STA error window starts after each
connected-to-disconnected transition.

The AP idle timeout is five minutes; polling is not activity, while a live
transfer holds the session open within its progress lease. `networkActive`
blocks manual light sleep. Battery/USB policy and actual measurement limits
are in [power.md](power.md). Future LAN discovery, pairing and push are only
a [design proposal](lan-design.md); do not expose these AP routes in N mode.

## Fonts and build

The public source includes two generated OFL subsets:

- `src/native_font_data.h`: Noto Serif CJK SC 2.003, native 24/28 px,
  internal derivative name `NativeFont`.
- `src/math_font_data.h`: STIX Two Math 2.13 b171, native 14/20/28 px,
  derivative name `PaperDIY Math`. MATH constants, italic corrections and
  vertical variants come from the pinned OpenType source, without runtime
  bitmap scaling.

No system font, book corpus or private artwork is needed. Both generators
verify fixed source SHA-256 values, download only when their cache is absent,
and write outputs atomically. Keep source fonts and temporary build artifacts
outside Git. License/provenance are in
[THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).

Use the Python environment from the README. After installing requirements:

```sh
pio run
python tools/build_fonts.py
python tools/build_math_fonts.py
cmake -S . -B build -DSANITIZE=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Native tests require CMake, a C/C++17 compiler, Node.js 18+ and OpenSSL headers
(`libssl-dev` on Ubuntu; `brew install openssl@3 cmake node` on macOS).
On macOS, pass `-DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"` if needed.
CMake rejects a missing/older Node version rather than silently omitting the
actual frontend tests. `pio run` supplies M5GFX; a custom dependency path can
be supplied with `-DM5GFX_DIR=…`. Font tests need the pinned source caches;
a reported font-source skip is not evidence of raster regeneration coverage.

Tests cover controls, release guards, power policy, refresh decisions,
cache corruption/I/O failures, source fingerprints, bookmark migration,
atomic resume records, NVS failures, real reader integration and EPUB
syntax/encoding/resource boundaries, optional structure ranges, unchanged-text
equivalence and structure-allocation fallback. Typography experiments check
actual native-font positions and source coverage without changing production
layout. Markdown tests cover source mappings,
raw-source fallback, list/page boundaries, variable baselines, tall formulas,
bounded/allocation-failed parsing and layout, and glyph provenance/metrics.

Network tests compile the production module against fault-injecting SD/NVS
and radio adapters. The actual HTTP parser/server wrapper is tested for
malformed framing, deadlines and disconnected/stalled peers. Run
`node tests/web_ui.js` independently to exercise the actual inline production
script with a fake DOM and transport: normal binary upload, lost start/chunk/
commit responses, all commit responses lost, and bounded failure/cancellation.
It checks transaction identity and offsets, not browser layout or real timing.

## Review and device evidence

The review covers persistence, byte-offset recovery, bounded parsing/layout,
upload authentication, idempotent retries and lifecycle/power gates.
Passing native tests cannot prove actual current draw, e-paper ghosting,
radio association or QR readability. Markdown is a first bounded subset,
not a promise of full TeX/CommonMark rendering or universal font coverage.
Device acceptance must still verify SD mounting, page turns, charging-only
LED, A+B shutdown, scene restore, loading feedback, QR scans and uploaded bytes.

Releases contain public source and a separately built application binary.
See [upgrading](upgrading.md) for scope and checksums. Application-only updates
with the unchanged partition table preserve NVS; do not erase flash or change
book paths when preserving progress. Keep books, credentials, device backups,
hardware logs and private shutdown assets out of release inputs.

`PAPERDIY_DIAGNOSTICS` enables bounded serial commands such as `STATUS`,
`SCREEN`, `OPEN n`, `NEXT`, `PREV`, `ENTER`, `LIBRARY`, `RESCAN`,
`SHUTDOWN_PREVIEW`, `REFRESH`, `PAGE_TEXT` and `CACHEINFO`. The last only reads
cache headers. Also enabling `PAPERDIY_DEVICE_TESTS` adds local parser,
typography and power checks. `MD_CHECKS` verifies measurement/drawing agreement;
`MD_BENCH` indexes fixed synthetic prose twice without SD/NVS writes, then frees
its sample before restoring the original adapter. Diagnostic work discards
input accumulated while busy. Neither flag is part of a normal build.

Test-only `SD_IO n` probes an existing library source; an expanded EPUB directory
is reported as a directory without opening it as a file. `SD_CACHE_IO n` probes
the existing cache derived from that source path, without loading, creating or
rebuilding a cache. Each reads at most the first 256 KiB twice, in 8 KiB chunks,
and compares SHA-256 internally; neither filenames, content nor digest values
are printed. Metadata reports file kind/size and captured error codes. FatFs can
report ENOENT for a directory passed to file-open, so the probe stats first.

The 12-second read deadline and 20-second synthetic-layout deadline are
cooperative checks between synchronous calls, not cancellation of a blocked
SDK operation. Prefix agreement is not whole-file validation. `read_us` excludes
hash work and is not book-open latency; `mount_hz=0` means unknown, and a mount
frequency must instead be verified from the successful boot attempt. No clock
or mount changes are performed by these probes.

`PAPERDIY_NETWORK_TESTS` additionally exposes local AP fixture hashing/cleanup.
Cleanup recognizes only fixed fixture paths with exact SHA-256 contents and
an exact dummy Wi-Fi record; it must never replace real saved credentials.
Remove the flag after acceptance. An optional `PAPERDIY_SHUTDOWN_HEADER` may
name a private bitmap header; keep it and its build configuration outside Git.

## Public release checks

Build releases from the reviewed public branch, with diagnostics and the optional
shutdown header disabled. `src/version.h` supplies the firmware version. Keep the
README short; user-facing changes and upgrade details live under `docs/`.

`python tools/check_public_source.py --index .` checks every staged source blob;
`--tree DIR` checks the unpacked source artifact. The allowlist rejects generated
files, private paths, artwork, credential markers and non-regular entries. It does
not replace an outgoing-history review or the separate firmware audit. Test it
with `python tests/public_source.py`. CI runs this gate, the pinned font-source
checks, all native tests and the default public firmware build, with read-only
repository permissions and Actions pinned to full commit IDs.

Source archives come from the exact release commit. Firmware packages contain
only the application image, upgrade instructions, checksums and matching license
notices. Record the source commit and SHA-256 digests. Do not package a working
directory, merged flash, NVS, SD content, logs, source font caches or private builds.
