# Development

[简体中文](development.zh-CN.md)

This guide records development contracts and commands. The [architecture report](architecture-and-implementation.md) owns the source map and current evidence table; build success, host correctness, device acceptance and publication are separate outcomes.

## Test-host isolation

Device development and flashing do not authorize changes to the host computer's
network. Do not toggle Wi-Fi, join device access points, alter DHCP/static IP,
DNS, gateway, service order or preferred networks, including automatic
restoration in cleanup handlers. Use USB for device checks and offline tests
or HTTP/browser fixtures explicitly bound to `127.0.0.1` on an ephemeral port.
Do not stop or reconfigure unrelated services or processes. Keep physical
phone/AP delivery as an explicitly untested boundary when these local checks
cannot cover it; never claim local worker parity proves wireless delivery.

## Build and dependencies

Use Python 3.12+ and the pinned requirements. The target is ESP32-S3 with
16 MiB flash, 8 MiB OPI PSRAM and the unchanged custom partition table.
PlatformIO pins `espressif32@6.12.0`, Arduino-ESP32 2.0.17 / ESP-IDF 4.4.7,
M5GFX `347a23274081a105cb9fc0fc3bbf0e15c78e79c3` and
M5Unified `e125e73f28b6b83e621e48df432af426bb2ff935`.
Firmware must compile as GNU++11 without exception-dependent recovery; native
tests use C++17. A host pass does not establish Xtensa compatibility.

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r requirements-dev.txt
pio run
cmake -S . -B build -DSANITIZE=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

Native tests require CMake, OpenSSL development headers and Node.js 18+.
PlatformIO installs M5GFX/miniz first; use `-DM5GFX_DIR=/path/to/M5GFX` for an
existing pinned copy. On Homebrew macOS, CMake may need
`-DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"`. Host-only tests use the portable
miniz byte-read path; they do not silently modify the pinned dependency.
Use a separate `-DSANITIZE=OFF` build for timing.

The public default is native USB MODE0 with CDC enabled at boot. `src/usb_features.h` defaults `PAPERDIY_EXPERIMENTAL_USB_MSC` to 0, disabling
MSC book transfer; ordinary builds hide U, cycle N → A → Off and reject
`USB_ENTER`. Explicit value 1 is for private experimental builds only and is
excluded from the shipped public configuration. `ARDUINO_USB_MSC_ON_BOOT=0`
alone is not this compile gate. Public builds exclude
`PAPERDIY_DIAGNOSTICS`, `PAPERDIY_EPUB_TESTS`, `PAPERDIY_DEVICE_TESTS`,
`PAPERDIY_NETWORK_TESTS` and external private artwork. Private configurations
must remain outside public source; never rename a private image into a public
release binary. Exact current sizes, hashes and hardware status belong in the
architecture evidence record, not in a stale build command.

## Ownership and source coordinates

`app` owns scenes/lifecycle, `input` owns button timing, `ui` draws scenes and
`reader` owns the active document, exact anchor and page state. SD, catalogue,
settings, NVS and delivery mutations are serialized on the application task.
Cooperative jobs do not authorize concurrent FAT access. TinyUSB MSC callbacks
run on the SDK task and use their own bounded ownership/mutex boundary.

`document_format.h` owns shared admission: TXT 4 MiB, EPUB source 64 MiB,
Markdown 1 MiB, all case-insensitive. Extracted EPUB text is at most 4 MiB.
Reader rendering never rewrites source files or persists preferences.

Markdown uses original UTF-8 source offsets, including a leading BOM. Generated
list markers, entities and math atoms have monotonic source/render mappings.
EPUB bookmarks refer to unchanged canonical extracted bytes. Semantic sidecars
must not insert presentation bytes into that identity. Keep the exact restored
anchor even when its containing page starts earlier; only actual navigation
adopts another page start. Invalid required recovery data must fail visibly,
not become page zero or overwrite an unsaved book.

`resumeV2` atomically stores scene, stable settings ID and a bounded selected
path. Missing V2 can read V1; corrupt V2 cannot revive stale V1. Wire key/version
must match. Legacy fields remain read-only. Save and readback must succeed
before destructive navigation, radio teardown or shutdown.

## EPUB extraction and chapter recovery

`epub::Importer::begin()` validates setup/package data and constructs a stable
primary/auxiliary chapter plan. `stepChapter()` processes one complete chapter;
`run()` is the synchronous wrapper over the same path. It preserves the prior
canonical byte order and text normalization. XML/deflate state is not suspended
or serialized between calls.

`sd_open::Job` in `sd_open_job.h` owns the cold EPUB opening lifecycle. Source
hashing is still complete; a chapter still needs bounded compressed/input/DOM
allocations. Per-byte-chunk, parser and tree-walk observers service cooperation
and latch input. Cancellation becomes terminal only after the current chapter
and any required checkpoint commit reach a safe boundary. These callbacks do
not make a blocked filesystem operation preemptible, make whole-source hashing
constant-time, or provide resumable streaming XML/deflate.

Extraction recovery uses separate files:

| Product | Contract |
| --- | --- |
| `.exttext` | Canonical prefix spool, at most 4 MiB; only the uncommitted suffix is writable |
| `.ext0` / `.ext1` | Two independently checked `PXE1` commits; 192-byte header and at most 1 MiB metadata each |
| `ECS1` inside a commit | Explicit portable Collector state, not native structs or flattened PDS |

The bounded product is at most 6 MiB plus 384 header bytes per book. ECS1 has a
96-byte header; its codec ceiling is 525,536 bytes, while fully used default
Collector capacities require at most 398,560 bytes. It retains records/parents,
resources/paths, language interns, math/TeX pools, counters and status. Ruby and
table relationships remain in the record tree. A closed nonempty chapter ends
with at least two canonical newlines, sealing the committed text prefix.
Final `finish()` may clip raw semantic boundaries; every commit stores a full
Collector snapshot, so metadata must not be saved as an append-only record tail.

A commit binds full source/dependency proof, framed chapter plan, extractor,
text/semantic revisions, chapter cursor, prefix length/digest and metadata.
Restart revalidates the source and every already-read directory dependency,
including optional missing-resource sentinels. Size/mtime alone is insufficient.
Corrupt/torn/newer slots can fall back to an older checked commit. If no commit
survives, recreate the fresh package/title and Collector state before appending
new text; failed verification may already have overwritten scratch bytes.

Only closed chapters are checkpoint frontiers. Current scheduling starts after
the first nonempty prefix, then requires at least 256 KiB or 64 additional
chapters and one second since the previous successful save; cancellation and
completion request a commit. Transfers/readback are at most 8 KiB per poll.
Open, flush, rename and individual parser/SDK calls remain synchronous.

Allocation fallback and a whole-import flat retry are volatile: they cannot
replace a richer semantic cache or become durable extraction success.
Deterministic budget fallback retains its explicit status. No ordinary book
identity, bookmark key or NVS progress is published for an unfinished prefix.
On the journal-enabled path, a fully validated completed extraction is committed
before `indexBook()` and ordinary reading publication. Optional journal failure
can permit complete RAM import with an explicit volatile outcome. Every poll
and final publication independently require the same nonzero opening lease.

## Semantic layout and fonts

Collector uses one checked optional allocation. A failure discards all
observable sidecar records; flat extraction must remain complete. The renderer
consumes PDS1/PDS2/PDS3 over the same canonical bytes. See
[EPUB typesetting](epub-typesetting.md) for CSS, vertical/ruby/TCY, MathML/table
limits, fallback and the quantified Japanese font audit.

Markdown uses vendored MD4C 0.5.2 with documented allocation, source-range,
yield and failure-cleanup changes in [PATCHES.md](../lib/md4c/PATCHES.md).
Limits are 1 MiB source and rendered text, 4,096 runs, 8,192 mappings and a
512 KiB parser budget. Parse failure exposes the full original source.

Math is a bounded TeX-like renderer, not TeX execution: at most 4,096 source
bytes, 256 nodes, depth 16, 1,024 commands and an 8 × 8 matrix. Admission reserves
128 KiB workspace and eight fixed command slots (about another 128 KiB on the
target). Slots form a bounded source-span replacement ring, not an LRU.
Recomputation after admission does not allocate. An admitted formula consumes
its complete source span once; display math owns a row. Unsupported syntax,
missing glyphs and geometry/resource failures preserve complete source fallback.

All glyphs are native bitmaps, without runtime scaling:

| Header | Source / sizes |
| --- | --- |
| `native_font_data.h` | Noto Serif CJK SC 2.003; 24/28 px, derivative NativeFont |
| `vertical_font_data.h` | Same pinned source's JAN/vertical forms at 24/28 px and ruby at 12/14 px, derivative PaperDIY JP |
| `math_font_data.h` | STIX Two Math 2.13 b171; 14/20/28 px with selected OpenType MATH metrics, derivative PaperDIY Math |

Orientation data is Unicode 17.0.0 under Unicode License v3. Regeneration uses
`tools/build_fonts.py`, `build_vertical_fonts.py`, `build_math_fonts.py` and
fixed source hashes, never installed/private fonts. The recorded JP derivative
regeneration used Python 3.14.8, Pillow 12.2.0, FreeType 2.14.3 and fontTools
4.62.1; keep actual toolchain provenance distinct from the development
requirements' Pillow 13.0.0 pin. Exact-generation tests must not be reported as
passed when fixed ignored source caches are absent.

## Cache, pagination and trust

The layout family is `0x2026a000`; eleven low bits bind the relevant geometry,
mode, language and semantic presence. `TEXT_REVISION=1` is unchanged; EPUB
requires `semanticRevision=2`. Old semantics and intermediate `0x20268000`
layouts supply checked migration anchors only, not current pages or direct
cached-text reflow. Fresh import recovers missing semantics while unchanged
canonical text preserves progress. TXT/Markdown retain audited text-family reuse.

Current complete caches use version 3. Validation checks source/dependencies,
lengths, digest, UTF-8, semantic bounds and strictly increasing pages before
exposing state. A same-size/same-mtime source can have different bytes. Two
64-entry volatile rings memoize full fingerprints and dependency graphs only
within an exclusive storage epoch; remount, source mutation, I/O error, sleep
entry and USB handoff revoke them. Reboot requires fresh validation.

EPUB first measures a page near the exact saved anchor. The local window holds
at most 32 starts; backtracking uses at most 16 page measurements from a 4 KiB earlier seed,
plus the same bounded word/UTF-8 boundary adjustment. Canonical indexing separately advances at most two pages under a
12 ms soft budget per application-loop visit. One page can exceed that budget.
Completion updates the footer, not the visible anchor. TXT/Markdown retain
synchronous indexing.

Partial pagination uses `.idx0`/`.idx1`, at most 524,624 bytes together. Its
identity includes layout, complete text/semantics and source/dependencies.
Saves begin at 32 pages, then need 128 additional pages and five seconds.
Restore cannot move the already visible anchor or invent a whole-book total.
These are page-index checkpoints, separate from chapter extraction commits.

`DeferredCacheWrite` starts after complete indexing and a checked progress save.
It snapshots metadata/pages, borrows stable text and reads/writes/verifies at
most 8 KiB per step. Pointer, length, key, layout and lease guard every step.
Only validated temporary output can replace the primary, retaining a checked
`.bak`. An Arduino `flush()`/FAT rename is not proof of power-loss durability.

An optional `.chunks` manifest binds the complete cache prefix and SHA-256 of
each 8 KiB text block; at most 512 blocks / 16,504 manifest bytes. Missing,
stale or damaged manifests fall back to full verification. The 4 MiB address
reservation and global offsets remain. Pins include up to 256 bytes of
lookbehind, the next canonical page and at least 1,024 bytes of lookahead, clipped at EOF.
Unloaded bytes cannot be read by layout. Ruby/math/table-row spans must be fully
resident; exceptional spans request full verified materialization. Failure
restores the prior pinned view and exact anchor without additional I/O.

## Settings and display contracts

A setting page is a virtual book immediately before ordinary books, using the
same row/navigation. Add one only for implemented functionality, without empty
placeholders. Stable IDs are None=0 and Network=1; retired ID2 maps read-only to
Network and cannot be reused. The first screen has one settings row and 21
ordinary books. Physical button groups remain independent across rotation.

Punctuation compression and Han/ASCII spacing default on and are configured
only through the AP WebUI. Ignore and preserve legacy `cnStyle`. Read current
style from validated `readerCfgV1`, a 20-byte versioned/CRC record with bounds
and reserved-byte checks. Save/readback precedes applying changes. Geometry
settings affect cache identity; click timing, refresh cadence and artwork do not.

P/S changes use native ink and preserve explicit spaces. Code/pre/math/rule
boundaries stop contextual prose adjustments. Code is native 24 px proportional
text, not synthesized monospace; it preserves indentation and disables normal
justification/hyphenation. Panel completion precedes clock lowering or shutdown.
Power evidence and experimental MSC's 240 MHz ownership gate are in [power](power.md).

Sleep artwork is optional `PDI1`: 16-byte header, LE dimensions, reserved zero
bytes, payload CRC32 and row-aligned MSB-first pixels, at most 540×960. The
browser fits/converts locally, without upscaling, and previews it. The device
writes/verifies an inactive SD slot before changing selection. Persist “none”
before removal; “firmware default” restores the compiled fallback. Missing or
invalid custom artwork uses that fallback. Book files/progress are unaffected.

## AP and preparation

A provides a WPA2 single-client AP with English settings/provisioning/upload UI
and local HTTP; no TLS is claimed. An exact session cookie authenticates API
requests; the QR query token bootstraps only the root page. Routes do not expose
passwords or grant authority on the LAN receiver. `wifiCfgV1` stores a checked
Wi-Fi record; legacy SSID/password keys are read-only fallback. NVS is not encrypted.

The HTTP adapter bounds headers to 3 KiB, request bodies to 17,000 bytes,
arguments to 16 and buffered responses to 32 KiB. I/O work is at most 2 KiB per
poll with five-second ordinary deadlines. Immutable compiled assets can stream
from flash up to 1 MiB under a separate 45-second response deadline. This avoids
an asset-sized RAM response; it does not preempt one blocking SDK operation.

AP admission checks name, format, size, storage, catalogue capacity, generation
and optional 128-bit request identity. Chunks are at most 4 KiB with exact
offsets. Four RAM completion receipts support same-session retries; AP restart,
card change or receipt eviction ends that recovery window. The browser keeps a
stable request ID, 12-second fetch deadline and at most three retry attempts;
unknown completion asks the user to check rather than silently import another copy.
The active transfer lease is 45 seconds of progress; AP closes after five idle
minutes, and background polling does not extend it.

The browser Worker/WASM and host CLI use the production importer/PDS serializer
and PDT1 extractor2. Source-first upload uses the returned filename for the
sidecar. Preparation supplies text/semantics, never a device page table or
bookmark. Source SHA-256 and payload SHA-256 are integrity checks, not publisher
signatures. See [prepared content](prepared-content.md) for commands, fields,
trust, hard Worker cancellation and regeneration.

## LAN identity and managed Push

`lan_nvs::Store` owns only `paperlan`. `nvs_get_blob` distinguishes Absent,
Present and Error; only NOT_FOUND permits first creation. Identity key and
certificate share one checked blob; at most four client verifier records share
another. Labels are nonempty valid UTF-8, at most 48 bytes, without C0/C1.
Open/type/read/commit errors fail closed. Uncertain credential mutation
invalidates the registry until verified reload and closes admission after the
active response drains. Secrets use constant-time comparison and non-elidable
zeroization on the target.

N exists only while explicitly selected. Device-specific P-256 TLS 1.2 uses
ECDHE-ECDSA/AES-128-GCM; the sender verifies exact certificate-DER SHA-256 before
any HTTP, token or pairing secret. Bearer authorization is separate from AP
cookies/Wi-Fi. Two 16 KiB record capacities allocate 33,434 internal buffer bytes
on the pinned SDK, plus TLS/crypto contexts. PSRAM cannot supply that internal
heap. Handshake, idle and absolute deadlines are cooperative, not crypto preemption.

The bounded store has one transaction, exact-offset 4 KiB chunks, 8 KiB
verification steps and sixteen shared client-scoped receipt slots. Ordinary
files never overwrite books. Push is at most 16 KiB Markdown in two owned SD
slots; fixed `pushRead0`/`pushRead1` progress records bind digest/length, avoiding
one NVS key per revision. Push bypasses ordinary page-cache/resident reuse.
Receipt replay precedes revision checks; identical validated Push content is a
no-op preserving its revision/anchor. Unknown publication stays Uncertain,
with recovery based on stored bytes, not an exactly-once FAT claim.
Protocol/CLI/physical inventory are in [LAN protocol](lan-protocol.md).

## Pinned USB DCD correction

The unmodified MIT-licensed controller source is pinned to lib-builder commit
`62d849ae49ac9b71c408bcc35039a5e487802806`; its exact checksum and notices are in
[third-party notices](../THIRD_PARTY_NOTICES.md) and the
[vendor record](../tools/vendor/arduino-esp32-2.0.17/README.md).
`patch_usb_dcd.py` serializes all three FIFO-empty mask updates with one shared
task/ISR spinlock. IN completion retires the old mask and acknowledges pending
completion/timeout before publishing the event, then skips old-transfer work.
TXFEMP is read-only. Critical sections cover only the register update, with no
event, queue, log or FIFO copy under the lock.

The POST hook `pio_usb_dcd.py` checks source/archive/object identities, compiles
a build-local copy and replaces exactly one SDK library token with the checked
archive. The installed framework stays unchanged; firmware builds need no
network download. Verify final link-map provenance as well as the portable
`tests/usb_dcd_patch.py` regressions; pass its `--framework` and `--ar` arguments
to include the installed-archive check. This correction has separate hardware
acceptance and does not identify the cause of a particular disconnect.

## USB ownership and firmware recovery

USB book transfer is paused while actual-SD failures remain unresolved. The
default MODE0 build retains CDC and disables MSC. Only an explicit
private `PAPERDIY_EXPERIMENTAL_USB_MSC=1` build enables the retained experimental
U path.
When enabled, U first saves progress, stops storage/network jobs, closes handles, revokes leases and
unmounts FAT. HostOwned, ExitRequested and Fault keep exclusive host ownership;
only accepted host eject and successful raw-driver release permit local FAT.
PREVENT/ALLOW, synchronization and START STOP UNIT govern that transition.
Disconnect, suspend, USB reset and idle time are not safe eject.

Callbacks transfer at most 4 KiB. The latest source holds 240 MHz throughout
ownership and retains first media/driver error plus backend/card-detect details.
A Fault cannot be treated as owned=0. Shutdown/reset/1200 touch are guarded while
the host owns the medium. A disposable RAM-disk test never accesses the SD card.
The RAM-disk MSC trial passed write/readback, ordinary eject and remount.
Actual-SD USB, the new progressive-pagination
fixture and final overlay restoration after the RGB565 correction remain
unverified. See the architecture evidence table; the RAM result does not
establish that raw SD USB is repaired.

Only after safe ownership release, native CDC recovery may use 1200 baud with DTR and RTS both asserted, followed by a newly enumerated ROM port.
Confirm the exact reader,
partition layout and selected application binary before writing only app0.
Do not erase NVS, flash bootloader/partition data or change book paths when
preserving progress. Never reset or flash while the host owns the medium.

## Tests, diagnostics and publication

Run relevant regressions, then required full native/pinned-SDK checks. Native
fixtures cover malformed XML/ZIP, UTF-8/source mappings, PDS/ECS/PDT bounds,
chapter and page restart, torn writes, poisoned unpinned text, exact anchors,
allocation failure, actual ink and formula primitive counts, geometry/footer/
rotation, protocol replay, persistence errors and USB ownership. A draw/index
parity pass alone cannot establish that an object was painted only once.

`PAPERDIY_DIAGNOSTICS` exposes bounded local commands including STATUS, SCREEN,
PAGE_TEXT, OPEN, navigation and CACHEINFO. Offset and exact anchor are separate
fields. OPEN/NEXT/PREV are mutations, not read-only probes. Optional EPUB/DEVICE/
NETWORK test flags expose only synthetic acceptance helpers; none is public.
`SD_IO` and `SD_CACHE_IO` read at most 256 KiB twice in 8 KiB chunks, comparing
SHA internally without printing names/content/digests; directories are classified
before file-open. Prefix agreement is not whole-file validation, and `read_us`
is not book-open time. Deadlines are cooperative; they do not cancel a blocked
filesystem call. No mount clock change is performed by the probe.

Keep device logs, framebuffers, backups, credentials, originals and custom art
outside public source. Native tests cannot establish electrical current,
battery lifetime, card endurance, physical wake behavior, QR readability or
wireless throughput. Report preparation, delivery, device acceptance and display
separately. Hardware data belongs in the architecture evidence table with its
actual build scope.

Use `python tools/check_public_source.py --index .` for the entire staged blobs,
and `--tree DIR` for an unpacked source artifact. The allowlist and private
markers do not replace human diff/outgoing-history review or a firmware audit.
Run `python tests/public_source.py`. Source releases must come from the reviewed
commit; firmware packages contain only the public app image, checksums,
upgrade guidance and matching licenses. Never package a working tree, handoff,
NVS/SD/flash dump, books, private logs/source-font caches or private artwork.
A successful test/CI run is not authorization to commit, push, tag or publish.
