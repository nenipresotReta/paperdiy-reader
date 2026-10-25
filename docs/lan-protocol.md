# LAN sender and protocol v2

[English](lan-protocol.md) | [简体中文](lan-protocol.zh-CN.md)

This is the client/device integration contract. The foreground Python sender,
firmware TLS receiver and bounded persistent store are implemented. Host tests
cover protocol behavior, storage faults and ownership gates; the [architecture
report](architecture-and-implementation.md) describes implementation
boundaries. Protocol tests do not establish physical delivery or wireless
performance.

Run the sender tests with `python3 tests/lan_client.py`. They use a simulated
reader, generated certificates and listeners bound only to `127.0.0.1`. With
`PAPERDIY_LAN_WIRE` pointing at the `lan_service_wire` build target (CMake sets
it), the same sender also drives the real firmware handlers with its exact
request bytes.

## Model

```
Sender                              Reader (Settings + N)
discover / resolve        ───────▶  mDNS responder
pinned TLS (SAS on first contact)
offer batch (1–8 entries) ───────▶  precheck each entry
                                    prompt "Hermes · Receive 3 files?"
                          ◀───────  short A+B → batch accepted
upload + commit per entry ───────▶  one active transaction at a time
                          ◀───────  receipt per entry → "Done · 3 files"
```

- **Reception is Settings-bound.** The receiver, its routes and its mDNS
  advertisement exist only while the reader shows Settings with N active.
- **Every batch needs consent on the reader.** One short A+B (released within
  2 s) accepts the whole batch. A keeps its meaning (next mode), so it switches
  N off, which cancels; B rotates and redraws; leaving Settings cancels; 60 s
  without action expires the prompt; holding A+B for 2 s shuts down and never
  accepts. Nothing reaches the card without acceptance.
- **First delivery pairs.** An unknown sender's first batch carries a
  commitment-protected short authentication string (SAS); one A+B both pairs
  and accepts. A standalone pairing is the same request with no entries.

The CLI does not install a daemon, enable a network, change host networking,
scan subnets or use a relay. `queued` means the sender retained the content;
`delivered` means a matching commit receipt was verified. Neither means the
reader displayed or the user read the content.

## TLS identity

Each reader generates its own key and self-signed certificate. `F` is the
SHA-256 of the certificate DER; the **device ID is the first 16 hex digits of
`F`**, so an advertised ID is bound to the certificate that presents it. The
firmware serves TLS 1.2; the sender requires TLS 1.2 or newer.

The sender compares the live certificate with the pin **before writing any
HTTP byte**. The only request ever sent unpinned is `GET /lan/v2/hello`, which
carries no credential and no body; it identifies the reader for first contact
and `--host`. After pairing, the pin is the certificate observed during the
accepted first contact. There is no accept-any-certificate, HTTP, redirect or
alternate-pin fallback. An address can change without changing the pin.

Authenticated requests carry `Authorization: Bearer <clientId>:<token>`. AP
cookies, AP tokens and Wi-Fi passwords grant no LAN authority. All receipt and
upload lookups are client scoped. At most four senders are paired.

## First contact

```
GET  /lan/v2/hello                                    (unpinned, no credentials)
→ { v:2, deviceId, pushRevision, batchEntries:8 }     sender checks deviceId == F[0:16]

POST /lan/v2/batch/offer   label, commit = hex SHA-256(Ns), e0..e7 (0–8 entries)
→ { batchId, state:"awaiting_reveal", pairingId, deviceNonce = Nd, deviceId, entries }

POST /lan/v2/pair/reveal   pairingId, senderNonce = Ns
→ { batchId, state:"awaiting_acceptance" }            the reader shows the SAS; the response never does

GET  /lan/v2/batch/status?batchId=…&proof=HMAC-SHA256(Ns, "paperdiy-batch-status-v2" ‖ batchId)
→ awaiting_acceptance | expired | accepted + deviceId + clientId + token + pushRevision
```

```
H   = SHA-256( "paperdiy-pair-v2\0" ‖ F ‖ Ns ‖ Nd ‖ pairingId ‖ len16(label) ‖ label )
SAS = big-endian uint32(H[0:4]) mod 1 000 000, shown as "482 913"
```

`F`, `Ns` and `Nd` are raw 32-byte values; `pairingId` is its 16 ASCII hex
digits; `len16` is the big-endian UTF-8 byte length. The sender computes `F`
from the certificate it observed, the reader from its own. A relay with its own
certificate therefore produces a different number on each side; committing to
`Ns` before learning `Nd` prevents grinding (about 10⁻⁶ per attempt).

- Offer, reveal and status are pinned to the `F` observed by `hello`. The
  token is never requested from a connection whose certificate differs.
- The reader admits a client only on A+B. Status returns the token until the
  sender's first bearer request, then forgets it.
- Labels are 1–48 UTF-8 bytes without C0/C1 controls. A paired sender is shown
  by its registry label; a self-asserted label appears only with
  `New sender`.
- A failed reveal ends the attempt. Unauthenticated offers, hello requests and
  failed reveals/proofs count toward the rate limit (8 per 10 s).

Shared test vectors (`tests/lan_batch.cpp`, `tests/lan_client.py`):
`F = 0x11×32`, `Ns = 0x22×32`, `Nd = 0x33×32`, pairing ID
`0123456789abcdef`, label `Hermes 書` → SAS `506 740`; proof for batch
`fedcba9876543210` →
`4b19bde7b8dd1e27aab707265889ba8c7a1b22e0bba59770d2f6dc3ebe6533e4`; commit →
`9f72ea0cf49536e3c66c787f705186df9a4378083753ae9536d65b3ad7fcddc4`.

## Batches

```
POST /lan/v2/batch/offer   (Bearer) e0..e7
→ { batchId, state:"awaiting_acceptance", entries:[{requestId, admission}] }
→ { state:"blocked", entries:[…] }                    nothing admissible: no prompt, no state

GET  /lan/v2/batch/status?batchId=…   (Bearer)
→ { batchId, state: awaiting_acceptance | accepted | expired, entries }
```

- 1–8 entries (0 only for a first-contact pairing), contiguous `e0`…, at most
  one Push admitted per batch. Unknown or duplicate fields are refused.
- Each `eN` is eight fields, each `<UTF-8 byte length>:<bytes>`, in order:
  `requestId fingerprint kind name title size sha256 baseRevision`. Lengths are
  canonical decimals of at most three digits with per-field bounds
  (32, 64, 4, 96, 96, 10, 64, 10); `baseRevision` is empty for files. The parser
  runs before authentication on first contact; it is bounded and fuzzed.
- Admission codes: `ok`, `invalid_metadata`, `size_limit`,
  `unsupported_format`, `already_received`, `duplicate_push`,
  `revision_conflict`, `library_full`, `no_space`. The precheck is advisory;
  upload-time and commit-time checks stay authoritative.
- One pending batch per reader (`409 batch_busy`) until every admitted entry
  has a receipt. A sender may replace its own accepted batch that it stopped
  uploading; the replacement needs a new A+B.
- After acceptance, `uploads/start` is admitted only for an entry of the
  accepted batch (otherwise `409 not_accepted`); a replay of a finished
  request returns its receipt. The accepted set lives in RAM and ends when N
  stops. The batch is not atomic: each entry has its own upload, commit and
  receipt.

## Upload operations

POST bodies are `application/x-www-form-urlencoded`, at most 16 fields and
17000 encoded bytes on both participants; this covers the worst-case percent
encoding of a 4096-byte base64 chunk and of eight worst-case entries. Responses
are bounded JSON with Content-Length. The sender rejects chunked responses,
duplicate headers or JSON keys, redirects, headers over 8192 bytes and JSON
over 16384 bytes. Errors use a stable snake_case `code`.

| Operation | Request and result |
| --- | --- |
| `GET /lan/v1/capabilities` | `v:2`, `deviceId`, `pushRevision`, 32-hex `epoch`, `receiptCapacity:16`, `receiptScope:"client"`, `batchEntries:8`, `limits`. |
| `GET /lan/v1/push` | Current Push metadata; no body. |
| `POST /lan/v1/uploads/start` | `requestId`, `fingerprint`, `kind`, `name`, `title`, `size`, `sha256`, `baseRevision`. Identical replay returns the same transaction or receipt. |
| `POST /lan/v1/uploads/chunk` | `requestId`, exact accepted `offset`, `data` (base64, ≤ 4096 bytes). No blind append after a lost response. |
| `GET /lan/v1/uploads/status?requestId=…` | Receiving/checking state and accepted offset, or a completed receipt. |
| `POST /lan/v1/uploads/commit` | Validates and publishes; may return `checking` while the reader hashes. Replays return the same receipt. |
| `POST /lan/v1/uploads/cancel` | Cancels only this client's unfinished transaction. |
| `GET /lan/v1/receipts?requestId=…` | Completed receipt, or unknown. Unknown never proves the request was not applied. |
| `POST /lan/v1/clients/revoke` | Bearer self-revocation with the caller's own `clientId`. |

`limits` keys are `chunkBytes`, `pushBytes`, `txtBytes`, `epubBytes` and
`markdownBytes`: Push 16384 source bytes, TXT 4 MiB, EPUB 64 MiB, Markdown
1 MiB. Name/title are at most 96 UTF-8 bytes without C0/C1 controls; names
cannot contain path separators, Windows-invalid characters, a leading dot or a
trailing dot/space. Push uses name `Push.md`. Files never overwrite a book.

Receiving/checking replies contain `state`, `requestId`, `fingerprint`, `size`
and `offset`; `checking` requires `offset == size`. A completed reply has
`state:"complete"` and `receipt` with `requestId`, `fingerprint`, `kind`,
`size`, `sha256` and `revision` (Push) or final `name` (file). The sender
verifies every identity/content field before reporting delivery.

Removed in v2: `POST /lan/v1/pair`, the physical-secret `clients` and
`clients/revoke` forms, and the pairing QR. Existing v1 clients stay valid.

## Request identity and concurrency

`requestId` is 128 random bits as 32 lowercase hex digits; the durable lookup
key is `(clientId, requestId)`. The request fingerprint is SHA-256 over
`kind, name, title, size, sha256, baseRevision`, each as a four-byte
big-endian UTF-8 length followed by the bytes; `baseRevision` is empty for
files. Test vector: `push`, `Push.md`, `Synthetic`, `3`, 64 zeros, `0` →
`7c5dc380b37e359158a305fc6aff88f3397a4c8a7d1b8a2d00c89c3fd0016d74`.

Push is one replaceable book. Its base revision is the reader's last known
revision when queued, or, for a first Push to a reader not yet known, the
revision the reader reports when the batch is offered. Queued content is never
rebased onto a newer revision; a conflict is `blocked` with
`revision_conflict`. Receipt replay precedes a fresh revision check, so a
commit with a lost response stays recoverable after later revisions. An
identical Push body is a receipt-backed no-op.

The reader keeps 16 shared receipt slots (client-scoped lookups) plus the two
Push commit records. Two full batches fill the ring, so the sender resolves
every `uncertain` entry for a reader before offering it anything new.
Publication recovery after power loss verifies the intent, the chosen final
file and its complete hash; existing files are never deleted or overwritten.

## Discovery

While N is active the reader advertises `paperdiy-<deviceId>.local` and
`_paperdiy._tcp` on port 8443 with TXT `id=<deviceId>`. The sender sends RFC
6762 one-shot queries (QU bit, ephemeral port; two 1-second attempts) to
`224.0.0.251:5353` and accepts IPv4 A records with an exact owner name; AAAA is
ignored. It needs no Bonjour, nss-mdns or Windows mDNS, and fails cleanly where
multicast does not reach the LAN (for example a NAT'd container); use `--host`
there, and give the reader a DHCP reservation.

- **Paired:** stored address with the pin; on a network failure or a pin
  mismatch there, resolve `paperdiy-<deviceId>.local` and try each IPv4 result
  with the pin. The address is saved only after pinned success. Other readers
  never make a paired sender ambiguous.
- **Unpaired:** one service query. One reader → first contact; several →
  `multiple_devices` with `{deviceId, host}` candidates (never a guess); none →
  `device_not_found`. `--host` skips discovery.

mDNS supplies candidate addresses only; identity is always the certificate.

## Sender

Python 3.9+ standard library on macOS, Linux (including containers) and
Windows. One foreground command per run; no daemon.

```sh
python3 tools/paperdiy_lan.py push brief.md --title "Brief" --label Hermes --json --wait 5
python3 tools/paperdiy_lan.py send a.epub b.md --json --wait 5
python3 tools/paperdiy_lan.py sync --batch-id 0123456789abcdef --json --wait 300
python3 tools/paperdiy_lan.py pair --label Hermes
python3 tools/paperdiy_lan.py status
```

| Command | Purpose |
| --- | --- |
| `push SOURCE [--title T]` | Queue the Push replacement, then offer. |
| `send SOURCE…` | Queue ordinary files, then offer. |
| `sync [--batch-id ID] [--reoffer]` | Continue a batch, resolve uncertain entries, offer what is queued. `--reoffer` includes entries that were `not_accepted`. |
| `pair --label NAME [--host IP]` | First contact with no entries (also re-pairs a reader). |
| `status` | Local readers, pending first contacts and entries. |
| `endpoint --host IP [--device D]` | Verify the pinned reader at an address and save it. |
| `revoke [--device D]` | Revoke this sender on the reader, then forget it locally. |
| `cancel` / `abandon --request-id ID` | Cancel an unfinished transfer / release a local entry (a possible publication is not undone). |
| `prune --delivered` | Remove delivered archives. |

Common options: `--device D` or `--host IP`; `--label NAME` (first contact);
`--wait N` (default 120) bounds discovery and waiting for acceptance; `--budget
N` (default 600) bounds the whole run including uploads; `--wait ≤ --budget ≤
600`; `--wait 0` only queues; `--json`.

- With one paired reader, `--device` is optional; several → `ambiguous_device`;
  none → first contact (needs `--label`).
- Batches take queued entries in order, up to 8 and at most one Push; a second
  Push waits. `blocked` and `not_accepted` entries are excluded until the user
  decides, and no entry is offered twice in one run.
- First contact returns early with `awaiting_pairing_confirmation` and `sas`
  under `--json` (the agent must relay the number); human output prints the
  number and keeps waiting.

### Entry states

`commitSent` is persisted immediately before `uploads/commit`.

| Situation | State | Code | Exit |
| --- | --- | --- | --- |
| Reader not found / unreachable | `queued` | `device_not_found` / `device_unreachable` | 0 |
| Waiting for A+B when the wait ends | `queued` | `awaiting_acceptance` / `awaiting_pairing_confirmation` | 0 |
| Not accepted (cancelled, expired, connection lost) | `queued` | `not_accepted` | 0 |
| Interrupted before `commit` | `queued` | `transfer_interrupted` / `budget_exhausted` | 0 |
| Commit sent, no conclusive answer | `uncertain` | `commit_unconfirmed` / `receipt_unknown` | 2 |
| Matching receipt | `delivered` | — | 0 |
| Precheck or upload-time refusal | `blocked` | specific | 2 |

Other states: `cancelled`, `abandoned`, `superseded`. Local and usage errors
are `{"state":"error","code":…}` with exit 2: `label_required`,
`no_paired_device`, `ambiguous_device`, `unknown_device`, `multiple_devices`,
`unauthorized`, `pin_mismatch`, `source_unreadable`, `invalid_utf8`,
`size_limit`, `state_dir_unavailable`, `state_dir_permissions`, `lock_held`,
`store_corrupt`.

JSON output is ASCII (`ensure_ascii`), carries `state` (`done`, `pending` or
`error`), `next`, `summary`, per-entry `items`, and `batchId`/`sas` when
relevant. It never contains tokens, nonces, document text or local paths.
Human output is UTF-8 and degrades unencodable characters on other consoles.

### Storage

| | POSIX (macOS, Linux) | Windows |
| --- | --- | --- |
| Root | `$XDG_DATA_HOME/paperdiy` or `~/.local/share/paperdiy` | `%LOCALAPPDATA%\paperdiy` |
| Lock | `fcntl.flock`, non-blocking | `msvcrt.locking`, non-blocking |
| Privacy | owner, mode `0700`/`0600`, no symlinks | profile ACL; symlinks and junctions refused (weaker) |
| Replace | `os.replace` + file and directory `fsync` | `os.replace` + file `fsync` |

The manifest holds readers (pin, address, client ID, token), entries and
pending first contacts (`Ns` until a terminal state, never reused). Source
bytes are archived, so later edits do not change a queued entry. Quotas: 16
files / 128 MiB outbox, 256 objects / 512 MiB archive. A v1 manifest is
migrated in place; v1 entries without a conclusive outcome become `uncertain`
and are resolved through receipts first.

Sender removal from the reader's side is in the AP-mode web page (Senders:
list, remove, forget all); see [usage](usage.md).
