---
name: paperdiy-delivery
description: Send a Markdown Push update or an ebook to a PaperDIY reader through its paired LAN interface, or inspect and synchronize the sender's delivery queue. Use for delivery to this reader, not for general networking or firmware updates.
---

Use the PaperDIY repository's `tools/paperdiy_lan.py` CLI. Locate the user's
checkout before invoking it; Python 3.9+ on macOS/Linux is required. Run its
`--help` for command syntax. The maintained protocol is
`docs/lan-protocol.md` in that checkout. Do not replace the client with curl,
an unpinned TLS connection, or an agent-managed retry loop.

The user's request to deliver specific content authorizes that delivery.
Choose the device they identify; clarify only if multiple saved devices make
the destination ambiguous. Pairing needs the physical reader's JSON QR payload
and the device in N mode. Read the payload from a private file or stdin; never
put pairing secrets or bearer tokens in command arguments, chat, or logs.

- `push` replaces the one Push book. Use UTF-8 Markdown, including supported
  `$...$` or `$$...$$` math, within 16 KiB of source. If it is larger, shorten it
  with the user's intent intact or send an ordinary Markdown file. Do not cut
  a formula or silently truncate content.
- `send` imports an EPUB, TXT or Markdown file without replacing an existing
  book. It does not open the book or move the current reading position.
- `sync` makes a finite foreground attempt on existing queued requests. N must
  be enabled on the reader for reception. The sender has no background daemon.

The CLI archives source content and owns bounded queueing, pinned device
identity, chunk retries, concurrency revisions and commit receipts. Keep the
same request ID when resolving an interrupted transfer. Never automatically
rebase an old Push onto a newer revision, abandon an uncertain request, prune
archives, revoke a client, or change its certificate pin to make a retry pass.
Those operations need the user's intent for that change.

Report the returned state accurately: `queued` is retained on the sender;
`delivered` has a matching device commit receipt; `uncertain` has no conclusive
outcome; `blocked` needs the stated conflict resolved. Delivery does not prove
that the screen refreshed or the user read it. Do not claim success from an
upload's byte count alone.

Use the existing host network. Never toggle or join Wi-Fi, change IP, DHCP,
DNS, routes, gateway, service order or preferred networks, scan a subnet,
stop other services, or install a relay to reach the reader. If it is offline,
retain the queued result and explain how N enables its receiving session.
Generated content, credentials, pairing payloads, outbox and archive are local
private data and must stay outside public source commits.
