#!/usr/bin/env python3
"""Synthetic protocol/persistence and real loopback TLS tests; no device needed."""
import base64
import contextlib
import hashlib
import importlib.util
import io
import json
import os
import re
from pathlib import Path
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("paperdiy_lan", Path(__file__).resolve().parents[1] / "tools" / "paperdiy_lan.py")
lan = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(lan)
DEVICE = {"deviceId": "synthetic-device", "host": "127.0.0.1", "port": 8443,
          "certSha256": "a" * 64, "clientId": "b" * 16, "token": "c" * 64, "pushRevision": 0}


class Protocol:
    def __init__(self):
        self.revision, self.items, self.receipts = 0, {}, {}
        self.lost, self.calls, self.offline = {}, [], False
        self.starts, self.commits, self.checking = 0, 0, 0
        self.waiting = None
        self.cancelled = set()

    def request(self, method, path, form=None, auth=True):
        operation, _, query = path.removeprefix(lan.API).partition("?")
        self.calls.append((method, operation))
        if self.offline:
            raise lan.Failure("offline")
        if operation == "capabilities":
            result = {"v": 1, "deviceId": DEVICE["deviceId"], "pushRevision": self.revision,
                      "epoch": "d" * 32, "limits": {"chunkBytes": 4096}}
        elif operation == "pair":
            assert not auth and form["secret"] == "e" * 32
            result = {key: DEVICE[key] for key in ("deviceId", "clientId", "token", "pushRevision")}
        elif operation == "uploads/start":
            request_id = form["requestId"]
            if request_id in self.receipts:
                result = {"state": "complete", "receipt": self.receipts[request_id]}
            else:
                if request_id not in self.items:
                    if form["kind"] == "push" and int(form["baseRevision"]) != self.revision:
                        raise lan.RemoteFailure("revision_conflict", 409)
                    self.starts += 1
                    self.items[request_id] = {**form, "size": int(form["size"]), "body": bytearray()}
                result = self.status(request_id)
        elif operation in ("receipts", "uploads/status"):
            request_id = query.partition("=")[2]
            if operation == "uploads/status" and self.waiting == request_id:
                self.waiting = None
                return self.request("POST", lan.API + "uploads/commit", {"requestId": request_id})
            if request_id in self.receipts:
                result = {"state": "complete", "receipt": self.receipts[request_id]}
            elif operation == "receipts" or request_id not in self.items:
                raise lan.RemoteFailure("unknown_receipt", 404)
            else:
                result = self.status(request_id)
        elif operation == "uploads/chunk":
            item = self.items[form["requestId"]]
            assert int(form["offset"]) == len(item["body"])
            data = base64.b64decode(form["data"], validate=True)
            assert len(data) <= 4096
            item["body"].extend(data)
            result = self.status(form["requestId"])
        elif operation == "uploads/commit":
            request_id = form["requestId"]
            if self.checking:
                self.checking -= 1
                self.waiting = request_id
                result = self.status(request_id); result["state"] = "checking"
            else:
                item = self.items[request_id]
                assert len(item["body"]) == item["size"]
                assert hashlib.sha256(item["body"]).hexdigest() == item["sha256"]
                self.commits += 1
                if item["kind"] == "push":
                    self.revision += 1
                receipt = {key: item[key] for key in ("requestId", "fingerprint", "kind", "size", "sha256", "name")}
                if item["kind"] == "push":
                    receipt["revision"] = self.revision
                self.receipts[request_id] = receipt
                result = {"state": "complete", "receipt": receipt}
        elif operation == "uploads/cancel":
            request_id = form["requestId"]
            if request_id in self.receipts:
                result = {"state": "complete", "receipt": self.receipts[request_id]}
            else:
                self.cancelled.add(request_id); self.items.pop(request_id, None)
                result = {"state": "cancelled", "requestId": request_id}
        else:
            raise AssertionError(operation)
        if self.lost.get(operation, 0):
            self.lost[operation] -= 1
            raise lan.Failure("connection_lost", sent=True)
        return result

    def status(self, request_id):
        item = self.items[request_id]
        return {"state": "receiving", "requestId": request_id, "fingerprint": item["fingerprint"],
                "size": item["size"], "offset": len(item["body"])}


class ClientTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.state = self.root / "state"
        self.source = self.root / "synthetic.md"
        self.source.write_bytes(("Synthetic **Markdown** $x^2$.\n" * 200).encode())
        with lan.Store(self.state) as store:
            store.db["devices"]["test"] = dict(DEVICE); store.save()
        self.protocol = Protocol()

    def tearDown(self):
        self.tmp.cleanup()

    def test_canonical_fingerprint_vector(self):
        item = dict(kind="push", name="Push.md", title="Synthetic", size=3,
                    sha256="0" * 64, baseRevision=0)
        self.assertEqual(lan.fingerprint(item), "7c5dc380b37e359158a305fc6aff88f3397a4c8a7d1b8a2d00c89c3fd0016d74")

    def enqueue(self, kind="push", base=None):
        with lan.Store(self.state) as store:
            return dict(store.enqueue("test", self.source, kind, "Synthetic", base))

    def send(self):
        with lan.Store(self.state) as store:
            item = store.db["items"][-1]
            lan.Sender(store, "test", transport=self.protocol).send(item)
            return dict(item)

    def test_normal_sender_archive_and_permissions(self):
        initial = self.enqueue()
        self.source.write_text("Changed original")
        result = self.send()
        self.assertEqual(result["state"], "delivered")
        self.assertEqual(self.protocol.commits, 1)
        self.assertEqual(result["sha256"], initial["sha256"])
        for file in self.state.rglob("*"):
            self.assertEqual(file.stat().st_mode & 0o077, 0)
        self.assertEqual(self.send()["state"], "delivered")
        self.assertEqual(self.protocol.commits, 1)

    def test_lost_start_chunk_commit_resolve_same_id(self):
        for operation in ("uploads/start", "uploads/chunk", "uploads/commit"):
            with self.subTest(operation=operation):
                self.protocol = Protocol()
                with lan.Store(self.state) as store:
                    store.db["items"] = []; store.db["devices"]["test"]["pushRevision"] = 0; store.save()
                item = self.enqueue()
                self.protocol.lost[operation] = 1
                self.assertEqual(self.send()["state"], "uncertain")
                result = self.send()
                self.assertEqual(result["state"], "delivered")
                self.assertEqual(result["requestId"], item["requestId"])
                self.assertEqual(self.protocol.starts, 1)
                self.assertEqual(self.protocol.commits, 1)
                self.assertEqual(bytes(self.protocol.items[item["requestId"]]["body"]), self.source.read_bytes())

    def test_offline_is_queued_without_mutation(self):
        self.enqueue(); self.protocol.offline = True
        result = self.send()
        self.assertEqual(result["state"], "queued")
        self.assertFalse(result["attempted"])
        self.assertEqual(self.protocol.starts, 0)

    def test_unknown_after_reboot_never_restarts(self):
        self.enqueue(); self.protocol.lost["uploads/commit"] = 1
        self.assertEqual(self.send()["state"], "uncertain")
        self.protocol.items.clear(); self.protocol.receipts.clear()
        self.assertEqual(self.send()["code"], "receipt_unknown")
        self.assertEqual(self.protocol.starts, 1)
        with lan.Store(self.state) as store:
            with self.assertRaisesRegex(lan.Failure, "resolve_previous_push"):
                store.enqueue("test", self.source, "push")

    def test_offline_baseline_not_automatically_rebased(self):
        self.enqueue(); self.protocol.revision = 4
        result = self.send()
        self.assertEqual(result["state"], "blocked")
        self.assertEqual(result["baseRevision"], 0)
        self.assertEqual(result["code"], "revision_conflict")
        self.assertEqual(self.protocol.starts, 0)

    def test_unknown_baseline_remains_blocked_after_capabilities(self):
        with lan.Store(self.state) as store:
            store.db["devices"]["test"].pop("pushRevision"); store.save()
        self.enqueue()
        result = self.send()
        self.assertEqual(result["code"], "base_revision_required")
        self.assertIsNone(result["baseRevision"])
        self.assertEqual(self.protocol.starts, 0)

    def test_receipt_precedes_new_cas_and_revision_does_not_regress(self):
        self.enqueue(); self.protocol.lost["uploads/commit"] = 1
        self.send(); self.protocol.revision = 9
        self.assertEqual(self.send()["state"], "delivered")
        with lan.Store(self.state) as store:
            self.assertEqual(store.device("test")["pushRevision"], 9)

    def test_latest_never_attempted_push_archive_preserved(self):
        first = self.enqueue()
        self.source.write_text("A newer synthetic revision")
        second = self.enqueue()
        with lan.Store(self.state) as store:
            self.assertEqual(len(store.db["items"]), 2)
            self.assertEqual(store.db["items"][0]["state"], "superseded")
            self.assertNotEqual(first["requestId"], second["requestId"])
            self.assertTrue(store.body(first).exists())

    def test_size_and_outbox_archive_quotas_refuse_without_deleting(self):
        self.source.write_bytes(b"x" * (lan.PUSH_MAX + 1))
        with lan.Store(self.state) as store:
            with self.assertRaisesRegex(lan.Failure, "size_limit"):
                store.enqueue("test", self.source, "push")
            self.assertEqual(list(store.objects.iterdir()), [])
        self.source.write_bytes(b"synthetic")
        with patch.object(lan, "OUTBOX_FILES", 1):
            self.enqueue("file")
            with lan.Store(self.state) as store:
                with self.assertRaisesRegex(lan.Failure, "outbox_full"):
                    store.enqueue("test", self.source, "file")
                self.assertEqual(len(list(store.objects.iterdir())), 1)
        with patch.object(lan, "ARCHIVE_BYTES", 10):
            with lan.Store(self.state) as store:
                with self.assertRaisesRegex(lan.Failure, "archive_full"):
                    store.enqueue("test", self.source, "push")
                self.assertEqual(len(list(store.objects.iterdir())), 1)

    def test_bad_receipt_never_delivered(self):
        item = self.enqueue(); self.protocol.lost["uploads/commit"] = 1
        self.send(); self.protocol.receipts[item["requestId"]]["sha256"] = "0" * 64
        self.assertEqual(self.send()["code"], "invalid_receipt")

    def test_checking_is_polled_until_complete_with_same_request(self):
        self.enqueue(); self.protocol.checking = 1
        self.assertEqual(self.send()["state"], "delivered")
        self.assertEqual(self.protocol.starts, 1)
        self.assertEqual(self.protocol.commits, 1)

    def test_pair_secret_not_saved_and_lost_response_truthful(self):
        payload = {"v": 1, **{k: DEVICE[k] for k in ("deviceId", "host", "port", "certSha256")}, "secret": "e" * 32}
        with lan.Store(self.state) as store:
            self.assertEqual(lan.pair(store, "paired", payload, "Synthetic", transport=self.protocol)["state"], "paired")
            self.assertNotIn(payload["secret"], (self.state / "state.json").read_text())
            self.protocol.lost["pair"] = 1
            with self.assertRaisesRegex(lan.Failure, "pair_uncertain"):
                lan.pair(store, "unknown", payload, "Synthetic", transport=self.protocol)
            self.assertNotIn("unknown", store.db["devices"])

    def test_pair_readback_failure_is_uncertain_and_never_retried(self):
        payload = {"v": 1, **{k: DEVICE[k] for k in ("deviceId", "host", "port", "certSha256")}, "secret": "e" * 32}
        saved = []
        def saved_then_failed(*args, **kwargs):
            saved.append("orphaned client")
            raise lan.RemoteFailure("pairing_not_saved", 500)
        with lan.Store(self.state) as store, patch.object(self.protocol, "request", side_effect=saved_then_failed):
            with self.assertRaisesRegex(lan.Failure, "pair_uncertain"):
                lan.pair(store, "unknown", payload, "Synthetic", transport=self.protocol)
            self.assertEqual(saved, ["orphaned client"])
            self.assertNotIn("unknown", store.db["devices"])

    def test_physical_inventory_and_explicit_revoke_without_local_state(self):
        payload = {"v": 1, **{k: DEVICE[k] for k in ("deviceId", "host", "port", "certSha256")}, "secret": "e" * 32}
        payload_file = self.root / "physical.json"; payload_file.write_text(json.dumps(payload))
        orphan_id = "f" * 16
        inventory = {"deviceId": DEVICE["deviceId"], "clients": [{"clientId": orphan_id, "label": "Synthetic orphan"}]}
        missing_state = self.root / "not-created"
        output = io.StringIO()
        with patch.object(lan, "PinnedHTTPS", return_value=self.protocol), patch.object(self.protocol, "request", return_value=inventory) as request, contextlib.redirect_stdout(output):
            self.assertEqual(lan.main(["--state-dir", str(missing_state), "clients", "--payload", str(payload_file)]), 0)
            request.assert_called_once_with("POST", lan.API + "clients", {"secret": payload["secret"]}, auth=False)
        self.assertEqual(json.loads(output.getvalue())["clients"], inventory["clients"])
        self.assertNotIn(payload["secret"], output.getvalue()); self.assertFalse(missing_state.exists())
        output = io.StringIO()
        with patch.object(lan, "PinnedHTTPS", return_value=self.protocol), patch.object(self.protocol, "request", return_value={"revoked": True}) as request, contextlib.redirect_stdout(output):
            self.assertEqual(lan.main(["--state-dir", str(missing_state), "revoke-client", "--payload", str(payload_file), "--client-id", orphan_id]), 0)
            request.assert_called_once_with("POST", lan.API + "clients/revoke", {"secret": payload["secret"], "clientId": orphan_id}, auth=False)
        self.assertEqual(json.loads(output.getvalue()), {"state": "revoked", "clientId": orphan_id})
        self.assertFalse(missing_state.exists())
        with patch.object(self.protocol, "request", side_effect=lan.Failure("connection_lost", sent=True)):
            with self.assertRaisesRegex(lan.Failure, "revoke_uncertain"):
                lan.physical_revoke(payload, orphan_id, transport=self.protocol)

    def test_physical_inventory_rejects_duplicates_credentials_and_unbounded_labels(self):
        payload = {"v": 1, **{k: DEVICE[k] for k in ("deviceId", "host", "port", "certSha256")}, "secret": "e" * 32}
        entry = {"clientId": "f" * 16, "label": "Synthetic orphan"}
        for entries in ([entry] * 2, [{**entry, "token": "private"}], [{**entry, "label": "x" * 49}], [entry] * 5):
            with self.subTest(entries=entries), patch.object(self.protocol, "request", return_value={"deviceId": DEVICE["deviceId"], "clients": entries}):
                with self.assertRaises(lan.Failure):
                    lan.physical_clients(payload, transport=self.protocol)

    def test_mutation_attempt_persisted_before_network_and_reload(self):
        self.enqueue()
        original = self.protocol.request
        def request(method, path, form=None, auth=True):
            if path.endswith("uploads/start"):
                manifest = json.loads((self.state / "state.json").read_text())
                self.assertTrue(manifest["items"][0]["attempted"])
                self.assertEqual(manifest["items"][0]["state"], "uncertain")
            return original(method, path, form, auth)
        self.protocol.request = request
        self.assertEqual(self.send()["state"], "delivered")

    def test_pair_label_bound_matches_device_48_utf8_bytes(self):
        payload = {"v": 1, **{k: DEVICE[k] for k in ("deviceId", "host", "port", "certSha256")}, "secret": "e" * 32}
        with lan.Store(self.state) as store:
            self.assertEqual(lan.pair(store, "label48", payload, "字" * 16, transport=self.protocol)["state"], "paired")
            with self.assertRaisesRegex(lan.Failure, "invalid_label"):
                lan.pair(store, "label49", payload, "x" * 49, transport=self.protocol)
            with self.assertRaisesRegex(lan.Failure, "invalid_label"):
                lan.pair(store, "control", payload, "bad\u0085label", transport=self.protocol)
            with self.assertRaisesRegex(lan.Failure, "invalid_label"):
                lan.pair(store, "empty", payload, "", transport=self.protocol)

    def test_cli_errors_redact_private_path_and_credential(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            code = lan.main(["--state-dir", str(self.state), "send", "--device", "test", str(self.root / "private-missing-file.md"), "--queue-only"])
        self.assertEqual(code, 2)
        self.assertNotIn(str(self.root), output.getvalue())
        self.assertNotIn(DEVICE["token"], output.getvalue())
        self.assertEqual(json.loads(output.getvalue())["code"], "local_state_error")

    def cancel_cli(self, request_id):
        output = io.StringIO()
        with patch.object(lan, "PinnedHTTPS", return_value=self.protocol), contextlib.redirect_stdout(output):
            lan.main(["--state-dir", str(self.state), "cancel", "--device", "test", "--request-id", request_id])
        return json.loads(output.getvalue())["items"][0]

    def test_explicit_cancel_and_lost_cancel_do_not_delete_archive(self):
        item = self.enqueue()
        self.assertEqual(self.cancel_cli(item["requestId"])["state"], "cancelled")
        self.assertEqual(self.protocol.calls, [])
        item = self.enqueue(); self.protocol.lost["uploads/start"] = 1
        self.assertEqual(self.send()["state"], "uncertain")
        self.protocol.lost["uploads/cancel"] = 1
        self.assertEqual(self.cancel_cli(item["requestId"])["state"], "uncertain")
        self.assertEqual(self.cancel_cli(item["requestId"])["state"], "cancelled")
        with lan.Store(self.state) as store:
            self.assertTrue(store.body(item).exists())

    def test_cancel_after_commit_reports_actual_delivery(self):
        item = self.enqueue(); self.protocol.lost["uploads/commit"] = 1
        self.send()
        self.assertEqual(self.cancel_cli(item["requestId"])["state"], "delivered")
        self.assertEqual(self.protocol.commits, 1)


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
                assert length <= lan.REQUEST_BODY_MAX
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


class TLSFixtureTests(unittest.TestCase):
    def test_subtest_setup_failure_does_not_read_unassigned_server(self):
        for method in ("test_worst_case_percent_encoded_4096_byte_chunks_fit_shared_bound",
                       "test_physical_recovery_checks_pin_before_sending_secret"):
            case = TLSTests(method)
            case.cert, case.key, case.pin = "synthetic-cert", "synthetic-key", "a" * 64
            result = unittest.TestResult()
            with patch(__name__ + ".TLSFixture", side_effect=PermissionError("synthetic bind denied")):
                case.run(result)
            self.assertEqual(len(result.errors), 2)
            self.assertFalse(result.failures)
            for _, error in result.errors:
                self.assertIn("PermissionError: synthetic bind denied", error)
                self.assertNotIn("UnboundLocalError", error)

    def test_setup_failures_close_socket_and_preserve_first_error(self):
        for stage in ("bind", "listen", "certificate", "thread_start"):
            with self.subTest(stage=stage), patch.object(socket, "socket") as make_socket, \
                    patch.object(ssl, "SSLContext") as context, patch.object(threading, "Thread") as thread:
                connection = make_socket.return_value
                connection.getsockname.return_value = ("127.0.0.1", 12345)
                failure = PermissionError("synthetic setup failure")
                if stage in ("bind", "listen"):
                    getattr(connection, stage).side_effect = failure
                elif stage == "certificate":
                    context.return_value.load_cert_chain.side_effect = failure
                else:
                    thread.return_value.start.side_effect = failure
                # Cleanup must not replace the setup error, even if it also fails.
                connection.close.side_effect = OSError("synthetic cleanup failure")
                with self.assertRaises(PermissionError) as caught:
                    with TLSFixture("synthetic-cert", "synthetic-key"):
                        self.fail("failed setup must not enter the fixture body")
                self.assertIs(caught.exception, failure)
                connection.bind.assert_called_once_with(("127.0.0.1", 0))
                connection.close.assert_called_once_with()


class TLSTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.cert, cls.key = Path(cls.tmp.name) / "cert.pem", Path(cls.tmp.name) / "key.pem"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1", "-subj", "/CN=synthetic-paperdiy-test", "-keyout", str(cls.key), "-out", str(cls.cert)], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        cls.pin = hashlib.sha256(ssl.PEM_cert_to_DER_cert(cls.cert.read_text())).hexdigest()

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_real_pinned_tls_sends_authorization_only_after_match(self):
        with TLSFixture(self.cert, self.key) as server:
            client = lan.PinnedHTTPS({**DEVICE, "port": server.port, "certSha256": self.pin})
            self.assertEqual(client.request("GET", lan.API + "capabilities"), {"ok": True})
        self.assertIn(("Authorization: Bearer " + DEVICE["clientId"] + ":" + DEVICE["token"]).encode(), server.received)
        self.assertIsNone(server.error)

    def test_wrong_certificate_sends_zero_http_or_credentials(self):
        with TLSFixture(self.cert, self.key) as server:
            client = lan.PinnedHTTPS({**DEVICE, "port": server.port})
            with self.assertRaisesRegex(lan.Failure, "certificate_mismatch"):
                client.request("POST", lan.API + "pair", {"secret": "e" * 32}, auth=False)
        self.assertEqual(server.received, b"")

    def test_worst_case_percent_encoded_4096_byte_chunks_fit_shared_bound(self):
        request_header = (Path(__file__).resolve().parents[1]/"src/bounded_http_request.h").read_text()
        self.assertEqual(lan.REQUEST_BODY_MAX, int(re.search(r'MAX_BODY\s*=\s*(\d+)', request_header)[1]))
        for data in (b"\xff" * 4096, (b"\xfb\xef\xbe" * 1366)[:4096]):
            with self.subTest(first=data[0]):
                with TLSFixture(self.cert, self.key) as server:
                    form = {"requestId": "d" * 32, "offset": 67104768, "data": base64.b64encode(data).decode()}
                    client = lan.PinnedHTTPS({**DEVICE, "port": server.port, "certSha256": self.pin})
                    self.assertEqual(client.request("POST", lan.API + "uploads/chunk", form), {"ok": True})
                encoded = server.received.split(b"\r\n\r\n", 1)[1]
                self.assertGreater(len(encoded), 8192)
                self.assertLessEqual(len(encoded), lan.REQUEST_BODY_MAX)
                decoded = lan.urllib.parse.parse_qs(encoded.decode(), strict_parsing=True)
                self.assertEqual(base64.b64decode(decoded["data"][0], validate=True), data)
                self.assertIsNone(server.error)
        with patch.object(lan.socket, "create_connection") as connect:
            with self.assertRaisesRegex(lan.Failure, "request_too_large"):
                lan.PinnedHTTPS(DEVICE).request("POST", lan.API + "uploads/chunk", {"data": "x" * lan.REQUEST_BODY_MAX})
            connect.assert_not_called()

    def test_physical_recovery_checks_pin_before_sending_secret(self):
        for operation in ("clients", "revoke"):
            with self.subTest(operation=operation):
                with TLSFixture(self.cert, self.key) as server:
                    payload = {"v": 1, **{k: DEVICE[k] for k in ("deviceId", "host", "certSha256")}, "port": server.port, "secret": "e" * 32}
                    with self.assertRaisesRegex(lan.Failure, "certificate_mismatch"):
                        if operation == "clients":
                            lan.physical_clients(payload)
                        else:
                            lan.physical_revoke(payload, "f" * 16)
                self.assertEqual(server.received, b"")

    def test_redirect_and_oversized_response_rejected(self):
        for response, code in [(b"HTTP/1.1 302 Found\r\nContent-Length: 2\r\nLocation: http://127.0.0.1/\r\n\r\n{}", "redirect_refused"), (b"HTTP/1.1 200 OK\r\nContent-Length: 999999\r\n\r\n", "response_too_large"), (b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 2\r\n\r\n{}", "invalid_response")]:
            with self.subTest(code=code), TLSFixture(self.cert, self.key, response) as server:
                client = lan.PinnedHTTPS({**DEVICE, "port": server.port, "certSha256": self.pin})
                with self.assertRaisesRegex(lan.Failure, code):
                    client.request("GET", lan.API + "capabilities")


if __name__ == "__main__":
    unittest.main()
