# Development

`app` owns navigation and lifecycle; `input` owns button events; `ui` draws scenes;
`reader` shares pagination and drawing metrics; `sd_reader` handles discovery;
`epub_core` imports text; `sd_cache` validates persisted text and page offsets.

The public build generates both font sizes from a pinned OFL font. No local
font discovery, book corpus, network service, or shutdown artwork is required.
Layout changes must bump `reader_cache::LAYOUT`; bookmark keys use text offsets
and survive repagination. Keep the legacy NVS namespace for compatible updates.

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
cache corruption and I/O failures, and synthetic EPUB import cases.

This first release distributes source code; CI compiles it without publishing
firmware binaries. Application-only updates
with the same partition table preserve NVS; do not erase flash or change book
paths if you want to retain progress. SD books are never rewritten.

Hardware acceptance: verify SD mounting, page turns, charging-only LED,
A+B shutdown, and boot resume. Panel ghosting and power behavior need a real
device; native tests cannot validate those effects.
