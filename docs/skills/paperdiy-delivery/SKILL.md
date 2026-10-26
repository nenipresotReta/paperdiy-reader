---
name: paperdiy-delivery
description: Send a Markdown Push update or ebooks to a PaperDIY reader over the home network, with the user accepting each batch on the reader, or update Push live on a reader left on P. Use for delivery to this reader, not for general networking or firmware updates.
---

Use the PaperDIY repository's `tools/paperdiy_lan.py` (Python 3.9+, standard
library only; macOS, Linux including containers, Windows). Run it as
`python3 tools/paperdiy_lan.py …` (`py` on Windows). The protocol is
`docs/lan-protocol.md` in that checkout. Never replace it with curl or another
client.

The reader receives only while it is on **Settings with N active**, and nothing
is written until the user **short-presses A+B** on the reader to accept the
batch. Your job is to send, tell the user exactly what to do on the reader,
and report receipts.

## Happy path

1. Send with a short wait. Add `--label NAME` (who you are, e.g. `Hermes`) on
   the first contact with a reader; it is ignored once paired.

   ```
   python3 tools/paperdiy_lan.py push notes.md --title "Weekly" --json --wait 5
   python3 tools/paperdiy_lan.py send a.epub b.md --json --wait 5
   ```

2. Act on `next` in the JSON result:

   | `next` | Say to the user | Then run |
   |---|---|---|
   | `device_not_found`, `device_unreachable` | "On the reader: Settings → press A until N." | the same command again, or `sync --json --wait 300` |
   | `multiple_devices` | "I see more than one reader. Which IP does yours show on its N screen?" | retry with `--host IP` |
   | `awaiting_acceptance` | "On the reader, short-press A+B to accept the files from LABEL, and stay until it shows Done." | `sync --batch-id ID --json --wait 300` |
   | `awaiting_pairing_confirmation` | "The reader should show *New sender LABEL · SAS* and the files. If the number matches, short-press A+B and stay until Done. If it doesn't, switch off N." | `sync --batch-id ID --json --wait 300` |
   | `batch_busy` | "The reader is accepting another sender's files; try again when it shows Done." | `sync --json --wait 300` |

   Relay `sas` exactly as returned (e.g. `506 740`).

3. Report from receipts with `summary` and `items`:

   | Result | Say |
   |---|---|
   | every entry `delivered` | "Delivered N files — the reader confirmed each. It should show Done; you can leave Settings." |
   | some `delivered` | "Delivered K of N; "name" is CODE." |
   | `queued` + `not_accepted` | "It wasn't accepted on the reader (cancelled, timed out, or the connection dropped). Do you want me to offer it again?" Only if yes: `sync --reoffer --json --wait 300`. |
   | `queued` + `transfer_interrupted` / `budget_exhausted` | "The transfer stopped before completion; nothing was published. Return to N and I'll offer the rest again." → `sync` |
   | `uncertain` | "The reader may or may not have published "name". Return to N and I'll check its receipt." → `sync` |
   | `blocked` + code | The specific reason and the decision needed (below). |

   When more than 8 entries are queued, they go in batches of 8; each batch
   needs its own A+B. Say "This was batch 1 of 2; accept it and I'll send the
   next."

## Live Push (reader on P)

When the user has left the reader on **P** (for example plugged in as a status
board), a sender that was paired earlier in N can replace Push with no A+B:

```
python3 tools/paperdiy_lan.py push board.md --title "Today" --live --position keep --json --wait 5
```

- `--position top` (default) shows page 1 of the new Push on a reader that has
  Push open. `keep` stays on the same text when it still exists. Use `keep` for
  a long document that is being edited, `top` for a board or notice.
- Exit code 0 means `delivered`; anything else is 2.
- `next`/`code` `live_unavailable`: "The reader isn't on P. On the reader:
  Settings → press A until P." The entry stays queued, and the next live push
  replaces it. Do not switch to N to force it through unless the user asks.
- `no_paired_device`: pair first in N (`pair --label NAME`), with the user
  confirming the SAS on the reader.
- `live_mode` from `send`, `pair` or a first contact: P takes only live Push.
  Files and pairing need N.
- `blocked` + `revision_conflict` after a live push means another sender won
  twice in a row; ask before pushing again.
- Push as often as the content changes, at most about once a minute. The
  reader redraws the whole screen on every new body, and an identical body
  is a no-op.

## Content

- `push` replaces the one Push book: UTF-8 Markdown (with `$…$`/`$$…$$` math)
  up to 16 KiB. If larger, shorten it with the user's intent intact or `send`
  it as an ordinary Markdown file; never cut a formula or truncate silently.
- `send` imports EPUB, TXT or Markdown without replacing other books or moving
  the reading position.
- `blocked` codes: `revision_conflict` (the Push changed on the reader since
  this one was prepared — ask before pushing again), `size_limit`,
  `library_full`, `no_space`, `unsupported_format`.
- `{"state":"error","code":…}` is a local or usage error, not a delivery
  state: `label_required`, `source_unreadable`, `invalid_utf8`, `size_limit`,
  `lock_held` (another run is active), `state_dir_permissions`,
  `store_corrupt`, `ambiguous_device`, `unauthorized` (this sender was removed
  on the reader; ask before pairing again).

## Readers and senders

- With one paired reader nothing more is needed. With several, add
  `--device DEVICE_ID` (from `status`); `ambiguous_device` means you must ask
  which one.
- `pair --label NAME --json` pairs without sending anything (same SAS flow).
- Removing senders is done by the user in the reader's AP-mode web page
  (Senders → Remove / Forget all senders). `revoke` removes only this sender.

## Never

- Use unpinned TLS after pairing, put tokens or secrets in arguments, chat or
  logs, scan a subnet, or change the host's network (Wi-Fi, IP, DHCP, DNS,
  routes).
- Automatically re-offer, rebase a Push in N, `abandon`, `prune`, `revoke` or
  `pair` again to make a retry pass — each needs the user's intent. (`--live`
  rebasing onto the reader's current Push is the documented P behavior.)
- Claim success without a `delivered` receipt, claim what the screen shows,
  or call `not_accepted` a rejection (switching off N and a dropped
  connection look the same to the sender).
