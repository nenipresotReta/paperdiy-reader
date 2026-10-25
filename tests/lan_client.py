#!/usr/bin/env python3
"""Sender tests: simulated reader, real loopback TLS and UDP; no device needed.

The simulated reader models the firmware's LAN v2 rules (tests/lan_service.cpp
tests the real handlers). Shared vectors tie both sides to the same bytes.
"""
import base64
import contextlib
import hashlib
import hmac
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("paperdiy_lan", Path(__file__).resolve().parents[1] / "tools" / "paperdiy_lan.py")
lan = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(lan)
POSIX = os.name != "nt"


# ------------------------------------------------------------ simulated world

class Clock:
    def __init__(self):
        self.now = 1000.0

    def __call__(self):
        return self.now

    def sleep(self, seconds):
        self.now += max(seconds, 0.001)


def unpack(text):
    fields, at = [], 0
    for _ in range(8):
        match = re.match(r"(0|[1-9][0-9]{0,2}):", text[at:])
        assert match, text
        at += len(match[0])
        size, start = int(match[1]), at
        encoded = text[at:].encode("utf-8")[:size]
        value = encoded.decode("utf-8")
        at = start + len(value)
        fields.append(value)
    assert at == len(text)
    keys = ("requestId", "fingerprint", "kind", "name", "title", "size", "sha256", "baseRevision")
    entry = dict(zip(keys, fields))
    entry["size"] = int(entry["size"])
    entry["baseRevision"] = int(entry["baseRevision"]) if entry["kind"] == "push" else None
    return entry


class Reader:
    """One reader on Settings + N. Times come from the shared clock."""

    def __init__(self, clock, host="192.0.2.10", cert=b"synthetic reader certificate"):
        self.clock, self.host, self.cert = clock, host, cert
        self.fp = hashlib.sha256(cert).hexdigest()
        self.device_id = self.fp[:16]
        self.on, self.clients, self.revision = True, {}, 0
        self.receipts, self.active = {}, {}
        self.batch, self.accept_at, self.checking = None, None, 0
        self.capacity, self.library, self.calls = 4, 0, []
        self.files = []

    # ---- user at the device
    def prompt(self):
        return self.batch if self.batch and self.batch["phase"] == "awaiting_acceptance" else None

    def accept(self):
        batch = self.prompt()
        assert batch, "no prompt shown"
        if batch["first"]:
            client, token = os.urandom(8).hex(), os.urandom(32).hex()
            self.clients[client] = {"label": batch["label"], "token": token}
            batch.update(client=client, issued=client, token=token)
        batch["phase"] = "accepted"

    def switch_off(self):
        self.on, self.batch = False, None

    def switch_on(self):
        self.on = True

    # ---- protocol
    def authenticate(self, credentials):
        if not credentials:
            return None
        client, token = credentials
        record = self.clients.get(client)
        if not record or not hmac.compare_digest(record["token"], token):
            raise lan.RemoteFailure("unauthorized", 401)
        if self.batch and self.batch.get("issued") == client:
            self.batch["token"] = None  # first bearer use drops the RAM copy
        return client

    def tick(self):
        batch = self.batch
        if batch and batch["phase"] == "awaiting_acceptance":
            if self.accept_at is not None and self.clock() >= self.accept_at:
                self.accept_at = None
                self.accept()
            elif self.clock() - batch["prompted"] >= 60:
                batch["phase"] = "expired"
        if batch and batch["phase"] == "awaiting_reveal" and self.clock() - batch["started"] >= 60:
            self.batch = None

    def finished(self):
        batch = self.batch
        if not batch or batch["phase"] == "expired":
            return True
        if batch["phase"] != "accepted":
            return False
        return all((batch["client"], e["requestId"]) in self.receipts for e in batch["entries"] if e["admission"] == "ok")

    def handle(self, method, path, form, credentials):
        self.tick()
        route, _, query = path.partition("?")
        args = dict(form or {})
        args.update(dict(pair.split("=", 1) for pair in query.split("&") if pair))
        self.calls.append((method, route, credentials is not None))
        client = self.authenticate(credentials)
        if route == "/lan/v2/hello":
            return {"v": 2, "deviceId": self.device_id, "pushRevision": self.revision, "batchEntries": 8}
        if route == "/lan/v2/batch/offer":
            return self.offer(args, client)
        if route == "/lan/v2/pair/reveal":
            batch = self.batch
            if not batch or batch["phase"] != "awaiting_reveal" or args.get("pairingId") != batch["pairingId"]:
                raise lan.RemoteFailure("unknown_pairing", 404)
            nonce = bytes.fromhex(args["senderNonce"])
            if hashlib.sha256(nonce).hexdigest() != batch["commit"]:
                self.batch = None
                raise lan.RemoteFailure("pairing_rejected", 409)
            batch.update(nonce=nonce, phase="awaiting_acceptance", prompted=self.clock())
            batch["sas"] = lan.sas_text(lan.sas_value(bytes.fromhex(self.fp), nonce, batch["nd"], batch["pairingId"], batch["label"]))
            return {"batchId": batch["id"], "state": "awaiting_acceptance"}
        if route == "/lan/v2/batch/status":
            batch = self.batch
            if not batch or args.get("batchId") != batch["id"]:
                raise lan.RemoteFailure("unknown_batch", 404)
            if client and client in (batch.get("client"), batch.get("issued")):
                return self.batch_json(False)
            if not batch["first"] or batch["phase"] == "awaiting_reveal":
                raise lan.RemoteFailure("unknown_batch", 404)
            expected = lan.status_proof(batch["nonce"], batch["id"])
            if not hmac.compare_digest(args.get("proof", ""), expected):
                raise lan.RemoteFailure("unknown_batch", 404)
            return self.batch_json(True)
        if route == "/lan/v1/clients/revoke":
            if not client or args.get("clientId") != client:
                raise lan.RemoteFailure("unauthorized", 401)
            del self.clients[client]
            return {"revoked": True}
        if not client:
            raise lan.RemoteFailure("unauthorized", 401)
        if route == "/lan/v1/capabilities":
            return {"v": 2, "deviceId": self.device_id, "pushRevision": self.revision, "epoch": "e" * 32,
                    "batchEntries": 8, "limits": {"chunkBytes": 4096}}
        request_id = args.get("requestId")
        key = (client, request_id)
        if route == "/lan/v1/uploads/start":
            if key in self.receipts:
                return {"state": "complete", "receipt": self.receipts[key]}
            batch = self.batch
            entry = None
            if batch and batch["phase"] == "accepted" and batch["client"] == client:
                entry = next((e for e in batch["entries"] if e["admission"] == "ok" and e["requestId"] == request_id), None)
            if key not in self.active and (not entry or entry["fingerprint"] != args["fingerprint"]):
                raise lan.RemoteFailure("not_accepted", 409)
            if key not in self.active:
                meta = {k: args[k] for k in ("requestId", "fingerprint", "kind", "name", "title", "sha256")}
                meta["size"] = int(args["size"])
                meta["baseRevision"] = args["baseRevision"]
                if meta["kind"] == "push" and int(meta["baseRevision"]) != self.revision:
                    raise lan.RemoteFailure("revision_conflict", 409)
                self.active[key] = {"meta": meta, "body": bytearray()}
            return self.status(key)
        if route == "/lan/v1/uploads/chunk":
            transaction = self.active[key]
            assert int(args["offset"]) == len(transaction["body"])
            data = base64.b64decode(args["data"], validate=True)
            assert len(data) <= 4096
            transaction["body"].extend(data)
            return self.status(key)
        if route == "/lan/v1/uploads/commit":
            if key in self.receipts:
                return {"state": "complete", "receipt": self.receipts[key]}
            if self.checking:
                self.checking -= 1
                result = self.status(key); result["state"] = "checking"
                self.active[key]["checking"] = True
                return result
            return self.publish(key)
        if route == "/lan/v1/uploads/status":
            if key in self.receipts:
                return {"state": "complete", "receipt": self.receipts[key]}
            if key not in self.active:
                raise lan.RemoteFailure("unknown_receipt", 404)
            if self.active[key].get("checking"):
                return self.publish(key)
            return self.status(key)
        if route == "/lan/v1/receipts":
            if key in self.receipts:
                return {"state": "complete", "receipt": self.receipts[key]}
            raise lan.RemoteFailure("unknown_receipt", 404)
        if route == "/lan/v1/uploads/cancel":
            if key in self.receipts:
                return {"state": "complete", "receipt": self.receipts[key]}
            self.active.pop(key, None)
            return {"state": "cancelled", "requestId": request_id, "fingerprint": "", "offset": 0, "size": 0}
        raise AssertionError(route)

    def publish(self, key):
        transaction = self.active.pop(key)
        meta, body = transaction["meta"], bytes(transaction["body"])
        assert len(body) == meta["size"] and hashlib.sha256(body).hexdigest() == meta["sha256"]
        receipt = {k: meta[k] for k in ("requestId", "fingerprint", "kind", "size", "sha256")}
        if meta["kind"] == "push":
            self.revision += 1
            receipt["revision"] = self.revision
        else:
            receipt["name"] = meta["name"]
            self.library += 1
        self.receipts[key] = receipt
        self.files.append((meta["name"], body))
        return {"state": "complete", "receipt": receipt}

    def status(self, key):
        transaction = self.active[key]
        meta = transaction["meta"]
        return {"state": "receiving", "requestId": meta["requestId"], "fingerprint": meta["fingerprint"],
                "size": meta["size"], "offset": len(transaction["body"])}

    def batch_json(self, token):
        batch = self.batch
        out = {"batchId": batch["id"], "state": batch["phase"],
               "entries": [{"requestId": e["requestId"], "admission": e["admission"]} for e in batch["entries"]]}
        if token and batch["phase"] == "accepted" and batch.get("token"):
            out.update(deviceId=self.device_id, clientId=batch["issued"], token=batch["token"], pushRevision=self.revision)
        return out

    def offer(self, args, client):
        first = client is None
        own = client and self.batch and self.batch["phase"] == "accepted" and self.batch["client"] == client
        if not self.finished() and not own:
            raise lan.RemoteFailure("batch_busy", 409)
        if first:
            if "label" not in args or "commit" not in args:
                raise lan.RemoteFailure("unauthorized", 401)
            if len(self.clients) >= self.capacity:
                raise lan.RemoteFailure("client_limit", 409)
        keys = sorted(k for k in args if re.fullmatch(r"e[0-7]", k))
        assert [int(k[1]) for k in keys] == list(range(len(keys)))
        assert set(args) <= set(keys) | {"label", "commit"}
        assert len(args) <= 16 and len(lan.urllib.parse.urlencode(args)) <= lan.REQUEST_BODY_MAX
        entries, push_seen = [], False
        for key in keys:
            entry = unpack(args[key])
            admission = "ok"
            if lan.fingerprint(entry) != entry["fingerprint"]:
                admission = "invalid_metadata"
            elif client and (client, entry["requestId"]) in self.receipts:
                admission = "already_received"
            elif entry["kind"] == "push" and push_seen:
                admission = "duplicate_push"
            elif entry["kind"] == "push" and entry["baseRevision"] != self.revision:
                admission = "revision_conflict"
            elif entry["kind"] == "file" and self.library >= 300:
                admission = "library_full"
            if admission == "ok" and entry["kind"] == "push":
                push_seen = True
            entry["admission"] = admission
            entries.append(entry)
        if not first and not entries:
            raise lan.RemoteFailure("invalid_entry", 400)
        if entries and not any(e["admission"] == "ok" for e in entries):
            return {"state": "blocked", "entries": [{"requestId": e["requestId"], "admission": e["admission"]} for e in entries]}
        batch = {"id": os.urandom(8).hex(), "first": first, "entries": entries, "client": client,
                 "label": args.get("label") if first else self.clients[client]["label"], "started": self.clock()}
        self.batch = batch
        listed = [{"requestId": e["requestId"], "admission": e["admission"]} for e in entries]
        if first:
            batch.update(phase="awaiting_reveal", commit=args["commit"], pairingId=os.urandom(8).hex(), nd=os.urandom(32))
            return {"batchId": batch["id"], "state": "awaiting_reveal", "pairingId": batch["pairingId"],
                    "deviceNonce": batch["nd"].hex(), "deviceId": self.device_id, "entries": listed}
        batch.update(phase="awaiting_acceptance", prompted=self.clock())
        return {"batchId": batch["id"], "state": "awaiting_acceptance", "entries": listed}


class Network:
    """Transport double: certificate pinning happens before the reader sees bytes."""

    def __init__(self, clock):
        self.clock, self.readers, self.presented = clock, {}, {}
        self.lost, self.log = {}, []

    def add(self, reader):
        self.readers[reader.host] = reader
        return reader

    def request(self, address, method, path, form=None, *, pin, credentials=None, deadline):
        self.clock.now += 0.05
        if pin is None:
            assert path in lan.UNPINNED_PATHS and credentials is None and form is None, path
        reader = self.readers.get(address[0])
        if reader is None or not reader.on:
            raise lan.Failure("device_unreachable")
        presented = self.presented.get(address[0], reader.fp)
        if pin is not None and pin != presented:
            raise lan.Failure("pin_mismatch")
        self.log.append((address[0], method, path, dict(form or {}), credentials))
        route = path.partition("?")[0]
        result = reader.handle(method, path, form, credentials)
        if self.lost.get(route, 0):
            self.lost[route] -= 1
            raise lan.Failure("connection_lost", sent=True)
        if address[0] in self.presented and "deviceId" in result:
            result = dict(result, deviceId=presented[:16])   # a relaying MITM rewrites the ID
        return result, presented


class Resolver:
    def __init__(self):
        self.hosts, self.browsed, self.resolved = {}, [], []

    def resolve(self, device_id, deadline):
        self.resolved.append(device_id)
        return list(self.hosts.get(device_id, []))

    def browse(self, deadline):
        self.browsed.append(deadline)
        return [{"deviceId": k, "host": v[0]} for k, v in sorted(self.hosts.items())]


class World(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.state = self.root / "state"
        self.clock = Clock()
        self.net = Network(self.clock)
        self.mdns = Resolver()
        self.reader = self.net.add(Reader(self.clock))

    def tearDown(self):
        self.tmp.cleanup()

    def source(self, name="notes.md", body=None):
        path = self.root / name
        path.write_bytes(body if body is not None else ("Synthetic **Markdown** %s\n" % name).encode() * 50)
        return str(path)

    def run_cli(self, *argv):
        out = io.StringIO()
        code = lan.main(["--state-dir", str(self.state), *argv], transport=self.net, resolver=self.mdns,
                        clock=self.clock, sleep=self.clock.sleep, stdout=out)
        text = out.getvalue()
        return code, (json.loads(text) if "--json" in argv else text)

    def db(self):
        with lan.Store(self.state) as store:
            return json.loads(json.dumps(store.db))

    def pair(self, label="Hermes", reader=None):
        reader = reader or self.reader
        self.mdns.hosts = {reader.device_id: [reader.host]}
        code, result = self.run_cli("pair", "--label", label, "--json", "--wait", "5")
        self.assertEqual(result["next"], "awaiting_pairing_confirmation")
        self.assertEqual(result["sas"], reader.batch["sas"])
        reader.accept()
        code, result = self.run_cli("sync", "--batch-id", result["batchId"], "--json", "--wait", "5")
        self.assertEqual(code, 0)
        self.assertEqual(result["state"], "done")
        self.assertIn(reader.device_id, self.db()["devices"])
        return result


# ------------------------------------------------------------------- vectors

class Vectors(unittest.TestCase):
    def test_shared_pairing_vectors_match_firmware(self):
        f, ns, nd = b"\x11" * 32, b"\x22" * 32, b"\x33" * 32
        value = lan.sas_value(f, ns, nd, "0123456789abcdef", "Hermes 書")
        self.assertEqual((value, lan.sas_text(value)), (506740, "506 740"))
        self.assertEqual(lan.status_proof(ns, "fedcba9876543210"), "4b19bde7b8dd1e27aab707265889ba8c7a1b22e0bba59770d2f6dc3ebe6533e4")
        self.assertEqual(lan.commitment(ns), "9f72ea0cf49536e3c66c787f705186df9a4378083753ae9536d65b3ad7fcddc4")
        self.assertEqual(lan.sas_text(7), "000 007")

    def test_packed_entry_vector_matches_firmware(self):
        # tests/lan_batch.cpp parses this exact string (sample(Kind::File)).
        item = {"requestId": "a" * 32, "fingerprint": "b" * 64, "kind": "file", "name": "書 book.epub",
                "title": "Synthetic 題", "size": 4096, "sha256": "c" * 64, "baseRevision": None}
        packed = lan.pack_entry(item)
        self.assertEqual(packed, "32:" + "a" * 32 + "64:" + "b" * 64 + "4:file13:書 book.epub13:Synthetic 題4:409664:" + "c" * 64 + "0:")
        push = dict(item, kind="push", name="Push.md", baseRevision=7)
        self.assertTrue(lan.pack_entry(push).endswith("1:7"))

    def test_canonical_fingerprint_vector(self):
        item = dict(kind="push", name="Push.md", title="Synthetic", size=3, sha256="0" * 64, baseRevision=0)
        self.assertEqual(lan.fingerprint(item), "7c5dc380b37e359158a305fc6aff88f3397a4c8a7d1b8a2d00c89c3fd0016d74")

    def test_worst_case_batch_fits_field_and_body_bounds(self):
        title = "\u0001".replace("\u0001", "題") * 32   # 96 UTF-8 bytes
        form = {"label": "書" * 16, "commit": "f" * 64}
        for n in range(8):
            item = {"requestId": "a" * 32, "fingerprint": "b" * 64, "kind": "push", "name": "書" * 31 + ".md",
                    "title": title, "size": 4294967295, "sha256": "c" * 64, "baseRevision": 4294967295}
            form["e%d" % n] = lan.pack_entry(item)
        self.assertLessEqual(len(form), 16)
        self.assertLessEqual(len(lan.urllib.parse.urlencode(form)), lan.REQUEST_BODY_MAX)


# --------------------------------------------------------------- local store

class StoreTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.state = Path(self.tmp.name) / "state"

    def tearDown(self):
        self.tmp.cleanup()

    def test_store_is_private_and_locked(self):
        with lan.Store(self.state) as store:
            store.save()
            if POSIX:
                self.assertEqual(self.state.stat().st_mode & 0o777, 0o700)
                self.assertEqual((self.state / "state.json").stat().st_mode & 0o777, 0o600)
            with self.assertRaisesRegex(lan.Failure, "lock_held"):
                with lan.Store(self.state):
                    pass
        with lan.Store(self.state):
            pass

    @unittest.skipUnless(POSIX, "POSIX modes")
    def test_permissions_and_corruption_have_specific_codes(self):
        with lan.Store(self.state) as store:
            store.save()
        os.chmod(self.state, 0o755)
        with self.assertRaisesRegex(lan.Failure, "state_dir_permissions"):
            lan.Store(self.state).__enter__()
        os.chmod(self.state, 0o700)
        (self.state / "state.json").write_bytes(b"{not json")
        with self.assertRaisesRegex(lan.Failure, "store_corrupt"):
            with lan.Store(self.state):
                pass
        blocker = Path(self.tmp.name) / "file"
        blocker.write_text("x")
        with self.assertRaisesRegex(lan.Failure, "state_dir_unavailable"):
            with lan.Store(blocker / "state"):
                pass

    @unittest.skipUnless(POSIX, "symlinks")
    def test_symlinked_state_rejected(self):
        real = Path(self.tmp.name) / "real"
        with lan.Store(real) as store:
            store.save()
        os.symlink(real, self.state)
        with self.assertRaisesRegex(lan.Failure, "state_dir_permissions"):
            with lan.Store(self.state):
                pass

    def test_windows_layer_locks_and_rejects_links(self):
        calls = []

        class Msvcrt:
            LK_NBLCK, LK_UNLCK = 2, 0
            held = set()

            @classmethod
            def locking(cls, fd, mode, size):
                key = os.fstat(fd).st_ino
                calls.append(mode)
                if mode == cls.LK_NBLCK:
                    if key in cls.held:
                        raise OSError("locked")
                    cls.held.add(key)
                else:
                    cls.held.discard(key)
        with patch.dict(sys.modules, {"msvcrt": Msvcrt}):
            with lan.Store(self.state, files=lan.WindowsFiles) as store:
                store.save()
                with self.assertRaisesRegex(lan.Failure, "lock_held"):
                    lan.Store(self.state, files=lan.WindowsFiles).__enter__()
            self.assertIn(Msvcrt.LK_UNLCK, calls)
            if POSIX:
                link = Path(self.tmp.name) / "link"
                os.symlink(self.state, link)
                with self.assertRaisesRegex(lan.Failure, "state_dir_permissions"):
                    with lan.Store(link, files=lan.WindowsFiles):
                        pass
        with patch.dict(os.environ, {"LOCALAPPDATA": ""}):
            with self.assertRaisesRegex(lan.Failure, "state_dir_unavailable"):
                lan.WindowsFiles.default_root()
        with patch.dict(os.environ, {"LOCALAPPDATA": self.tmp.name}):
            self.assertEqual(lan.WindowsFiles.default_root(), Path(self.tmp.name) / "paperdiy")

    def test_v1_state_migrates_without_losing_unresolved_entries(self):
        self.state.mkdir(mode=0o700)
        (self.state / "objects").mkdir(mode=0o700)
        device = {"deviceId": "0123456789abcdef", "host": "192.0.2.5", "port": 8443, "certSha256": "0123456789abcdef" + "a" * 48,
                  "clientId": "b" * 16, "token": "c" * 64, "pushRevision": 3}
        base = {"device": "home", "kind": "file", "name": "a.md", "title": "", "size": 5, "sha256": "d" * 64,
                "baseRevision": None, "offset": 0, "created": 1}
        items = []
        for n, (state, attempted) in enumerate([("queued", False), ("receiving", True), ("delivered", True), ("queued", True)]):
            item = dict(base, requestId="%032x" % n, state=state, attempted=attempted)
            item["fingerprint"] = lan.fingerprint(item)
            items.append(item)
        old = {"version": 1, "devices": {"home": device}, "items": items}
        fd = os.open(self.state / "state.json", os.O_CREAT | os.O_WRONLY, 0o600)
        with open(fd, "w") as stream:
            json.dump(old, stream)
        with lan.Store(self.state) as store:
            self.assertEqual(list(store.db["devices"]), ["0123456789abcdef"])
            states = [(i["state"], i.get("code"), i["commitSent"]) for i in store.db["items"]]
            self.assertEqual(states, [("queued", None, False), ("uncertain", "commit_unconfirmed", True),
                                      ("delivered", None, False), ("uncertain", "commit_unconfirmed", True)])
            store.save()
        self.assertEqual(json.loads((self.state / "state.json").read_text())["version"], 2)

    def test_enqueue_is_atomic_and_bounded(self):
        root = Path(self.tmp.name)
        good, bad = root / "a.md", root / "b.png"
        good.write_text("hello"); bad.write_text("png")
        with lan.Store(self.state) as store:
            with self.assertRaisesRegex(lan.Failure, "unsupported_format"):
                store.enqueue(None, [str(good), str(bad)], "file")
            self.assertEqual(store.db["items"], [])
            self.assertEqual(list((self.state / "objects").iterdir()), [])
            with self.assertRaisesRegex(lan.Failure, "source_unreadable"):
                store.enqueue(None, [str(root / "missing.md")], "file")
            (root / "empty.md").write_text("")
            with self.assertRaisesRegex(lan.Failure, "empty_document"):
                store.enqueue(None, [str(root / "empty.md")], "file")
            (root / "bad.md").write_bytes(b"\xff")
            with self.assertRaisesRegex(lan.Failure, "invalid_utf8"):
                store.enqueue(None, [str(root / "bad.md")], "push")
            (root / "big.md").write_bytes(b"x" * (lan.PUSH_MAX + 1))
            with self.assertRaisesRegex(lan.Failure, "size_limit"):
                store.enqueue(None, [str(root / "big.md")], "push")
            [item] = store.enqueue(None, [str(good)], "file")
            self.assertEqual(item["name"], "a.md")
            self.assertEqual(len(list((self.state / "objects").iterdir())), 1)


# ------------------------------------------------------------ delivery flows

class FirstContact(World):
    def test_push_first_contact_pairs_and_delivers_with_one_confirmation(self):
        self.mdns.hosts = {self.reader.device_id: [self.reader.host]}
        code, result = self.run_cli("push", self.source(), "--title", "Weekly 週", "--label", "Hermes", "--json", "--wait", "5")
        self.assertEqual((code, result["state"], result["next"]), (0, "pending", "awaiting_pairing_confirmation"))
        self.assertEqual(result["sas"], self.reader.batch["sas"])
        self.assertRegex(result["sas"], r"^[0-9]{3} [0-9]{3}$")
        self.assertEqual(result["items"][0]["code"], "awaiting_pairing_confirmation")
        self.assertEqual(self.reader.clients, {})            # nothing paired before A+B
        self.assertEqual(len(self.db()["pending"]), 1)
        self.reader.accept_at = self.clock() + 3
        code, result = self.run_cli("sync", "--batch-id", result["batchId"], "--json", "--wait", "60")
        self.assertEqual((code, result["state"], result["summary"]["delivered"]), (0, "done", 1))
        db = self.db()
        self.assertEqual(db["pending"], [])
        device = db["devices"][self.reader.device_id]
        self.assertEqual((device["label"], device["certSha256"], device["pushRevision"]), ("Hermes", self.reader.fp, 1))
        self.assertEqual(self.reader.files[0][0], "Push.md")
        self.assertEqual(self.reader.revision, 1)
        # The proof travels only to the pinned certificate, and no bearer
        # credential is ever sent before pairing completes.
        first_bearer = next(n for n, entry in enumerate(self.net.log) if entry[4])
        self.assertTrue(all("/lan/v1/uploads" in e[2] or "/lan/v2/batch/status" in e[2] for e in self.net.log[first_bearer:]))

    def test_human_mode_shows_number_then_waits_for_acceptance(self):
        self.mdns.hosts = {self.reader.device_id: [self.reader.host]}
        self.reader.accept_at = self.clock() + 4
        code, text = self.run_cli("send", self.source("a.md"), self.source("b.md"), "--label", "DSH")
        self.assertEqual(code, 0)
        self.assertIn("New sender DSH · ", text)
        self.assertIn("delivered  a.md", text)
        self.assertEqual(sorted(name for name, _ in self.reader.files), ["a.md", "b.md"])

    def test_new_send_finishes_an_open_first_contact_first(self):
        self.mdns.hosts = {self.reader.device_id: [self.reader.host]}
        code, result = self.run_cli("send", self.source("a.md"), "--label", "H", "--json", "--wait", "5")
        self.assertEqual(result["next"], "awaiting_pairing_confirmation")
        self.reader.accept()
        def accept_later(original=self.reader.handle):
            def handle(method, path, form, credentials):
                if self.reader.prompt() and self.reader.accept_at is None:
                    self.reader.accept_at = self.clock() + 1
                return original(method, path, form, credentials)
            return handle
        self.reader.handle = accept_later()
        code, result = self.run_cli("send", self.source("b.md"), "--json", "--wait", "30")
        self.assertEqual((code, result["summary"]["delivered"]), (0, 1), result)
        self.assertEqual(sorted(n for n, _ in self.reader.files), ["a.md", "b.md"])
        self.assertEqual(self.db()["pending"], [])

    def test_standalone_pairing_and_repairing(self):
        self.pair("Hermes")
        self.assertEqual(len(self.reader.clients), 1)
        old = self.db()["devices"][self.reader.device_id]["clientId"]
        self.pair("Hermes")                                   # explicit re-pair replaces the record
        self.assertNotEqual(self.db()["devices"][self.reader.device_id]["clientId"], old)

    def test_mitm_number_differs_and_unbound_identity_is_refused(self):
        self.mdns.hosts = {self.reader.device_id: [self.reader.host]}
        attacker = hashlib.sha256(b"attacker certificate").hexdigest()
        self.net.presented[self.reader.host] = attacker
        code, result = self.run_cli("pair", "--label", "Hermes", "--json", "--wait", "1")
        # The rewritten ID matches the attacker's certificate but not the
        # advertised reader, so nothing is offered at all.
        self.assertEqual((code, result["code"]), (2, "pin_mismatch"))
        self.assertIsNone(self.reader.batch)
        # Without discovery (--host) the relay reaches the reader, but both
        # sides derive the number from different certificates.
        code, result = self.run_cli("pair", "--label", "Hermes", "--host", self.reader.host, "--json", "--wait", "1")
        self.assertEqual(result["next"], "awaiting_pairing_confirmation")
        self.assertNotEqual(result["sas"], self.reader.batch["sas"])

    def test_status_never_goes_to_a_different_certificate(self):
        self.mdns.hosts = {self.reader.device_id: [self.reader.host]}
        code, result = self.run_cli("pair", "--label", "Hermes", "--json", "--wait", "1")
        self.reader.accept()
        self.net.presented[self.reader.host] = hashlib.sha256(b"swapped").hexdigest()
        before = len(self.reader.calls)
        code, result = self.run_cli("sync", "--batch-id", result["batchId"], "--json", "--wait", "5")
        self.assertEqual((code, result["code"]), (2, "pin_mismatch"))
        self.assertEqual(len(self.reader.calls), before)       # no batch ID or proof sent
        self.assertEqual(self.db()["devices"], {})

    def test_wrong_proof_yields_no_token(self):
        self.mdns.hosts = {self.reader.device_id: [self.reader.host]}
        code, result = self.run_cli("pair", "--label", "Hermes", "--json", "--wait", "1")
        self.reader.accept()
        with lan.Store(self.state) as store:
            store.db["pending"][0]["senderNonce"] = "00" * 32
            store.save()
        code, result = self.run_cli("sync", "--batch-id", result["batchId"], "--json", "--wait", "5")
        self.assertEqual(result["next"], "not_accepted")
        self.assertEqual((self.db()["devices"], self.db()["pending"]), ({}, []))

    def test_pending_record_removed_on_expiry_and_entries_not_auto_reoffered(self):
        self.mdns.hosts = {self.reader.device_id: [self.reader.host]}
        code, result = self.run_cli("push", self.source(), "--label", "Hermes", "--json", "--wait", "5")
        self.clock.now += 61
        code, result = self.run_cli("sync", "--batch-id", result["batchId"], "--json", "--wait", "5")
        self.assertEqual((code, result["next"]), (0, "not_accepted"))
        self.assertEqual(result["items"][0]["code"], "not_accepted")
        self.assertEqual(self.db()["pending"], [])
        offers = sum(1 for c in self.reader.calls if c[1] == "/lan/v2/batch/offer")
        code, result = self.run_cli("sync", "--label", "Hermes", "--json", "--wait", "5")
        self.assertEqual(sum(1 for c in self.reader.calls if c[1] == "/lan/v2/batch/offer"), offers)
        code, result = self.run_cli("sync", "--label", "Hermes", "--reoffer", "--json", "--wait", "5")
        self.assertEqual(result["next"], "awaiting_pairing_confirmation")

    def test_discovery_none_several_and_label_rules(self):
        code, result = self.run_cli("push", self.source(), "--json", "--wait", "5")
        self.assertEqual((code, result["code"]), (2, "label_required"))
        self.assertEqual(self.db()["items"], [])               # nothing queued on a usage error
        self.mdns.hosts = {}
        code, result = self.run_cli("push", self.source(), "--label", "H", "--json", "--wait", "5")
        self.assertEqual((code, result["code"]), (2, "device_not_found"))
        self.assertEqual(self.db()["items"][0]["state"], "queued")
        other = self.net.add(Reader(self.clock, host="192.0.2.11", cert=b"another reader"))
        self.mdns.hosts = {self.reader.device_id: [self.reader.host], other.device_id: [other.host]}
        code, result = self.run_cli("sync", "--label", "H", "--json", "--wait", "5")
        self.assertEqual(result["code"], "multiple_devices")
        self.assertEqual(sorted(c["host"] for c in result["candidates"]), [self.reader.host, other.host])
        self.assertIsNone(self.reader.batch); self.assertIsNone(other.batch)
        code, result = self.run_cli("sync", "--label", "H", "--host", other.host, "--json", "--wait", "5")
        self.assertEqual(result["next"], "awaiting_pairing_confirmation")
        self.assertIsNotNone(other.batch)

    def test_all_blocked_first_contact_creates_nothing(self):
        self.mdns.hosts = {self.reader.device_id: [self.reader.host]}
        self.reader.library = 300
        code, result = self.run_cli("send", self.source("a.md"), "--label", "H", "--json", "--wait", "5")
        self.assertEqual((code, result["items"][0]["state"], result["items"][0]["code"]), (2, "blocked", "library_full"))
        self.assertEqual((self.reader.clients, self.reader.batch, self.db()["pending"]), ({}, None, []))


class Paired(World):
    def setUp(self):
        super().setUp()
        self.pair()
        self.device = self.reader.device_id

    def test_paired_push_needs_acceptance_then_delivers(self):
        code, result = self.run_cli("push", self.source(), "--json", "--wait", "5")
        self.assertEqual((code, result["next"]), (0, "awaiting_acceptance"))
        self.assertEqual(self.reader.files, [])
        self.reader.accept_at = self.clock() + 2
        code, result = self.run_cli("sync", "--batch-id", result["batchId"], "--json", "--wait", "60")
        self.assertEqual((code, result["state"], result["summary"]["delivered"]), (0, "done", 1))
        # Next push builds on the delivered revision.
        code, result = self.run_cli("push", self.source(), "--json", "--wait", "5")
        self.assertEqual(self.reader.batch["entries"][0]["baseRevision"], 1)

    def test_batches_of_eight_and_receipt_per_entry(self):
        sources = [self.source("f%d.md" % n) for n in range(9)]
        self.reader.accept_at = self.clock() + 1
        def accept_next(original=self.reader.handle):
            def handle(method, path, form, credentials):
                if self.reader.prompt() and self.reader.accept_at is None:
                    self.reader.accept_at = self.clock() + 1
                return original(method, path, form, credentials)
            return handle
        self.reader.handle = accept_next()
        code, result = self.run_cli("send", *sources, "--json", "--wait", "120")
        self.assertEqual((code, result["summary"]["delivered"]), (0, 9))
        offers = [e for e in self.net.log if e[2] == "/lan/v2/batch/offer"]
        self.assertEqual([len(e[3]) for e in offers if "label" not in e[3]], [8, 1])

    def test_not_accepted_when_switched_off_and_asked_before_reoffer(self):
        code, result = self.run_cli("push", self.source(), "--json", "--wait", "5")
        self.reader.switch_off(); self.reader.switch_on()
        code, result = self.run_cli("sync", "--json", "--wait", "5")
        self.assertEqual((code, result["items"][0]["code"]), (0, "not_accepted"))
        offers = len([e for e in self.net.log if e[2] == "/lan/v2/batch/offer"])
        self.run_cli("sync", "--json", "--wait", "5")
        self.assertEqual(len([e for e in self.net.log if e[2] == "/lan/v2/batch/offer"]), offers)
        self.reader.accept_at = self.clock() + 1
        code, result = self.run_cli("sync", "--reoffer", "--json", "--wait", "30")
        self.assertEqual(result["summary"]["delivered"], 1)

    def test_one_offer_per_entry_per_command_even_with_reoffer(self):
        self.run_cli("send", self.source("a.md"), "--json", "--wait", "1")
        self.clock.now += 61
        code, result = self.run_cli("sync", "--reoffer", "--json", "--wait", "200")
        offers = [e for e in self.net.log if e[2] == "/lan/v2/batch/offer" and "label" not in e[3]]
        self.assertEqual(len(offers), 2)   # the original send, then exactly one re-offer
        self.assertEqual(result["items"][0]["code"], "not_accepted")

    def test_interruption_before_commit_is_queued_and_resumes(self):
        code, result = self.run_cli("send", self.source("a.md", b"x" * 10000), "--json", "--wait", "1")
        self.reader.accept()
        self.net.lost["/lan/v1/uploads/chunk"] = 1
        code, result = self.run_cli("sync", "--json", "--wait", "5")
        self.assertEqual((code, result["items"][0]["state"], result["items"][0]["code"]), (0, "queued", "transfer_interrupted"))
        self.assertEqual(self.reader.receipts, {})
        code, result = self.run_cli("sync", "--json", "--wait", "5")
        self.assertEqual(result["summary"]["delivered"], 1)
        self.assertEqual(self.reader.files[0][1], b"x" * 10000)

    def test_lost_commit_is_uncertain_until_receipt_and_blocks_new_offers(self):
        self.run_cli("send", self.source("a.md"), "--json", "--wait", "1")
        self.reader.accept()
        self.net.lost["/lan/v1/uploads/commit"] = 1
        self.net.lost["/lan/v1/receipts"] = 5
        code, result = self.run_cli("sync", "--json", "--wait", "5")
        self.assertEqual((code, result["items"][0]["state"], result["items"][0]["code"]), (2, "uncertain", "commit_unconfirmed"))
        self.assertEqual(len(self.reader.receipts), 1)        # it did land
        self.run_cli("send", self.source("b.md"), "--json", "--wait", "0")
        offers = len([e for e in self.net.log if e[2] == "/lan/v2/batch/offer"])
        code, result = self.run_cli("sync", "--json", "--wait", "5")
        self.assertEqual(result["next"], "uncertain")
        self.assertEqual(len([e for e in self.net.log if e[2] == "/lan/v2/batch/offer"]), offers)
        self.net.lost.clear()
        code, result = self.run_cli("sync", "--json", "--wait", "5")
        states = {i["name"]: i["state"] for i in result["items"]}
        self.assertEqual(states["a.md"], "delivered")
        self.assertEqual(result["next"], "awaiting_acceptance")   # b.md offered only after a.md resolved

    def test_no_delivered_without_matching_receipt(self):
        self.run_cli("send", self.source("a.md"), "--json", "--wait", "1")
        self.reader.accept()
        original = self.reader.publish
        def forged(key):
            result = original(key); result["receipt"] = dict(result["receipt"], sha256="0" * 64); return result
        self.reader.publish = forged
        code, result = self.run_cli("sync", "--json", "--wait", "5")
        self.assertNotEqual(result["items"][0]["state"], "delivered")
        self.assertEqual(code, 2)

    def test_checking_is_polled_until_complete(self):
        self.reader.checking = 1
        self.reader.accept_at = self.clock() + 1
        code, result = self.run_cli("send", self.source("a.md"), "--json", "--wait", "30")
        self.assertEqual(result["summary"]["delivered"], 1)

    def test_revision_conflict_is_blocked_and_never_rebased(self):
        self.reader.revision = 5
        with lan.Store(self.state) as store:
            store.db["devices"][self.device]["pushRevision"] = 2; store.save()
        self.run_cli("push", self.source(), "--json", "--wait", "0")
        code, result = self.run_cli("sync", "--json", "--wait", "5")
        # capabilities refreshed the known revision, but the queued entry keeps its base.
        self.assertEqual((code, result["items"][0]["state"], result["items"][0]["code"]), (2, "blocked", "revision_conflict"))

    def test_device_selection(self):
        code, result = self.run_cli("push", self.source(), "--device", "f" * 16, "--json", "--wait", "1")
        self.assertEqual(result["code"], "unknown_device")
        second = self.net.add(Reader(self.clock, host="192.0.2.20", cert=b"second reader"))
        self.mdns.hosts = {second.device_id: [second.host]}
        self.run_cli("pair", "--label", "Hermes", "--host", second.host, "--json", "--wait", "1")
        second.accept()
        self.run_cli("sync", "--batch-id", second.batch["id"], "--json", "--wait", "5")
        code, result = self.run_cli("push", self.source(), "--json", "--wait", "1")
        self.assertEqual((code, result["code"]), (2, "ambiguous_device"))
        code, result = self.run_cli("push", self.source(), "--device", second.device_id, "--json", "--wait", "1")
        self.assertEqual(result["next"], "awaiting_acceptance")
        self.assertIsNotNone(second.prompt())

    def test_paired_reader_found_by_id_after_address_change(self):
        old = self.reader.host
        del self.net.readers[old]
        self.reader.host = "192.0.2.77"
        self.net.add(self.reader)
        # Another reader now answers at the old address; its pin differs.
        self.net.add(Reader(self.clock, host=old, cert=b"stranger"))
        self.mdns.hosts = {self.device: ["192.0.2.77"]}
        self.reader.accept_at = self.clock() + 1
        code, result = self.run_cli("send", self.source("a.md"), "--json", "--wait", "30")
        self.assertEqual(result["summary"]["delivered"], 1)
        self.assertEqual(self.db()["devices"][self.device]["host"], "192.0.2.77")
        stranger = self.net.readers[old]
        self.assertEqual(stranger.calls, [])                  # nothing reached the stranger

    def test_unreachable_reader_keeps_host_and_queues(self):
        self.reader.switch_off()
        self.mdns.hosts = {self.device: ["192.0.2.99"]}
        code, result = self.run_cli("send", self.source("a.md"), "--json", "--wait", "5")
        self.assertEqual((code, result["next"], result["items"][0]["code"]), (0, "device_unreachable", "device_unreachable"))
        self.assertEqual(self.db()["devices"][self.device]["host"], self.reader.host)
        self.assertLessEqual(self.clock() - 1000, 30)

    def test_wait_zero_only_queues(self):
        calls = len(self.net.log)
        code, result = self.run_cli("send", self.source("a.md"), "--json", "--wait", "0")
        self.assertEqual((code, result["items"][0]["state"], len(self.net.log)), (0, "queued", calls))

    def test_revoke_cancel_abandon_prune(self):
        self.run_cli("send", self.source("a.md"), "--json", "--wait", "0")
        request_id = self.db()["items"][0]["requestId"]
        code, result = self.run_cli("cancel", "--request-id", request_id, "--json")
        self.assertEqual(result["items"][0]["state"], "cancelled")
        self.run_cli("send", self.source("b.md"), "--json", "--wait", "0")
        request_id = self.db()["items"][-1]["requestId"]
        code, result = self.run_cli("abandon", "--request-id", request_id, "--json")
        self.assertEqual(result, {"state": "abandoned", "requestId": request_id, "remoteOutcome": "not_sent"})
        self.reader.accept_at = self.clock() + 1
        self.run_cli("send", self.source("c.md"), "--json", "--wait", "30")
        code, result = self.run_cli("prune", "--delivered", "--json")
        self.assertEqual(result["count"], 1)
        code, result = self.run_cli("revoke", "--json")
        self.assertEqual(result["state"], "revoked")
        self.assertEqual((self.reader.clients, self.db()["devices"]), ({}, {}))

    def test_revoked_on_device_reports_unauthorized(self):
        self.reader.clients.clear()
        code, result = self.run_cli("send", self.source("a.md"), "--json", "--wait", "5")
        self.assertEqual((code, result["code"]), (2, "unauthorized"))


class Output(World):
    def test_json_is_ascii_and_human_output_survives_non_utf8_console(self):
        self.mdns.hosts = {}
        self.run_cli("send", self.source("書.md"), "--json", "--wait", "0")
        code, result = self.run_cli("status", "--json")
        self.assertEqual(result["items"][0]["name"], "書.md")
        out = io.TextIOWrapper(io.BytesIO(), encoding="cp1252", errors="strict")
        with lan.Store(self.state) as store:
            items = list(store.db["items"])
        lan.emit(lan.summarize(items, {}), False, out)
        out.flush()
        self.assertIn(b"queued", out.buffer.getvalue())
        buffer = io.StringIO()
        lan.emit({"state": "x", "name": "書"}, True, buffer)
        self.assertTrue(buffer.getvalue().isascii())

    def test_errors_do_not_leak_paths_or_tokens(self):
        code, text = self.run_cli("send", str(self.root / "secret-name.md"), "--json", "--wait", "0")
        self.assertEqual(text, {"state": "error", "code": "source_unreadable"})
        self.pair()
        code, text = self.run_cli("status")
        token = self.db()["devices"][self.reader.device_id]["token"]
        self.assertNotIn(token, text)
        code, result = self.run_cli("status", "--json")
        self.assertNotIn(token, json.dumps(result))

    def test_invalid_budget(self):
        code, result = self.run_cli("sync", "--json", "--wait", "601")
        self.assertEqual(result["code"], "invalid_budget")
        code, result = self.run_cli("sync", "--json", "--wait", "100", "--budget", "50")
        self.assertEqual(result["code"], "invalid_budget")


# ------------------------------------------------- real firmware over the wire

WIRE = os.environ.get("PAPERDIY_LAN_WIRE")


class FirmwareWire:
    """The real lan_service handlers (tests/lan_service.cpp, PAPERDIY_LAN_WIRE)
    fed the sender's exact request bytes. Firmware time follows the test clock."""
    HOST, FP = "192.0.2.50", "a" * 64

    def __init__(self, binary, clock):
        self.proc = subprocess.Popen([binary], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        self.clock, self.accept_at, self.after_request = clock, None, None
        self.paths = []

    def close(self):
        self.proc.stdin.close(); self.proc.wait(10)
        assert self.proc.returncode == 0, self.proc.returncode

    def command(self, line):
        self.proc.stdin.write(line + "\n"); self.proc.stdin.flush()
        reply = self.proc.stdout.readline().strip()
        assert reply, "firmware driver exited"
        return reply

    def prompt(self):
        _, pending, total, label, sas = self.command("PROMPT").split(" ")
        return {"pending": pending == "1", "total": int(total), "label": bytes.fromhex(label).decode(),
                "sas": bytes.fromhex(sas).decode()}

    def sleep(self, seconds):
        self.clock.sleep(seconds)
        self.command("TICK %d" % max(1, int(seconds * 1000)))

    def request(self, address, method, path, form=None, *, pin, credentials=None, deadline):
        packet = lan.HTTPS.packet(address, method, path, form, pin=pin, credentials=credentials)
        self.clock.now += 0.05
        self.command("TICK 50")
        if address[0] != self.HOST:
            raise lan.Failure("device_unreachable")
        if pin is not None and pin != self.FP:
            raise lan.Failure("pin_mismatch")
        if self.accept_at is not None and self.clock() >= self.accept_at and self.prompt()["pending"]:
            assert self.command("ACCEPT") == "OK"
            self.accept_at = None
        reply = self.command("REQ " + packet.hex())
        if reply == "DOWN":
            raise lan.Failure("device_unreachable")
        self.paths.append(path.partition("?")[0])
        result = lan.HTTPS.parse(bytes.fromhex(reply[4:]))
        if self.after_request:
            self.after_request(path)
        return result, self.FP


@unittest.skipUnless(WIRE, "set PAPERDIY_LAN_WIRE to the lan_service_wire binary")
class Firmware(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.clock = Clock()
        self.wire = FirmwareWire(WIRE, self.clock)
        self.mdns = Resolver()
        self.mdns.hosts = {"a" * 16: [FirmwareWire.HOST]}

    def tearDown(self):
        self.wire.close()
        self.tmp.cleanup()

    def run_cli(self, *argv):
        out = io.StringIO()
        code = lan.main(["--state-dir", str(self.root / "state"), *argv], transport=self.wire, resolver=self.mdns,
                        clock=self.clock, sleep=self.wire.sleep, stdout=out)
        return code, json.loads(out.getvalue())

    def source(self, name, body):
        path = self.root / name
        path.write_bytes(body)
        return str(path)

    def test_first_contact_push_then_batches_against_real_handlers(self):
        code, result = self.run_cli("push", self.source("p.md", "週報 one".encode()), "--title", "Weekly 週", "--label", "Hermes 書",
                                    "--json", "--wait", "5")
        self.assertEqual(result["next"], "awaiting_pairing_confirmation", result)
        prompt = self.wire.prompt()
        self.assertEqual((prompt["pending"], prompt["total"], prompt["label"], prompt["sas"]), (True, 1, "Hermes 書", result["sas"]))
        self.wire.accept_at = self.clock()
        code, result = self.run_cli("sync", "--batch-id", result["batchId"], "--json", "--wait", "30")
        self.assertEqual((code, result["state"], result["summary"]["delivered"]), (0, "done", 1), result)
        # Paired: one confirmation for a push plus three files.
        sources = [self.source("f%d.md" % n, ("file %d\n" % n).encode() * (2000 + n)) for n in range(3)]
        self.wire.accept_at = self.clock() + 2
        code, result = self.run_cli("send", *sources, "--json", "--wait", "60")
        self.assertEqual((code, result["summary"]["delivered"]), (0, 3), result)
        self.wire.accept_at = self.clock() + 1
        code, result = self.run_cli("push", self.source("p2.md", b"two"), "--json", "--wait", "30")
        self.assertEqual(result["summary"]["delivered"], 1, result)
        self.assertNotIn("/lan/v1/pair", self.wire.paths)

    def test_expiry_and_n_restart_against_real_handlers(self):
        self.wire.accept_at = self.clock()
        self.run_cli("pair", "--label", "DSH", "--json", "--wait", "5")
        code, result = self.run_cli("sync", "--json", "--wait", "30")
        self.assertEqual(result["state"], "done", result)
        code, result = self.run_cli("send", self.source("a.md", b"a" * 30000), "--json", "--wait", "2")
        self.assertEqual(result["next"], "awaiting_acceptance", result)
        self.wire.sleep(61)
        code, result = self.run_cli("sync", "--json", "--wait", "5")
        self.assertEqual(result["items"][0]["code"], "not_accepted", result)
        # Accepted, then N switched off mid-transfer: nothing published.
        chunks = []

        def switch_off(path):
            if path.startswith("/lan/v1/uploads/chunk"):
                chunks.append(path)
                if len(chunks) == 3:
                    assert self.wire.command("OFF") == "OK"
        self.wire.after_request = switch_off
        self.wire.accept_at = self.clock() + 1
        code, result = self.run_cli("sync", "--reoffer", "--json", "--wait", "20")
        self.assertEqual((result["items"][0]["state"], result["items"][0]["code"]), ("queued", "transfer_interrupted"), result)
        self.wire.after_request = None
        assert self.wire.command("ON") == "OK"
        self.wire.accept_at = self.clock() + 1
        code, result = self.run_cli("sync", "--json", "--wait", "30")
        self.assertEqual((code, result["summary"]["delivered"]), (0, 1), result)


# ---------------------------------------------------------------------- mDNS

def dns_answer(query, records, ident=None):
    ident = struct.unpack(">H", query[:2])[0] if ident is None else ident
    body = b""
    for kind, name, value in records:
        rdata = {
            "A": lambda v: socket.inet_aton(v),
            "AAAA": lambda v: socket.inet_pton(socket.AF_INET6, v),
            "PTR": lambda v: lan.dns_name(v),
            "SRV": lambda v: struct.pack(">HHH", 0, 0, v[1]) + lan.dns_name(v[0]),
            "TXT": lambda v: b"".join(bytes([len(s)]) + s.encode() for s in v),
        }[kind](value)
        code = {"A": 1, "AAAA": 28, "PTR": 12, "SRV": 33, "TXT": 16}[kind]
        body += lan.dns_name(name) + struct.pack(">HHIH", code, 0x8001, 120, len(rdata)) + rdata
    return struct.pack(">HHHHHH", ident, 0x8400, 0, len(records), 0, 0) + body


class Responder:
    def __init__(self, records, *, silent=False):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0)); self.sock.settimeout(3)
        self.address, self.records, self.silent = self.sock.getsockname(), records, silent
        self.queries = []
        self.thread = threading.Thread(target=self.run, daemon=True)

    def run(self):
        try:
            while True:
                data, peer = self.sock.recvfrom(9000)
                self.queries.append((data, peer))
                if not self.silent:
                    # Noise first: wrong ID, then a malformed packet, then the answer.
                    self.sock.sendto(dns_answer(data, self.records, ident=0xdead), peer)
                    self.sock.sendto(b"\x00\x01\x84", peer)
                    self.sock.sendto(dns_answer(data, self.records), peer)
        except OSError:
            return

    def __enter__(self):
        self.thread.start(); return self

    def __exit__(self, *_):
        self.sock.close(); self.thread.join(4)


class MdnsTests(unittest.TestCase):
    ID = "0123456789abcdef"

    def test_resolve_ipv4_only_exact_owner(self):
        host = "paperdiy-%s.local" % self.ID
        records = [("AAAA", host, "fe80::1"), ("A", "paperdiy-ffffffffffffffff.local", "192.0.2.66"), ("A", host, "192.0.2.5")]
        with Responder(records) as responder:
            result = lan.Mdns(responder.address, attempt=0.5).resolve(self.ID, lan.time.monotonic() + 3)
        self.assertEqual(result, ["192.0.2.5"])
        query = responder.queries[0][0]
        self.assertEqual(struct.unpack(">HH", query[-4:]), (1, 0x8001))   # A, QU bit
        self.assertEqual(lan.Mdns().resolve("NOT-HEX", 0), [])

    def test_browse_one_several_none(self):
        service = lan.Mdns.SERVICE
        one = [("PTR", service, "paperdiy-%s.%s" % (self.ID, service)), ("TXT", "paperdiy-%s.%s" % (self.ID, service), ["id=" + self.ID]),
               ("SRV", "paperdiy-%s.%s" % (self.ID, service), ("paperdiy-%s.local" % self.ID, 8443)), ("A", "paperdiy-%s.local" % self.ID, "192.0.2.5")]
        with Responder(one) as responder:
            self.assertEqual(lan.Mdns(responder.address, attempt=0.3).browse(lan.time.monotonic() + 3), [{"deviceId": self.ID, "host": "192.0.2.5"}])
        other = "fedcba9876543210"
        two = one + [("PTR", service, "paperdiy-%s.%s" % (other, service)), ("SRV", "paperdiy-%s.%s" % (other, service), ("paperdiy-%s.local" % other, 8443)),
                     ("A", "paperdiy-%s.local" % other, "192.0.2.6")]
        with Responder(two) as responder:
            self.assertEqual(len(lan.Mdns(responder.address, attempt=0.3).browse(lan.time.monotonic() + 3)), 2)
        # PTR only (no SRV/TXT/A in the answer): ID from the instance name, A by query.
        bare = [("PTR", service, "paperdiy-%s.%s" % (self.ID, service)), ("A", "paperdiy-%s.local" % self.ID, "192.0.2.7")]
        with Responder(bare) as responder:
            self.assertEqual(lan.Mdns(responder.address, attempt=0.3).browse(lan.time.monotonic() + 3), [{"deviceId": self.ID, "host": "192.0.2.7"}])
        with Responder([], silent=True) as responder:
            start = lan.time.monotonic()
            self.assertEqual(lan.Mdns(responder.address, attempt=0.2).browse(start + 3), [])
            self.assertLess(lan.time.monotonic() - start, 1.5)
            self.assertEqual(len(responder.queries), 2)               # two one-shot attempts

    def test_parser_bounds(self):
        query = struct.pack(">H", 7) + b"\0" * 10
        good = dns_answer(query, [("A", "x.local", "192.0.2.1")])
        self.assertEqual(lan.parse_dns(good, 7), [("A", "x.local", "192.0.2.1")])
        self.assertEqual(lan.parse_dns(good, 8), [])
        self.assertEqual(lan.parse_dns(good[:-2], 7), [])
        loop = struct.pack(">HHHHHH", 7, 0x8400, 0, 1, 0, 0) + b"\xc0\x0c" + struct.pack(">HHIH", 1, 1, 1, 4) + b"\1\2\3\4"
        self.assertEqual(lan.parse_dns(loop, 7), [])
        for n in range(len(good)):
            lan.parse_dns(good[:n], 7)
        self.assertIsNone(lan.ipv4("224.0.0.251"))
        self.assertIsNone(lan.ipv4("::1"))

    def test_network_without_multicast_fails_cleanly(self):
        with patch.object(lan.socket.socket, "sendto", side_effect=OSError("network unreachable")):
            self.assertEqual(lan.Mdns(attempt=0.1).browse(lan.time.monotonic() + 1), [])


# ----------------------------------------------------------------- real TLS

class TLSFixture:
    def __init__(self, certificate, key, response=None):
        self.socket = socket.socket()
        try:
            self.socket.bind(("127.0.0.1", 0)); self.socket.listen(1); self.socket.settimeout(3)
            self.port = self.socket.getsockname()[1]
            self.context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            self.context.load_cert_chain(certificate, key)
            self.response = response or b'HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\n{"ok":true}'
            self.received, self.error = b"", None
            self.thread = threading.Thread(target=self.run)
        except BaseException:
            with contextlib.suppress(Exception):
                self.socket.close()
            raise

    def run(self):
        try:
            raw, _ = self.socket.accept()
            with raw, self.context.wrap_socket(raw, server_side=True) as stream:
                stream.settimeout(3)
                while b"\r\n\r\n" not in self.received:
                    block = stream.recv(8192)
                    if not block:
                        return
                    self.received += block
                head, body = self.received.split(b"\r\n\r\n", 1)
                length = next((int(line.split(b":", 1)[1]) for line in head.split(b"\r\n")[1:]
                               if line.lower().startswith(b"content-length:")), 0)
                while len(body) < length:
                    block = stream.recv(min(8192, length - len(body)))
                    if not block:
                        return
                    body += block; self.received += block
                stream.sendall(self.response)
        except Exception as error:
            self.error = error
        finally:
            self.socket.close()

    def __enter__(self):
        try:
            self.thread.start()
        except BaseException:
            with contextlib.suppress(Exception):
                self.socket.close()
            raise
        return self

    def __exit__(self, *_):
        self.thread.join(4)
        assert not self.thread.is_alive(), "loopback fixture exceeded deadline"


class TLSTests(unittest.TestCase):
    CREDENTIALS = ("b" * 16, "c" * 64)

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.cert, cls.key = Path(cls.tmp.name) / "cert.pem", Path(cls.tmp.name) / "key.pem"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1", "-subj", "/CN=synthetic-paperdiy-test",
                        "-keyout", str(cls.key), "-out", str(cls.cert)], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        cls.pin = hashlib.sha256(ssl.PEM_cert_to_DER_cert(cls.cert.read_text())).hexdigest()

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def request(self, server, path="/lan/v1/capabilities", form=None, pin=None, credentials=CREDENTIALS):
        return lan.HTTPS().request(("127.0.0.1", server.port), "GET" if form is None else "POST", path, form,
                                   pin=self.pin if pin is None else pin, credentials=credentials, deadline=lan.time.monotonic() + 5)

    def test_pinned_tls_sends_authorization_only_after_match(self):
        with TLSFixture(self.cert, self.key) as server:
            result, observed = self.request(server)
        self.assertEqual((result, observed), ({"ok": True}, self.pin))
        self.assertIn(b"Authorization: Bearer " + b"b" * 16 + b":" + b"c" * 64, server.received)
        self.assertIsNone(server.error)

    def test_wrong_certificate_sends_zero_bytes(self):
        with TLSFixture(self.cert, self.key) as server:
            with self.assertRaisesRegex(lan.Failure, "pin_mismatch"):
                self.request(server, pin="a" * 64)
        self.assertEqual(server.received, b"")

    def test_only_the_identity_probe_runs_unpinned(self):
        with patch.object(lan.socket, "create_connection") as connect:
            for path, credentials, form in (("/lan/v2/batch/offer", None, {"a": "b"}), ("/lan/v2/hello", self.CREDENTIALS, None),
                                            ("/lan/v1/capabilities", None, None)):
                with self.assertRaisesRegex(lan.Failure, "invalid_request"):
                    lan.HTTPS().request(("127.0.0.1", 1), "GET", path, form, pin=None, credentials=credentials, deadline=lan.time.monotonic() + 1)
            connect.assert_not_called()
        with TLSFixture(self.cert, self.key) as server:
            result, observed = lan.HTTPS().request(("127.0.0.1", server.port), "GET", "/lan/v2/hello", pin=None, deadline=lan.time.monotonic() + 5)
        self.assertEqual(observed, self.pin)
        self.assertNotIn(b"Authorization", server.received)

    def test_worst_case_chunk_fits_shared_bound(self):
        header = (Path(__file__).resolve().parents[1] / "src/bounded_http_request.h").read_text()
        self.assertEqual(lan.REQUEST_BODY_MAX, int(re.search(r"MAX_BODY\s*=\s*(\d+)", header)[1]))
        for data in (b"\xff" * 4096, (b"\xfb\xef\xbe" * 1366)[:4096]):
            with TLSFixture(self.cert, self.key) as server:
                form = {"requestId": "d" * 32, "offset": 67104768, "data": base64.b64encode(data).decode()}
                self.request(server, "/lan/v1/uploads/chunk", form)
            encoded = server.received.split(b"\r\n\r\n", 1)[1]
            self.assertLessEqual(len(encoded), lan.REQUEST_BODY_MAX)
            self.assertEqual(base64.b64decode(lan.urllib.parse.parse_qs(encoded.decode())["data"][0]), data)
        with patch.object(lan.socket, "create_connection") as connect:
            with self.assertRaisesRegex(lan.Failure, "request_too_large"):
                lan.HTTPS().request(("127.0.0.1", 1), "POST", "/lan/v1/uploads/chunk", {"data": "x" * lan.REQUEST_BODY_MAX},
                                    pin=self.pin, credentials=self.CREDENTIALS, deadline=lan.time.monotonic() + 1)
            connect.assert_not_called()

    def test_redirect_oversize_and_duplicate_headers_rejected(self):
        for response in (b"HTTP/1.1 302 Found\r\nContent-Length: 2\r\n\r\n{}", b"HTTP/1.1 200 OK\r\nContent-Length: 999999\r\n\r\n",
                         b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 2\r\n\r\n{}"):
            with self.subTest(response=response[:20]), TLSFixture(self.cert, self.key, response) as server:
                with self.assertRaisesRegex(lan.Failure, "invalid_response"):
                    self.request(server)

    def test_remote_error_code_is_bounded(self):
        response = b'HTTP/1.1 409 Conflict\r\nContent-Length: 24\r\n\r\n{"code":"not_accepted"}\n'
        with TLSFixture(self.cert, self.key, response) as server:
            with self.assertRaises(lan.RemoteFailure) as caught:
                self.request(server)
        self.assertEqual((caught.exception.code, caught.exception.status), ("not_accepted", 409))


if __name__ == "__main__":
    unittest.main()
