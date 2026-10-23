# LAN document and message delivery

Status: review draft, 2026-10-23. This document proposes future work; it does not
enable a LAN service. The shipped N mode only joins saved Wi-Fi and shows its
state/IP. It has no Web UI, push endpoint, cloud connection or background inbox
polling. A mode remains the existing, separately authenticated AP upload page.

## Scope and recommendation

Start with a local CLI sending one document while the user explicitly selects N.
Use the same bounded upload/storage transaction as A, followed by the same
on-device document reader. Add authenticated discovery and TLS transport around
that transaction. Do not expose the AP page, Wi-Fi settings, filesystem browsing,
bookmarks, arbitrary deletion, firmware updates or command execution over N.

Keep networking tied to the existing settings lifecycle: leaving settings,
selecting Off/A/U, shutdown and reboot stop the N service. USB power alone never
starts it. Opening a received document consequently stops N in the first version.
Receiving while reading would require a separately reviewed lifecycle change;
removing `networkStop()` on navigation is not an acceptable shortcut.

Document delivery is the first milestone. Short messages can subsequently use the
same UTF-8 Markdown ingestion path with a smaller quota and an explicit inbox.
They must not replace the current page, change reading progress or synthesize
button presses. A footer notification is sufficient when the current screen can
show one; inbox navigation and unread handling need their own UI review. There
is no requirement for a resident desktop daemon, external relay or account.

## Existing behavior and reusable boundaries

The current implementation is in [network.cpp](../src/network.cpp),
[bounded_web_server.h](../src/bounded_web_server.h),
[bounded_http_request.h](../src/bounded_http_request.h) and
[app.cpp](../src/app.cpp). The following are implemented, not LAN promises:

- A accepts one file transaction and 4 KiB decoded base64 chunks. Admission uses
  a generation and optional client `requestId`. Status reports an accepted offset;
  commit replays return the stored result without importing again.
- The last four completion receipts exist only in RAM for the current AP/SD
  session. Leaving A or changing storage clears them. Losing a station connection
  currently aborts an unfinished AP upload. Reboot recovery is not implemented.
- Active uploads have a 45-second progress lease. A closes after five idle
  minutes; background status/file polling does not refresh its activity clock.
- The HTTP adapter bounds headers to 3 KiB, bodies to 17,000 bytes, responses to
  32 KiB, I/O work to 2 KiB per poll, and request/response time to five seconds.
  It bypasses the pinned SDK's multipart parser and uses connection-close HTTP.
- [document_format.h](../src/document_format.h) owns input formats and limits:
  TXT 4 MiB, EPUB 64 MiB, Markdown 1 MiB. The library limit is 256 books.
  Temporary files are owned by the importer; committing never overwrites a book.
- N reconnect reporting gets a fresh 20-second grace period after a previously
  established connection drops. A network connection keeps manual light sleep
  disabled. Returning to Off restores the existing offline power policy.

Before implementing LAN, extract an `UploadStore` from the network callbacks.
It owns validation, admission, byte offsets, storage leases, commit results and
cleanup; it receives explicit storage/network lifecycle events. AP and LAN
adapters supply authenticated owners and bounded request bodies. Neither adapter
calls UI rendering, changes a bookmark or writes Wi-Fi configuration through the
store. SD, the catalogue and receipt changes stay serialized on the application
owner task; a TLS worker may enqueue bounded work but must not mutate them.

## Discovery is not authentication

Advertise only while N has an address and its receiver is ready. Proposed DNS-SD
service type is `_paperdiy._tcp` with a hostname such as `paperdiy-<random-id>.local`.
This is a proposed private service name, not a claim of IANA registration. The
random device ID is stable and independent of the MAC, SSID and account identity.
Use a new ID when the user resets pairing. TXT data should be small: protocol
major version, TLS requirement and supported content types. Do not advertise
book names, reading state, client names, Wi-Fi credentials, pairing secrets or
bearer tokens. A stable ID is still visible to other peers on that LAN.

mDNS provides local hostname/service discovery; DNS-SD uses PTR, SRV and TXT
records to locate a service and describe it. Those records locate an endpoint;
the client still authenticates the device. Remove the advertisement on address
loss or service stop and republish on a new address. Use the displayed IP as a
fallback when multicast or client-to-client traffic is restricted; do not scan
unrelated subnets. See the [ESP-IDF mDNS API](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32s3/api-reference/protocols/mdns.html)
and [DNS-SD specification](https://www.rfc-editor.org/rfc/rfc6763.html).

## Pairing, credentials and transport

Pairing is an explicit physical action, separate from discovering a device. A
future device pairing screen can show a QR payload with protocol version,
device ID, public-key fingerprint and a one-use random pairing secret. Proposed
limits are two minutes per pairing window and at least 128 bits of entropy.
The CLI reads that payload locally, pins the TLS public key before exchanging
anything secret, and receives a device-generated 256-bit client token. No remote
request can silently open a pairing window. The concrete two-button pairing and
revoke UI remains a prerequisite for implementation.

Use a per-device key generated on the device, never a private key embedded in
public firmware. Validate the pinned identity on every connection, even when
mDNS reports a changed IP. Do not implement a fallback that disables certificate
verification. A deliberate pin is an out-of-band trust anchor, not a generic
"accept any self-signed certificate" exception; HTTP explicitly allows a client
to use a configured alternative identity such as an expected certificate.
[HTTP server identity verification](https://www.rfc-editor.org/rfc/rfc9110.html#section-4.3.4)
describes this distinction. Clock/bootstrap and certificate renewal must be
handled explicitly in the chosen pinned-key client, without public NTP or an
external certificate authority being necessary for local transfers.

Allow at most four paired clients initially. Issue separate revocable tokens,
store only token verifiers on the device, and store the client token in the OS
credential store or a user-only local configuration. Use an `Authorization`
header over TLS; no query token, browser cookie, logs or mDNS TXT field. Every
status and write request is authenticated and scoped to its client. AP session
cookies and home Wi-Fi credentials have no authority on N. Bearer credentials
need protection in transit and storage; this principle is documented in
[RFC 6750](https://www.rfc-editor.org/rfc/rfc6750.html#section-5), without requiring
an OAuth service for this local protocol.

The current AP transport cannot become TLS merely by moving it to port 443:
its response writer operates on raw TCP descriptors. Evaluate a separate bounded
TLS adapter or ESP-IDF HTTPS server, preserving limits, cancellation and watchdog
responsiveness. Limit concurrent TLS sessions to one initially and bound the
handshake. Keep a connection only for an active bounded transfer, then close it.
The pinned SDK documents substantial initial TLS setup cost and lower cost for
subsequent requests on an existing secure socket; these are reference figures,
not measurements on this device. [ESP-IDF HTTPS server](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32s3/api-reference/protocols/esp_https_server.html)
is the starting point for a transport spike.

## Proposed protocol v1

These paths and fields are a review contract, not implemented API. Keep the
existing AP `/api/...` protocol compatible; the LAN adapter has a distinct
`/lan/v1/...` namespace and no HTML routes. Version the wire protocol separately
from document format, cached layout and firmware release. Reject an unsupported
major version; additional response fields can be optional within v1.

| Request | Purpose and result |
| --- | --- |
| `GET /lan/v1/capabilities` | Authenticated version, formats, byte/chunk limits, current random `epoch`, admission generation and receipt capacity. No library listing. |
| `POST /lan/v1/uploads/start` | `epoch`, `generation`, random 128-bit `requestId`, name, kind, source size and SHA-256; returns ID, accepted offset and progress lease. Identical replay returns the existing transaction. |
| `GET /lan/v1/uploads/status?id=...` | Owner-scoped `receiving` or `complete`, accepted offset, source size and final receipt. Unknown/expired state is explicit. |
| `POST /lan/v1/uploads/chunk` | ID, exact byte offset and bounded base64 data; returns next accepted offset. Reuse 4 KiB chunks initially. |
| `POST /lan/v1/uploads/commit` | Validates complete size and source hash before publication; returns the same receipt for repeated commits. |
| `POST /lan/v1/uploads/cancel` | Cancels only that client's unfinished transaction. Never removes a completed document. |

A new random epoch on receiver start, storage change or reboot prevents a stale
client from admitting an old request after generation resets. Bind an ID to
client identity, epoch and the exact original metadata. Reuse with different
metadata is a conflict. Admission increments generation once; status, successful
start replays and commit replays do not. Another client receives busy rather than
entering an implicit upload queue. Limits and failure codes must be machine
readable, with human text optional.

Keep one active transaction and four completion receipts initially, matching the
current bounded memory model. A receipt includes the final chosen filename,
source hash, byte size and document ID. Return a code such as `unknown_upload` or
`epoch_changed` when recovery cannot be proved. A client must not reinterpret that
as permission to silently create another transaction. An explicit user retry can
choose a new request; uncertainty about a previous import must remain visible.

For a lost start response, retry the original ID and metadata. For a lost chunk
response, query its accepted offset; resume at that offset and never assume that
a timed-out POST was not applied. For a lost commit response, retry the same ID
or query its completion receipt. Limit retries with backoff and jitter; stop on
authentication, metadata conflict, size and storage failures. These application
semantics are what make retries safe: HTTP does not make arbitrary POSTs
idempotent. [RFC 9110, section 9.2.2](https://www.rfc-editor.org/rfc/rfc9110.html#section-9.2.2)
requires that automatic retries account for the operation's semantics.

In N, a brief TCP/Wi-Fi interruption can leave a transaction alive until its
progress lease expires, unlike today's AP station-disconnect policy. Unplugging
SD, leaving N, shutdown or the lease deadline aborts it. Status probes alone do
not extend the lease. Stored bytes and final results are still session scoped:
v1 does not promise reboot resume, durable receipts or exactly-once delivery
across power loss. FAT rename avoids publishing an incomplete upload during
normal operation; it is not a power-loss durability guarantee. Persistent
receipts/journaling and recovery checkpoints require a separate wear/corruption
review before such guarantees can be advertised.

## Documents, formulas and messages

Keep the original UTF-8 `.md`/`.markdown` file as the authoritative content, whether
it arrives over A, N or directly on SD. Store bookmarks as source byte offsets
with validated source identity. Use an independent document/layout identity so
Markdown changes do not invalidate existing TXT/EPUB progress. Apply the same
size, UTF-8, name and nonoverwrite rules through every importer.

The device should parse supported Markdown blocks/inlines and lay out supported
mathematics locally using boxes with width, ascent and descent. Fractions, roots,
scripts, operators, Greek letters and basic matrices must not depend on a LAN
pre-rendering service. Unsupported constructs need a readable source fallback,
not silent deletion. Treat HTML, code fences, links and formulas as document
data: never execute code, fetch external images/fonts or resolve network includes.

A later optional `DocBundle` may carry prepared math assets to improve quality.
It must preserve the full original source and source spans. Its versioned,
bounded manifest needs the source hash, formula source hash, source range,
font/render-profile hash and baseline/ascent/descent. Assets are caches: a
mismatch discards them and falls back to local source rendering. Validate every
asset's type, byte count and dimensions before allocation; accept only importer-
assigned paths, not arbitrary archive extraction paths. Bundles are deferred
until plain Markdown and local formulas pass offline acceptance.

For a later message inbox, propose `kind=note`, at most 16 KiB UTF-8 per note and
eight retained notes in a separately owned directory. Enforce an aggregate quota
and return inbox-full instead of silently deleting unread content. Reuse the
same transaction, not a separate unbounded message queue. Notes do not become
books or change the selected book until an explicit user action. TTL, deletion,
read acknowledgements and the two-button inbox UI need decisions before this
kind is advertised in capabilities.

## Interaction and power policy

| Mechanism | Behavior | Recommendation |
| --- | --- | --- |
| Explicit foreground send | Local CLI contacts the active N receiver, sends one item and closes. Radio is needed only during the chosen receive window. | First milestone. No desktop service required. |
| On-demand pull | Device contacts a user-selected local companion only when explicitly requested. | Optional if users prefer a local outbox; adds companion configuration and outbound authentication. |
| Periodic polling | Device repeatedly checks a local companion; interval trades latency for radio wake time. | Deferred; no default timer or background cloud endpoint. |
| Persistent SSE/WebSocket | Low message latency while a session and Wi-Fi remain alive; requires reconnect, heartbeats and bounded backpressure. | Deferred until a concrete continuous-stream use case exists. USB alone does not enable it. |

Proposed first-version lifetime: on battery, N has a five-minute inactivity
window; admitted transfer progress extends it, discovery/status traffic does
not. While externally powered, an explicitly selected N may remain available
until the user leaves it. Detecting USB removal starts a fresh bounded battery
window. A paused transfer still has its shorter progress lease, so a disconnected
sender cannot prevent idle shutdown indefinitely. Use the existing stable power
source detection rather than USB CDC enumeration alone.

Do not redraw the e-paper or reset UI idle timers on network polling. Refresh
only a changed connection/transfer/inbox indicator, use the existing footer
convention and preserve input release guards. Network and storage work remains
bounded so shutdown, cancel and physical keys continue to be serviced. Rendering
waits until ingestion releases storage ownership; accepting a remote document
must not enqueue repeated navigation actions.

Manual light/deep sleep does not maintain Wi-Fi connections. Maintaining Wi-Fi
through sleep requires modem sleep and supported automatic light sleep, according
to [ESP-IDF sleep modes](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32s3/api-reference/system/sleep_modes.html).
The pinned ESP32-S3 Arduino SDK is ESP-IDF 4.4.7; its checked `dio_opi` configuration
does not enable `CONFIG_PM_ENABLE` or `CONFIG_FREERTOS_USE_TICKLESS_IDLE`. Adding a
runtime `esp_pm_configure()` call cannot enable absent compiled support. See
[ESP-IDF power management](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32s3/api-reference/system/power_management.html)
for these build requirements.

Keep current manual sleep gating while any service is active. Investigate modem
sleep and an SDK rebuild with automatic light sleep only as a separately measured
change. Radio-off offline mode remains the battery baseline. A radio-off reader
cannot accept immediate LAN pushes; a companion may keep its own bounded outbox,
but must report waiting for the next explicit receive window. No actual current
savings or standby lifetime are claimed without electrical measurements.

## Privacy and acceptance gates

A local network is not an authorization boundary: neighboring clients can send
packets, spoof discovery or stall connections. TLS plus pinned identity protects
content in transit; client tokens authorize operations. No UPnP port mapping,
public listener, telemetry, internet reachability check, external DNS dependency
or automatic update service is part of this proposal. Refuse redirects to other
hosts. A device may still be reachable across routed LAN segments; authentication
and the explicit receiver lifetime remain necessary.

Physical flash/SD access is a separate threat. Existing home Wi-Fi credentials
are stored in unencrypted NVS; adding a token verifier does not encrypt books or
protect the device TLS private key at rest. Flash encryption/secure boot would
need explicit provisioning and recovery design. Do not promise those protections
from this networking work. Logs contain result/error codes and byte counts, not
tokens, passwords, document text, book titles or private host paths. Tests use
synthetic files and temporary credentials only; hardware tests must preserve
already saved real credentials and reading state.

Before implementing P3, review the pairing/revocation interaction, N lifetime,
TLS adapter resource budget and the proposed protocol together. Before enabling
it in a release, require:

- AP protocol compatibility and N-only lifecycle tests; Off/navigation/shutdown
  leave no listener, mDNS advertisement, transfer or active radio.
- Spoofed discovery/wrong pin/revoked token/other-client ID tests; no downgrade,
  credential echo or unauthenticated metadata endpoint.
- Lost start/chunk/commit responses, duplicate replay, receipt eviction, epoch
  changes, SD removal, failed flush/readback/rename, full storage and library-full
  tests using the production transaction implementation.
- Slow sender/receiver and stalled TLS tests with bounded memory, concurrency,
  deadlines and shutdown/key responsiveness on the actual pinned SDK.
- Direct SD/AP/LAN parity for Markdown source bytes, bookmarks, formulas and
  unsupported-source fallback; no network dependency for opening received files.
- Real transfer hash verification and idle/wake tests on USB and battery;
  electrical measurements before publishing battery-life claims.

The AP browser regression is executable separately with Node.js 18+ using
`node tests/web_ui.js`. It extracts the production inline script and tests normal
upload plus five ambiguous/failing network outcomes. It does not replace native
handler tests, actual browser/QR checks or transport timing measurements.
