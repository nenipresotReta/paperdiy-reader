# MD4C 0.5.2

Pinned upstream: https://github.com/mity/md4c/tree/release-0.5.2 (MIT).
Only the parser is vendored; no HTML renderer or command line tool.

Original source checksums:
- `md4c.c` SHA-256: `52e39843b96b64d0281bf795128bcc88c9c4dc52d7436737e9f4306648dc4590`
- `md4c.h` SHA-256: `4efd19bf7ec270691d5b4189f496886e421768a814b5e817eb945aa85e859f18`
- `LICENSE.md` SHA-256: `d30937367d5413e7eaa218b1640b8946ff76fd34d97152f6979fd96169d5d0fc`

Local changes:

- Add `md_parse_ex` with per-parse checked allocation and cooperative tick hooks. All parser allocations use those hooks; `md_parse` keeps its default allocation behavior. This lets the device enforce a 512 KiB transient parser budget in PSRAM.
- Add raw source ranges to math span, table, and rule details. Math consumers use the original bytes, preserving newlines and TeX percent comments that MD4C's normalized text callbacks would otherwise change.
- Recognize `\(...\)` and `\[...\]` in the mark collector before ordinary backslash escapes, outside code spans/blocks. Failed closer searches are memoized to retain linear scanning. Escaped backslashes remain ordinary text, and incomplete opening delimiters remain visible.
- Recognize complete standalone `$$` / `\[...\]` blocks before normal Markdown block classification, so blank lines and leading `+`/`-` inside formulae cannot become paragraphs/lists. The first unescaped closer must end its line; inline `$$` still uses the upstream span parser.
- Skip an initial UTF-8 BOM during block scanning while retaining original byte offsets and file identity.
- Tick during line/mark collection and inline callbacks, including long ordinary lines.
- Fix partial allocation failure ownership in `md_build_attr_append_substr`: retain the first reallocated array before allocating the second, so cleanup cannot free a stale pointer. Also zero freed attribute state and initialize both fenced-code attribute builders, making outer cleanup safe when an inner build fails. Allocation failure tests exercise every allocation of reference-link and code fixtures.

All offsets are byte offsets in the original UTF-8 input; no source rewriting.
