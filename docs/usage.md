# Operation

[English](usage.md) | [简体中文](usage.zh-CN.md)

## Library and files

Use a FAT32 microSD card. Copy files onto it, safely eject it, then insert it into the reader; the AP page also accepts uploads. The flat library has 22 rows per screen, with a settings entry and 21 books on the first screen, and at most 256 books. Folder navigation and web folder management are not implemented.

| Format | Source-file limit | Notes |
| --- | --- | --- |
| EPUB | 64 MiB | Extracted text ≤4 MiB; unpacked `.epub` directories also work |
| TXT | 4 MiB | UTF-8 |
| `.md` / `.markdown` | 1 MiB | UTF-8; see [Markdown](markdown.md) for formatting and formulas |

Extensions are case-insensitive. Each book retains its reading position. For EPUB, on first open or after a layout change, the reader shows a page near the saved position and continues pagination; the total remains unknown until completion. Valid caches can load text near that position directly, and progressive checkpoints retain partial indexing work. Source validation and initial extraction still take time. Replacing a file or changing its path can affect progress matching. Preserve SD files and NVS; upgrading does not require formatting the card or erasing all flash.

## Buttons

| Scene | A | B | Short A+B |
| --- | --- | --- | --- |
| Library | Previous book; double-click previous screen; hold to repeat | Next book; double-click next screen; hold to repeat | Open selection |
| Reading | Previous page; hold to repeat | Next page; hold to repeat | Return to library |
| Network settings | Cycle N → A → P → Off | Toggle R (180°) | Return to library |
| Network settings, delivery prompt shown | Next mode (turns N off, which cancels) | Toggle R (180°) | Accept the batch |

The library detects a double-click when releases of the same button are at most 200 ms apart by default; otherwise they are two single clicks. A single click waits up to the configured double-click interval (default 200 ms). Holding starts repeat after about 600 ms. Navigation during loading or refresh is discarded; release the buttons before a new action. The bottom icon indicates busy work.

R saves display rotation. Rotated library/reading navigation swaps A/B; settings retain physical button mappings. N/A/P stay on one row, with R nearest the buttons. A+B and the power key are unchanged.

## Chinese typography

Adjacent-punctuation compression and Chinese–Latin spacing are enabled by default and adjustable under Advanced typography in the AP page; there is no separate device page. Supported punctuation pairs use their actual glyph whitespace, and adjacent Han/Latin letters or digits receive a small gap. Explicit spaces do not receive an extra gap. Markdown code and formulas do not use these adjustments.

## AP transfer and Wi-Fi setup

1. Select the first library settings row, press A+B, then use A to select AP mode A.
2. Scan the Wi-Fi QR. Each A session gets a fresh SSID/password and admits one client.
3. Once connected, the same QR changes to the web address. Scan again to open the page and upload a book.
4. After success, return to the library and open it. Chinese filenames are retained; duplicate names receive a new filename rather than replacing a book.

Supported browsers preprocess EPUB locally before uploading the original and companion content. This reduces initial extraction work on the device; validation and pagination remain on the reader. If preparation fails, upload only the original. See [EPUB preparation](prepared-content.md).

The screen retains the SSID/password. After disconnection, the QR returns to Wi-Fi setup. Stay connected if the phone warns that the AP has no internet.

Transfers are chunked and enter the library only after complete commit. Lost responses receive bounded retries within the same session. If failure remains, inspect the web library before retrying; closing the page, disconnecting or rebooting does not promise resume. Unfinished transfers are cleaned up without changing existing books.

Enter the home SSID/password under Home Wi-Fi and save, then select N. Failed saves retain the old configuration; passwords are never echoed or logged. AP credentials last only for the session. Home credentials remain in unencrypted NVS.

A closes after five idle minutes. Background library polling does not extend the session; progressing transfers keep it active, while 45 seconds without progress cancels reception. The AP serves local HTTP without TLS; QR session credentials apply only to that AP session.

## N / A / P / Off

- **N** connects to saved home Wi-Fi and receives LAN deliveries while Settings stays open. Below the SSID, status and IP, the screen shows a delivery prompt or transfer type/name/progress. A dropped connection gets a fresh 20-second connection window; timeout turns the radio off until N is selected again.
- **A** provides a temporary AP, web uploads and home Wi-Fi setup.
- **P** (live Push) stays connected after you leave Settings, so a sender paired earlier in N can update Push at any time without A+B. An open Push page refreshes immediately; the library and other books are not interrupted. Ordinary files and pairing still use N. While P is selected, a small icon at the bottom-left of the library and the reader, opposite the status icon, shows its state: a dot under an arc (online), a dot alone (connecting or retrying), or the dot and arc struck through (paused or stopped; the reason is in Settings).
- **Off** closes networking.

P is meant for a reader on external power. On battery it keeps the radio awake while you read; below 50 % it pauses until external power is connected, and it stops after 30 seconds without a connection, or 60 seconds after losing one, for example when you take the reader out of the house. On external power it keeps retrying (1, 2, then every 5 minutes). P is restored after a reboot unless it stopped by itself. Selecting P needs saved home Wi-Fi, a paired sender and an SD card; without one, Settings shows which one is missing.

USB book transfer is paused and disabled by default because actual-SD USB failures remain unresolved. It is no longer a network mode; ordinary builds do not expose the card. CDC serial/programming remains available. Transfer books through A or a safely removed SD card. The retained MSC implementation requires an explicit experimental build; it is not normal user operation.

Inactive entries carry `¬`. Leaving settings closes N and A; P keeps running. After reboot, restored settings start with N/A inactive. USB attachment enables neither book transfer nor networking. Reading settings and shutdown-image management are available only through A.

N and P use the [LAN client](lan-protocol.md) on macOS, Linux or Windows (P: `push SOURCE --live`); an AI agent can use the [delivery skill](skills/paperdiy-delivery/SKILL.md). Each delivery asks first:

```
Hermes · Receive 3 files?
notes.md
ch1.md
Push: Weekly
Replaces Push: Last week
A+B accept · A to cancel
```

Short-press A+B to accept the whole batch, then stay on Settings until the panel shows `Done · 3 files`. Press A (N turns off) or leave Settings to cancel; after 60 seconds the prompt expires. Holding A+B still shuts down and never accepts. A computer that has never sent to this reader shows `New sender · 482 913` on the first line: accept only if the number matches the one the sender (or your agent) reports; that one A+B pairs it and accepts its files. Later connections pin the reader's certificate. While N is active the reader announces itself on the local network, so senders usually need no IP address; give the reader a DHCP reservation for networks where that does not work.

Paired senders are listed in the AP page under Senders, where you can remove one or forget all of them; a removed sender must pair again. Ordinary files enter SD `Books` without overwriting names. Push is one fixed library entry containing at most 16 KiB UTF-8 Markdown with supported formulas. New content replaces it; bounded history stays on the sender instead of accumulating books on the reader. With N off, the device receives nothing. A bounded sender queue can sync after N is enabled again. Delivery never automatically opens a book or changes the current reading position.

## Japanese and EPUB objects

Supported EPUB content declaring `vertical-rl` flows top-to-bottom and right-to-left. A language tag alone does not rotate horizontal Japanese. Bounded ruby, two-digit text combination, some MathML and simple tables are supported. Complex CSS, nested tables and unsupported objects retain readable text fallback. See [EPUB typesetting](epub-typesetting.md) for fonts and compatibility.

## Shutdown, startup and battery

Hold A+B for at least two seconds, then release after the shutdown scene appears. Shutdown saves progress first; a save failure leaves the reader on for retry.

Startup restores the prior reading scene, library selection or settings; a first boot opens the library. Reading resumes by saved text position, so page numbers can change with layout. Public firmware has no private shutdown image. A private build may supply `PAPERDIY_SHUTDOWN_HEADER` outside the repository.

The library shows battery state, and the bottom icon shows device state. Conditional idle sleep should execute the first press directly. Active networking blocks manual light sleep. The charging LED follows charging state. See [power behavior](power.md) for evidence and limits; electrical current still requires measurement.

## WebUI settings

All controls use English labels. Open the existing AP Web UI; no extra settings
book is added on the reader. Save explicitly; settings persist offline.

| Control | Options / default |
| --- | --- |
| Reading preset | Compact: native 24 px, 34 px line spacing, 18/24 px margins; Standard (default): 28/40 px, 24/30 px margins; Relaxed: 28/46 px, 36/42 px margins |
| Automatic English hyphenation | On by default; applies where the document supports English hyphenation |
| Alignment | Justified (default) / Left aligned |
| Compress adjacent Chinese punctuation | On by default, under Advanced typography |
| Chinese–Latin spacing | Off / Compact (2 px) / Standard (4 px, default) |
| Double-click interval | 100–400 ms, default 200 ms; measured between releases |
| Screen refresh | Clarity / Balanced (default) / Less flashing |
| Sleep background ink | 0–100%, default 50%; monochrome ordered dithering approximates intermediate values |

Presets affect prose, not interface labels or the native 24 px code font. Math
keeps its native 28/20/14 px sizes. Typography changes apply when a book is next
opened: pagination is rebuilt around the saved source position, not a page number.
Balanced allows 10 consecutive partial reading updates before a full refresh;
Clarity allows 4 and Less flashing 24. Scene changes still use a full refresh.
Status updates (P icon, battery, loading icon, page count, delivery progress)
never cause a full refresh and do not count toward these limits.

Upload a PNG, JPEG or WebP image up to 12 MiB under Sleep image. The browser
converts it locally into a bounded monochrome mask and shows a preview; white
areas are transparent over the dimmed page. The image keeps its aspect ratio,
fits within 540×960 without upscaling, and is aligned bottom right. The SD card
stores the custom mask; it is never included in public source or firmware.
Remove image disables the overlay and removes the two custom upload slots when
the SD card is available. Use firmware default restores the compiled fallback
(private artwork on a private build, no artwork on the public build). Missing or
invalid custom artwork falls back to that firmware default. Book files and
reading records are unaffected.
