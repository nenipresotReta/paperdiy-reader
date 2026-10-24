# LAN sender and protocol v1

[English](lan-protocol.md) | [简体中文](lan-protocol.zh-CN.md)

This is the client/device integration contract. The foreground Python sender,
firmware TLS receiver and bounded persistent store are implemented. Host tests
cover protocol behavior, storage faults and ownership gates. Current physical
acceptance and unresolved hardware work are recorded in the
[architecture report](architecture-and-implementation.md).
Run synthetic client tests with `python3 tests/lan_client.py`.
They use generated test certificates and listeners bound only to `127.0.0.1`
on ephemeral ports. They do not exercise a physical reader or Wi-Fi delivery.

The receiver exists only during an explicitly selected N session. The CLI does
not install a daemon, enable a network, change host networking, scan subnets or
use a relay. Both participants must be online together. `queued` means the
sender retained the content; `delivered` means a matching commit receipt was
verified. Neither means the reader displayed or the user read the content.

## Physical pairing and TLS

The physical pairing payload is bounded JSON (4096 bytes maximum):

```json
{"v":1,"deviceId":"example-device","host":"192.0.2.1","port":8443,"certSha256":"64 lowercase hexadecimal characters","secret":"32 lowercase hexadecimal characters"}
```

The host is a literal IPv4/IPv6 address. Discovery is not implemented. The
128-bit secret is single use and valid only in a physically enabled, bounded
pairing window. Device keys/certificates are generated per device; public
firmware must not contain a shared private key. Pairing returns a client ID
(16 lowercase hexadecimal characters), a 256-bit token (64 lowercase
hexadecimal characters), device ID and current `pushRevision`. Client labels are
nonempty and at most 48 UTF-8 bytes without C0/C1 controls. At most four
clients are admitted. The device retains token verifiers and supports revoking
individual clients.

The firmware serves TLS 1.2; the sender requires TLS 1.2 or newer.
The sender hashes the exact peer DER
certificate with SHA-256 and compares it to the physical payload **before
sending any HTTP bytes, secret, token or document**. This exact certificate is
the trust anchor; public CA/hostname/clock checks are not used for this local
identity. Certificate renewal requires explicit physical re-pairing. There is
no accept-any-certificate, HTTP, redirect, proxy or alternate-pin fallback.
An endpoint address can change without changing the pinned identity.

Authenticated requests carry `Authorization: Bearer <clientId>:<token>`.
AP cookies, AP tokens and Wi-Fi passwords grant no LAN authority. All receipt
and upload lookups are client scoped. Pairing input is read from a file or
stdin, never a secret command-line argument. A lost pair response or unsuccessful
credential persistence/readback is `pair_uncertain`; the client does not consume
another slot automatically. The compatible server code `pairing_not_saved`
cannot prove that NVS did not save the client before the failure.

For an orphaned pairing, explicitly enable a fresh N session and obtain its
current physical payload. `clients --payload pairing.json` lists up to four
client IDs and labels over pinned TLS without needing a local pairing or
manifest. Listing does not consume or extend the five-minute secret window.
Inspect the result, then use `revoke-client --payload pairing.json --client-id …`
to revoke a chosen identity. Revocation consumes that physical secret; obtain a
fresh payload before pairing again. Neither command automatically revokes or
retries another pairing. Inventory never returns a token or verifier. If the
credential record itself cannot be read and validated, the receiver stays
unavailable; inventory does not repair corrupt NVS or rotate the device identity.

## HTTP operations

POST bodies are `application/x-www-form-urlencoded`, bounded to 17000 encoded
bytes on both participants; this accommodates the worst-case percent encoding
of a 4096-byte base64 chunk. Responses are bounded JSON
objects with Content-Length. The sender rejects chunked responses, duplicate
headers/JSON keys, redirects, headers exceeding 8192 bytes and JSON exceeding
16384 bytes. Errors use a stable snake_case `code`; optional `error` prose has
no protocol authority. Each client request has a total deadline, including
TLS, headers and body.

| Operation | Request and result |
| --- | --- |
| `POST /lan/v1/pair` | Unauthenticated over pinned TLS; `secret`, `label`. Returns `deviceId`, `clientId`, `token`, `pushRevision`. |
| `POST /lan/v1/clients` | Current physical `secret` required; an ordinary bearer alone is insufficient. Returns `deviceId` and `clients`, at most four `{clientId,label}` entries. Read-only; does not consume the secret. |
| `POST /lan/v1/clients/revoke` | Authenticated `clientId` for self-revocation, or `secret` plus explicit `clientId` for physical revocation. Success returns `revoked:true`. The CLI supports both forms. |
| `GET /lan/v1/capabilities` | Authenticated `v:1`, `deviceId`, current `pushRevision`, 32-hex `epoch`, `receiptCapacity:16`, `receiptScope:"client"`; optional `limits` object. |
| `GET /lan/v1/push` | Authenticated current Push metadata; no private body by default. |
| `POST /lan/v1/uploads/start` | `requestId`, `fingerprint`, `kind`, `name`, `title`, `size`, `sha256`, `baseRevision`. Identical replay returns the same transaction or receipt. |
| `POST /lan/v1/uploads/chunk` | `requestId`, exact accepted byte `offset`, `data` (base64, at most 4096 decoded bytes). No blind append after a lost response. |
| `GET /lan/v1/uploads/status?requestId=…` | Receiving/checking state and accepted offset, or a completed receipt. Available before the sender knows any separate upload handle. |
| `POST /lan/v1/uploads/commit` | `requestId`. Validates and publishes; may return `checking` while bounded device polling runs. Replays return the same completed receipt. |
| `POST /lan/v1/uploads/cancel` | `requestId`. Cancels only the client's unfinished transaction; cannot delete a published book or Push. |
| `GET /lan/v1/receipts?requestId=…` | Completed receipt or explicit unknown/expired result. Unknown is never proof that the request was not applied. |

`limits` keys are `chunkBytes`, `pushBytes`, `txtBytes`, `epubBytes` and
`markdownBytes`. The client honors lower limits and never expands its local
policy. Initial policy is Push 16384 source bytes, TXT 4 MiB, EPUB 64 MiB,
Markdown 1 MiB and one active transaction. Name/title are at most 96 UTF-8
bytes without C0/C1 control characters; names cannot contain path separators,
Windows-invalid filename characters, a leading dot or a trailing dot/space.
Push uses name `Push.md`; its stable library identity is independent of that
transport name. Ordinary files never overwrite an existing book.

Receiving/checking replies contain `state`, `requestId`, `fingerprint`, `size`
and `offset`. `checking` requires `offset == size`; this does not yet mean
delivery succeeded. A completed reply has `state:"complete"` and `receipt`,
or the receipt fields directly. A receipt contains `requestId`, `fingerprint`,
`kind`, `size`, `sha256` and `revision` for Push or final `name` for a file.
The sender verifies all identity/content fields before reporting delivery.

## Request identity and concurrency

`requestId` is 128 random bits represented as 32 lowercase hexadecimal
characters. The durable lookup key is `(clientId, requestId)`. Changing any
bound metadata with the same key is a conflict. The request fingerprint is
SHA-256 over these six fields in order:

```text
kind, name, title, size, sha256, baseRevision
```

Each field is encoded as a four-byte unsigned **big-endian UTF-8 byte length**
followed by those UTF-8 bytes. Kind is exactly `push`
or `file`; size is canonical unsigned ASCII decimal; content SHA-256 is
64 lowercase hexadecimal characters; Push base revision is canonical uint32
decimal and file base revision is the empty string. An empty title remains a
field. Request/client IDs are excluded because they form the lookup key.

Test vector: kind `push`, name `Push.md`, title `Synthetic`, size `3`, SHA-256
value consisting of 64 zero characters, base revision `0` gives fingerprint
`7c5dc380b37e359158a305fc6aff88f3397a4c8a7d1b8a2d00c89c3fd0016d74`.

Push is one replaceable book. The sender records its base revision when the
content is queued, using an explicitly supplied revision or the last
authenticated response. Sync never attaches a newer revision to old content.
Unknown baselines remain blocked until an explicit replacement is queued with
a known baseline. A revision conflict is visible. Receipt replay precedes a
fresh revision/CAS check, so a successful commit with a lost response remains
recoverable after later Push revisions. An identical validated Push body is a
receipt-backed no-op: it retains the current revision, source, title and reading
anchor, and emits no publication notification or screen refresh.

The device has 16 shared durable receipt slots, with client-scoped lookups;
this is not 16 slots per client. The two retained Push commit records can also
answer receipt lookups for their verified generations. Receipt eviction,
unreadable recovery evidence, epoch changes or unknown status can leave an
outcome uncertain. Across power loss, publication recovery must verify the
intent, chosen final file and its complete hash; an ordinary rename alone
does not establish durable exactly-once delivery. No unknown result permits
the sender to invent a new request ID and silently import another copy.

The device store uses two fixed Push source/commit slots, two fixed copies of
the current publication intent and 16 fixed receipt slots. Receiving uses one
fixed staging file. Hash checking and startup recovery read at most 8192 bytes
per application-loop poll; record verification/publication runs separately
from the final hash slice. Existing ordinary files are never deleted or
overwritten. After reboot, an intended existing final file is reconciled by
complete length/hash verification; a missing or mismatching target remains
uncertain instead of being imported again under another filename. Local FAT
ownership and its storage lease are required throughout; card changes revoke
the session. Readback/fault tests do not prove physical power-loss durability.

## Sender storage and commands

Python 3.9 or newer on macOS/Linux is required; transport and persistence use
the standard library. Tests additionally use OpenSSL to generate disposable
certificates. State defaults to `~/.local/share/paperdiy`, with a user-only
directory, credentials/manifest/body files, an exclusive sender lock, atomic
manifest replacement and archived source bytes. Original source edits do not
alter an already queued item. Public code/fixtures contain no real pairing
data, document body, endpoint or credential.

```sh
python3 tools/paperdiy_lan.py pair --device reader --payload pairing.json
python3 tools/paperdiy_lan.py push brief.md --device reader
python3 tools/paperdiy_lan.py send report.epub --device reader
python3 tools/paperdiy_lan.py sync --device reader --wait 60
python3 tools/paperdiy_lan.py status --device reader
```

`--queue-only` archives without attempting transfer. `--state-dir` is a global
option before the command. `--budget` bounds the complete foreground sync
(default 120 seconds, maximum 600); `--wait` optionally waits for an offline
receiver within that same budget (maximum 600). There is no scheduled retry.
Status reads local evidence; it does not imply the receiver is online.
The physical `clients` and `revoke-client` commands need only `--payload`, plus
the explicit `--client-id` for revocation; they do not create or load local state.

The outbox holds at most 16 ordinary files/128 MiB and one latest never-attempted
Push. Replacing that Push keeps its old source in the archive. An attempted,
receiving or uncertain Push must be resolved or explicitly abandoned before
another Push can replace its queue record. File quota exhaustion refuses new
work. Archive quota is 256 objects/512 MiB, including unresolved and orphaned
objects from interrupted local writes; no history is silently removed.

`prune --device reader --delivered` explicitly removes delivered sender
archives. `abandon --device reader --request-id …` releases an unresolved local
queue slot while retaining its archive; it does not undo or cancel a possible
device publication. `cancel --device reader --request-id …` explicitly cancels
an unfinished device transaction; a completed receipt still reports delivered,
and an ambiguous cancellation remains uncertain. `endpoint --device reader --host … --port 8443` changes
only the saved address. `revoke --device reader` revokes its client token over
pinned TLS. A lost revoke response is uncertain and retains local credentials
for explicit recovery.

Output is bounded JSON containing request IDs, byte counts and truthful states,
without document text, tokens, titles or private paths. The explicit physical
inventory additionally returns client IDs and their user-supplied labels.
`uncertain` and
`blocked` return exit code 2; offline `queued` is a successful local queue
operation. `delivered` always requires a verified receipt.
