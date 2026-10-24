# Prepare EPUB content on a phone or computer

[简体中文](prepared-content.zh-CN.md)

The AP WebUI can prepare an EPUB locally in the phone browser before uploading
it. The `prepare_epub` command offers the same operation on a computer. Both
use the firmware's production importer and create an optional
`example.epub.pdt` file beside an unchanged `example.epub`. The reader still
validates the source and prepared payload, computes pages for its current font
and settings, and saves its own cache and reading position.

This reduces work on an initial import. It does not eliminate SD reads or the
first pagination pass, and it does not promise a particular opening time.
Existing valid page caches are already the preferred path after preparation.

## Prepare in the AP WebUI

Open the reader's current hotspot page using its QR code and select an EPUB.
**Prepare EPUB for faster opening** is enabled by default when the browser
supports WebAssembly and Web Workers. Press **Upload** once. The browser
prepares the book locally, uploads the original, then uploads its matching
prepared content under the actual filename returned by the reader.

The worker, JavaScript loader and WebAssembly module are served directly by
the reader at `/prepare-worker.js`, `/prepare-core.js` and
`/prepare-core.wasm`. These routes require the current AP session. No CDN,
Internet connection, browser secure-context crypto service, or companion server
is required. The source `File` is passed to a worker; the page does not read the
whole archive on its UI thread. The worker runs the production C++ importer,
semantic serializer and `PDT1` codec, with a portable SHA-256 implementation.

**Cancel** terminates the worker during preparation and stops subsequent
uploads. If preparation fails or times out, **Upload original** remains
available. If the original is already saved but prepared delivery fails,
**Retry faster-opening file** retries only the prepared file. Closing the page
loses its unsent temporary result; it does not remove a saved original. Existing
books and their progress are never replaced by selecting a duplicate filename.

The WebAssembly heap grows from 16 MiB to a hard ceiling of 128 MiB, with a
256 KiB stack. The input still has the 64 MiB EPUB limit below. The browser also
needs memory for its source buffer, engine and UI outside that heap; a phone
can reject a large preparation earlier if its own memory limits are lower.
Unsupported browsers can still upload an original. Preparation is local
extraction, not page rendering: the reader computes pages with its own fonts
and current settings.

## Build the computer tool

Use Linux or macOS with a C++17 compiler, CMake 3.16+, Python 3.12+, OpenSSL
development headers and Node.js 18+. Node is required by the project's current
CMake configuration even when building only this tool.

From the repository root:

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r requirements-dev.txt
pio pkg install -e paperdiy
cmake -S . -B build -DSANITIZE=ON
cmake --build build --target prepare_epub epub_core prepared_content epub_document --parallel 2
ctest --test-dir build -R '^(prepared_content|prepared_epub|epub_document)$' --output-on-failure
```

PlatformIO installs the pinned M5GFX dependency used by the shared decompressor.
The host build does not flash a device. If dependencies were installed elsewhere,
pass their existing location to CMake:

```sh
cmake -S . -B build -DM5GFX_DIR=/path/to/M5GFX -DSANITIZE=ON
```

On macOS with Homebrew OpenSSL, CMake may also need:

```sh
cmake -S . -B build -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)" -DSANITIZE=ON
```

Use `-DSANITIZE=OFF` in a separate build directory when measuring host
performance. Sanitized and release timings are not comparable.

## Prepare a local file

Keep books and generated sidecars outside the source checkout. For example:

```sh
build/prepare_epub /path/to/example.epub /path/to/prepared/example.epub.pdt
```

The output directory must already exist. Generate on a local filesystem that
supports hard links, such as APFS or ext4; the tool's atomic no-overwrite
publication does not support generating directly on FAT32. Copy or upload the
completed output to the SD card afterward.

Success returns exit code 0 and one JSON object containing format, extractor
version, byte lengths, package-language code and SHA-256 digests. It prints no
book text. A nonzero exit code means preparation did not publish the output.
An existing output is never replaced; choose a new destination to regenerate.
The input is opened read-only and fingerprinted before and after extraction.

Ordinary preparation includes supported headings, quotes, emphasis, code,
local language ranges, source-declared vertical layout, bounded ruby, MathML and
simple tables. The supported subset is documented in [EPUB typesetting](epub-typesetting.md).
If optional structures exceed their bounded budget, the tool can produce
readable flat text with `metadataBytes: 0`.
`--flat` explicitly requests that form for diagnostics:

```sh
build/prepare_epub /path/to/example.epub /path/to/prepared/example-flat.epub.pdt --flat
```

Rename that diagnostic output to the exact source filename plus `.pdt` before
delivery. The default semantic form is recommended for normal reading.

## Deliver through the AP WebUI

1. Open the reader's settings book and select **A**. Join its displayed hotspot
   and open the current QR page. The page is authenticated for that AP session.
2. Under **Upload book**, upload the original `.epub` first. Wait for the
   **Uploaded: …** result and retain the returned filename.
3. Name the prepared file after that exact returned filename, followed by
   `.pdt`, then upload it through the same form. The page selects the prepared
   upload type from its `.epub.pdt` suffix.
4. Leave settings and open the original book. The prepared file is hidden from
   the book list. The reader checks it before using its text or metadata.

If `example.epub` already exists, the source upload allocates another filename;
it does not overwrite the existing book. For example, if the response returns
`example-2.epub`, deliver `example-2.epub.pdt`. Renaming the sidecar does not
change its source hash: its original must still have exactly the same bytes.
Do not assume the requested filename was used.

For a book already present, upload only its matching sidecar. Its leaf filename
must identify exactly one regular EPUB in the library. Two books named
`example.epub` in different directories are ambiguous to the AP route, so that
upload is rejected. Direct SD copying can place the sidecar beside the intended
source without this ambiguity.

The complete `.epub.pdt` filename is limited to 96 UTF-8 bytes by AP admission;
choose an original name of at most 92 bytes when planning a paired upload.
Preserve exact capitalization when matching the book's name.

The existing AP session has an inactivity timeout. Keep the page open until
upload completes. A lost response is handled by the page's transaction-status
and retry logic; do not blindly re-upload the original and create duplicates.
An **Uploaded** receipt confirms delivery, not that the reader has displayed
or accepted the semantic content yet.

### AP client integration

An agent client can use the same transaction protocol while the AP session is
open. Obtain the current page session from the device's QR URL; subsequent
requests use its `diy_token` cookie. Do not embed an AP token in a script, a
public skill or source control. POST bodies are
`application/x-www-form-urlencoded`, not multipart uploads.

| Request | Fields / result |
| --- | --- |
| `GET /api/status` | Read `sd`, `busy`, `free` and `uploadGeneration` |
| `POST /api/upload/start` | `name`, `size`, `generation`, fresh 32-character lowercase hexadecimal `requestId`; add `kind=prepared` for `.epub.pdt` |
| `POST /api/upload/chunk` | Returned `id`, exact next `offset`, canonical base64 `data`; honor returned `chunkSize` |
| `GET /api/upload/status?id=…` | `receiving` with accepted offset, or `complete` with final name |
| `POST /api/upload/commit` | `id`; returns `ok` and actual saved `name` |
| `POST /api/upload/cancel` | `id`; cancels the active owned transaction |

Upload the source with no `kind`, then start a separate transaction for its
sidecar with `kind=prepared` and the returned source name plus `.pdt`. Fetch a
fresh generation for the second transaction. The source and sidecar are two
independent publications; a failed sidecar never rolls back or replaces the
original book.

After a timeout, reuse the same request identity and metadata for admission,
or query the existing transaction before resending data. An acknowledged chunk
must not be appended twice. A completed commit can be retried against its
receipt. Four completion receipts are retained in RAM for the AP session;
reboot or session shutdown ends that recovery window. Report preparation,
delivery and on-reader acceptance as separate outcomes.

## Copy through an SD reader

With the card removed from the powered-off reader, copy both files into the
same directory, for example:

```text
/Books/example.epub
/Books/example.epub.pdt
```

Safely eject the card before returning it to the device. Do not modify a card
while the firmware owns its mounted filesystem. USB book transfer is paused
and disabled by default; use AP upload or a safely removed card. CDC remains
available, while MSC requires an explicit experimental build. See
[USB ownership and recovery](development.md#usb-ownership-and-firmware-recovery)
for the ownership contract and current hardware acceptance limits.

## Limits, validation and fallback

| Item | Current limit or rule |
| --- | --- |
| Input | One regular ZIP-based EPUB, 22 bytes to 64 MiB |
| Extracted text | Nonempty UTF-8, at most 4 MiB |
| Optional semantic metadata | At most 352,808 bytes; bounded `PDS1`/`PDS2`/`PDS3` semantics, including at most 16,384 runs |
| Prepared file | 160-byte header + text + metadata; at most 4,547,272 bytes |
| Extractor identity | `PDT1` version 1, extractor ID 2; older sidecars require regeneration |
| Language field | 0 = no English patterns; 1 = US; 2 = GB |
| Device page index | Computed locally; absent from the prepared file |
| Directory EPUB | Supported by the reader importer, **not** by either preparer or the prepared upload route |

The normal importer's chapter, XML, path and archive-entry limits still apply.
Preparation is not a converter for DRM, unsupported encryption, malformed ZIP
files, oversized chapters or unsupported layout semantics. See
[the implementation report](architecture-and-implementation.md#5-admission-and-epub-extraction)
for those independent bounds.

At AP commit, the device checks the envelope, transfer payload digest and
original-file size. On opening, it rechecks the prepared bytes read from SD,
the original EPUB's complete SHA-256 identity, UTF-8 and semantic ranges before
display. Unknown versions, wrong-source sidecars, corruption or invalid metadata
cannot become visible prepared content. The reader retains a usable existing
cache where available, otherwise falls back to ordinary import. A newly supplied
valid sidecar can upgrade an older flat cache while preserving its reading anchor.

SHA-256 detects mismatches and corruption; it does not prove that an arbitrary
sender's extracted text faithfully represents the EPUB. Use the bundled tool
from trusted source, and accept prepared files only from a trusted local sender.
The AP interface is an explicit local session, not an authenticated general LAN
push service. Both the native CLI and browser worker use the same production
importer; a SHA-256 digest is an integrity check, not a publisher signature.

Prepared files contain readable book text and metadata. Treat them as private
book data: do not add them to public source, releases or diagnostic reports.
They contain no Wi-Fi credentials or device reading records, but that does not
make their book contents public. The original EPUB remains the recovery source.

The wire schema and research rationale are documented in
[performance-research.md](performance-research.md#5-optional-host-and-phone-preprocessing).


## Rebuild the browser assets

The committed `src/prepared_web_assets.h` contains gzip-compressed JS/WASM as
ASCII C++ arrays. Firmware builds use these committed assets without invoking
Emscripten or regenerating them. Regeneration is a separate pinned-toolchain
operation. Rebuild these assets whenever the importer, semantic codec,
prepared envelope, XML parser or browser adapter changes. Ordinary CI executes
the committed module against the native importer; regeneration is a separate
pinned-toolchain check.

Install the exact toolchain outside the source checkout, following the
[official emsdk workflow](https://emscripten.org/docs/getting_started/downloads.html):

```sh
git clone https://github.com/emscripten-core/emsdk.git /path/to/emsdk
cd /path/to/emsdk
git checkout eb5eb4b3b2b792f44ded198bfe615c985da5be2c
./emsdk install 4.0.18
./emsdk activate 4.0.18
```

Return to the repository root after installing the pinned PlatformIO
M5GFX dependency, then run:

```sh
python3 tools/build_browser_prepare.py \
  --emsdk /path/to/emsdk \
  --m5gfx .pio/libdeps/paperdiy/M5GFX \
  --build-dir /path/to/browser-build
```

The recipe verifies the compiler version and decompressor source hashes. It
compiles the C decompressor and production C++ code separately, then links an
optimized modularized worker module. Gzip timestamps and OS bytes are fixed.
The public header contains no machine paths or book inputs. Raw `.js`, `.wasm`,
objects and the JSON build receipt stay in the selected build directory; do not
add that directory to public source. The recipe fails if compressed core assets
exceed 1 MiB. The recorded integration assets use 3,991 bytes for the gzip loader and 42,449
bytes for gzip WASM (46,440 bytes total), excluding the small worker wrapper.

To verify byte-for-byte reproducibility without rewriting the checked-in header:

```sh
python3 tools/build_browser_prepare.py \
  --emsdk /path/to/emsdk \
  --m5gfx .pio/libdeps/paperdiy/M5GFX \
  --build-dir /path/to/independent-browser-build --check
```

Run the actual committed WASM in Node against the native tool:

```sh
python3 tests/browser_prepare.py --node node \
  --native build/prepare_epub --importer build/epub_core
```

The regression captures the existing production EPUB corpus, compares whole
prepared files in both semantic and flat modes, checks SHA-256 against Node's
crypto implementation, includes an exact 64 MiB source, rejects oversized
requests, and terminates a worker during CPU-bound preparation. The current
corpus has 108 successful byte-identical cases and 94 rejected cases, including
external-CSS vertical/ruby/tate-chu-yoko and MathML/simple-table fixtures.
Directory EPUB and host-only fault-injection modes are not browser inputs.

For a real allocation-failure check, build a separate capped test module:

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

This checks source/text allocation exhaustion and subsequent recovery. A test
heap cannot overwrite the production asset header. Node execution establishes
native/WASM data parity and worker cancellation; it does not by itself prove
physical phone browser performance or a successful AP upload. Those are
separate integration checks. Compiler/runtime notices are retained in
[the third-party notices](../THIRD_PARTY_NOTICES.md).

### Browser integration evidence

An actual desktop browser has fetched and executed the production worker and
WASM under production CSP on an HTTP service bound to `127.0.0.1` at an
ephemeral port. The page completed a synthetic source-first/prepared-second
upload and correctly used the server-returned renamed source for its sidecar.
The 9,184-byte original and 706,858-byte PDT matched independent references,
and the browser reported no console errors or warnings. This checks the real
page, worker and generated assets against a local transport; it does not
establish ESP32 AP delivery, physical phone performance or browser background
behavior. No fixture was uploaded to the device in this trial. Follow
[test-host isolation](development.md#test-host-isolation); do not switch the
developer computer's network to run an integration test.
