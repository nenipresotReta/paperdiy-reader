# Development

`app` owns navigation and lifecycle; `input` owns button events; `ui` draws scenes;
`reader` shares pagination and drawing metrics; `sd_reader` handles discovery;
`epub_core` imports text; `sd_cache` validates persisted text and page offsets.

The public build generates both font sizes from a pinned OFL font. No local
font discovery, book corpus, network service, or shutdown artwork is required.
Layout changes must bump `reader_cache::LAYOUT`. Bookmarks use byte offsets;
when extraction changes, migrate a context anchor from the validated old cache
before replacing it. Keep the legacy NVS namespace and keys for recovery.
The current migration reader accepts v1 caches. A future extraction change
must add read-only migration from v2 before changing its layout identifier;
an unchanged text hash can already retain its bookmark across font changes.
NVS read/write failures must not become page zero or permit an unsaved book to
be replaced. Rendering never writes preferences.

XML input is validated before every TinyXML2 parse: at most 32 element levels,
1 MiB decoded text, and at most 2 MiB of conservatively estimated DOM pools.
Pool costs use actual target object sizes rather than host sizes. The device also
checks available PSRAM and the largest free block. UTF-8/UTF-16, whitespace,
numeric and fixed XHTML named entities are normalized without loading DTDs.
Auxiliary spine items are appended after the main reading order.
The loop task has a 16 KiB stack; hardware boundary tests require at least
4 KiB remaining after parsing and recursive document destruction.

Caches hash source bytes, verify a temporary file before replacement, and keep
one previous generation as `.bak`. Cache hits still read the source to detect
same-size, same-timestamp replacements. Only the cache directory is written.
Cache I/O uses an 8 KiB buffer and yields after 128 KiB or 50 ms, avoiding a
full scheduler tick on every small read while keeping long operations cooperative.

After installing requirements, generating fonts and running `pio run`:

```sh
cmake -S . -B build -DSANITIZE=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Native tests require CMake, a C/C++17 compiler and OpenSSL development headers
(`libssl-dev` on Ubuntu; `brew install openssl@3 cmake` on macOS, then pass
`-DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"` to CMake).
Tests cover button state transitions, release guards, refresh decisions,
cache corruption and I/O failures, source fingerprints, real reader integration,
bookmark migration and NVS failures, font metrics and independent raster
comparisons, and synthetic EPUB syntax/encoding/resource boundaries.

Releases provide source and a separately built public application binary.
See [upgrading](upgrading.md) for its scope and checksums. Application-only
updates with the same partition table preserve NVS; do not erase flash or change
book paths if you want to retain progress. SD books are never rewritten.

Hardware acceptance: verify SD mounting, page turns, charging-only LED,
A+B shutdown, and boot resume. Panel ghosting and power behavior need a real
device; native tests cannot validate those effects.
The [idle power design](power.md) separates the pinned SDK source review from
completed basic device checks from remaining stress tests and electrical measurements.

For local hardware verification, `PAPERDIY_DIAGNOSTICS` enables bounded serial
commands (`STATUS`, `SCREEN`, `OPEN n`, `NEXT`, `PREV`, `ENTER`, `LIBRARY`,
`RESCAN`, `SHUTDOWN_PREVIEW`, `REFRESH`, `PAGE_TEXT`, `CACHEINFO`).
`CACHEINFO` reports cache headers without changing files.
Also defining `PAPERDIY_DEVICE_TESTS`
enables `XML_CHECKS`: three in-memory parser boundary checks that leave books
and preferences untouched. Neither flag is enabled in the normal build.
An optional `PAPERDIY_SHUTDOWN_HEADER` may name a local bitmap header; keep that
file and the configuration supplying it outside this repository.
