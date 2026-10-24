#!/usr/bin/env python3
"""Foreground-only PaperDIY LAN sender; Python standard library, HTTPS pinning."""
import argparse
import base64
import contextlib
import hashlib
import hmac
import ipaddress
import json
import os
from pathlib import Path
import re
import secrets
import socket
import ssl
import struct
import sys
import time
import urllib.parse

PUSH_MAX = 16384
FILE_LIMITS = {".txt": 4 * 1024 * 1024, ".epub": 64 * 1024 * 1024,
               ".md": 1024 * 1024, ".markdown": 1024 * 1024}
ARCHIVE_COUNT, ARCHIVE_BYTES = 256, 512 * 1024 * 1024
OUTBOX_FILES, OUTBOX_BYTES = 16, 128 * 1024 * 1024
RESPONSE_MAX, HEADER_MAX, CHUNK_MAX = 16384, 8192, 4096
# Matches BoundedHttpRequest::MAX_BODY, including worst-case percent-encoded
# base64 for a complete 4096-byte binary chunk.
REQUEST_BODY_MAX = 17000
API = "/lan/v1/"


class Failure(Exception):
    def __init__(self, code, *, sent=False):
        self.code, self.sent = code, sent
        super().__init__(code)


class RemoteFailure(Failure):
    def __init__(self, code, status):
        super().__init__(code)
        self.status = status


def hex_value(value, length):
    return isinstance(value, str) and bool(re.fullmatch(r"[0-9a-f]{%d}" % length, value))


def integer(value, maximum=0xffffffff):
    return type(value) is int and 0 <= value <= maximum


def clean_label(value, maximum=96):
    if not isinstance(value, str) or any(ord(c) < 32 or 127 <= ord(c) <= 159 for c in value):
        raise Failure("invalid_label")
    try:
        if len(value.encode("utf-8")) > maximum:
            raise Failure("invalid_label")
    except UnicodeError:
        raise Failure("invalid_label") from None
    return value


def clean_name(value):
    clean_label(value)
    if not value or value.startswith(".") or value.endswith((".", " ")) or any(c in value for c in '/\\:<>"|?*'):
        raise Failure("invalid_name")
    return value


def endpoint(host, port):
    try:
        ipaddress.ip_address(host)
    except ValueError:
        raise Failure("literal_ip_required") from None
    if not integer(port, 65535) or not port:
        raise Failure("invalid_port")


def fingerprint(item):
    values = [item["kind"], item["name"], item.get("title", ""), str(item["size"]),
              item["sha256"], str(item["baseRevision"]) if item["baseRevision"] is not None else ""]
    digest = hashlib.sha256()
    for value in values:
        encoded = value.encode("utf-8")
        digest.update(struct.pack(">I", len(encoded)))
        digest.update(encoded)
    return digest.hexdigest()


def json_bytes(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"), allow_nan=False).encode("utf-8")


def parse_json(data):
    def pairs(entries):
        result = {}
        for key, value in entries:
            if key in result:
                raise Failure("invalid_response")
            result[key] = value
        return result
    try:
        value = json.loads(data.decode("utf-8"), object_pairs_hook=pairs,
                           parse_constant=lambda _: (_ for _ in ()).throw(Failure("invalid_response")))
    except (ValueError, UnicodeError, RecursionError):
        raise Failure("invalid_response") from None
    if not isinstance(value, dict):
        raise Failure("invalid_response")
    return value


class PinnedHTTPS:
    """Exact DER certificate is the trust anchor; authenticate before HTTP bytes."""
    def __init__(self, device, *, timeout=3, deadline=None):
        endpoint(device["host"], device["port"])
        if not hex_value(device.get("certSha256"), 64):
            raise Failure("invalid_pin")
        self.device, self.timeout, self.deadline = device, timeout, deadline

    def request(self, method, path, form=None, *, auth=True):
        deadline = min(self.deadline or float("inf"), time.monotonic() + self.timeout)
        def remaining():
            left = deadline - time.monotonic()
            if left <= 0:
                raise Failure("budget_exhausted")
            return left
        if not path.startswith(API) or "\r" in path or "\n" in path:
            raise Failure("invalid_request")
        body = urllib.parse.urlencode(form or {}).encode("ascii") if form is not None else b""
        if len(body) > REQUEST_BODY_MAX:
            raise Failure("request_too_large")
        device = self.device
        host_header = "[%s]" % device["host"] if ":" in device["host"] else device["host"]
        headers = ["%s %s HTTP/1.1" % (method, path),
                   "Host: %s:%d" % (host_header, device["port"]),
                   "Connection: close", "Accept: application/json", "Content-Length: %d" % len(body)]
        if form is not None:
            headers.append("Content-Type: application/x-www-form-urlencoded")
        if auth:
            if not hex_value(device.get("clientId"), 16) or not hex_value(device.get("token"), 64):
                raise Failure("not_paired")
            headers.append("Authorization: Bearer %s:%s" % (device["clientId"], device["token"]))
        packet = ("\r\n".join(headers) + "\r\n\r\n").encode("ascii") + body
        sent = False
        try:
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
            context.minimum_version = ssl.TLSVersion.TLSv1_2
            # CA/hostname verification is replaced by an explicit out-of-band
            # exact-certificate trust anchor. No HTTP/auth bytes precede pinning.
            context.check_hostname = False
            context.verify_mode = ssl.CERT_NONE
            with socket.create_connection((device["host"], device["port"]), timeout=remaining()) as raw:
                raw.settimeout(remaining())
                with context.wrap_socket(raw, server_hostname=device["host"]) as stream:
                    actual = hashlib.sha256(stream.getpeercert(binary_form=True)).hexdigest()
                    if not hmac.compare_digest(actual, device["certSha256"]):
                        raise Failure("certificate_mismatch")
                    stream.settimeout(remaining())
                    sent = True
                    stream.sendall(packet)
                    data = b""
                    while b"\r\n\r\n" not in data:
                        stream.settimeout(remaining())
                        block = stream.recv(min(1024, HEADER_MAX + 4 - len(data)))
                        if not block:
                            raise Failure("connection_lost", sent=True)
                        data += block
                        if len(data) >= HEADER_MAX + 4 and b"\r\n\r\n" not in data:
                            raise Failure("response_headers_too_large", sent=True)
                    head, data = data.split(b"\r\n\r\n", 1)
                    if len(head) > HEADER_MAX:
                        raise Failure("response_headers_too_large", sent=True)
                    lines = head.split(b"\r\n")
                    match = re.fullmatch(rb"HTTP/1\.[01] ([0-9]{3})(?: [\x20-\x7e]*)?", lines[0])
                    if not match or len(lines) > 33:
                        raise Failure("invalid_response", sent=True)
                    status = int(match[1])
                    parsed = {}
                    for line in lines[1:]:
                        if b":" not in line:
                            raise Failure("invalid_response", sent=True)
                        key, value = line.split(b":", 1)
                        key, value = key.lower(), value.strip()
                        if key in parsed or not re.fullmatch(rb"[a-z0-9-]+", key):
                            raise Failure("invalid_response", sent=True)
                        parsed[key] = value
                    length = parsed.get(b"content-length", b"")
                    if b"transfer-encoding" in parsed or not re.fullmatch(rb"[0-9]{1,6}", length):
                        raise Failure("invalid_response", sent=True)
                    length = int(length)
                    if length > RESPONSE_MAX or len(data) > length:
                        raise Failure("response_too_large", sent=True)
                    while len(data) < length:
                        stream.settimeout(remaining())
                        block = stream.recv(min(4096, length - len(data)))
                        if not block:
                            raise Failure("connection_lost", sent=True)
                        data += block
                    if 300 <= status < 400:
                        raise Failure("redirect_refused", sent=True)
                    result = parse_json(data)
                    if not 200 <= status < 300:
                        code = result.get("code", "remote_error")
                        if not isinstance(code, str) or not re.fullmatch(r"[a-z_]{1,48}", code):
                            code = "remote_error"
                        raise RemoteFailure(code, status)
                    return result
        except (OSError, ssl.SSLError):
            raise Failure("offline" if not sent else "connection_lost", sent=sent) from None
        except Failure as error:
            error.sent = error.sent or sent
            raise


class Store:
    def __init__(self, directory):
        self.root = Path(directory)
        self.db = {"version": 1, "devices": {}, "items": []}

    def __enter__(self):
        import fcntl
        self.root.mkdir(mode=0o700, parents=True, exist_ok=True)
        if self.root.is_symlink() or self.root.stat().st_uid != os.getuid() or self.root.stat().st_mode & 0o077:
            raise Failure("state_directory_not_private")
        self.objects = self.root / "objects"
        self.objects.mkdir(mode=0o700, exist_ok=True)
        if self.objects.is_symlink() or self.objects.stat().st_mode & 0o077:
            raise Failure("state_directory_not_private")
        flags = os.O_CREAT | os.O_RDWR | getattr(os, "O_NOFOLLOW", 0)
        self.lock = os.open(self.root / "lock", flags, 0o600)
        try:
            fcntl.flock(self.lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            os.close(self.lock)
            raise Failure("sender_busy") from None
        try:
            manifest = self.root / "state.json"
            if manifest.exists():
                if manifest.is_symlink() or manifest.stat().st_size > 512 * 1024 or manifest.stat().st_mode & 0o077:
                    raise Failure("invalid_local_state")
                self.db = parse_json(manifest.read_bytes())
                if self.db.get("version") != 1 or not isinstance(self.db.get("devices"), dict) or not isinstance(self.db.get("items"), list):
                    raise Failure("invalid_local_state")
                if len(self.db["items"]) > ARCHIVE_COUNT:
                    raise Failure("invalid_local_state")
                for item in self.db["items"]:
                    if not isinstance(item, dict) or not hex_value(item.get("requestId"), 32) or item.get("kind") not in ("push", "file") or item.get("state") not in ("queued", "receiving", "checking", "uncertain", "blocked", "delivered", "cancelled", "abandoned", "superseded") or not integer(item.get("size"), 64 * 1024 * 1024) or not integer(item.get("offset"), item["size"]) or type(item.get("attempted")) is not bool or not hex_value(item.get("sha256"), 64):
                        raise Failure("invalid_local_state")
                    clean_label(item.get("name")); clean_label(item.get("title"))
                    if item.get("device") not in self.db["devices"] or (item.get("baseRevision") is not None and not integer(item["baseRevision"])) or item.get("fingerprint") != fingerprint(item):
                        raise Failure("invalid_local_state")
            # Include orphaned objects from interrupted writes in physical
            # quotas. They are never silently deleted or ignored for admission.
            self.object_usage()
            return self
        except BaseException:
            os.close(self.lock)
            raise

    def __exit__(self, *_):
        os.close(self.lock)

    def save(self):
        temporary = self.root / (".state-%s.tmp" % secrets.token_hex(8))
        try:
            with open(os.open(temporary, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600), "wb") as stream:
                stream.write(json_bytes(self.db)); stream.flush(); os.fsync(stream.fileno())
            os.replace(temporary, self.root / "state.json")
            fd = os.open(self.root, os.O_RDONLY)
            try:
                os.fsync(fd)
            finally:
                os.close(fd)
        finally:
            if temporary.exists():
                temporary.unlink()

    def device(self, alias):
        try:
            return self.db["devices"][alias]
        except KeyError:
            raise Failure("unknown_device") from None

    def object_usage(self):
        count, size = 0, 0
        for path in self.objects.iterdir():
            if path.is_symlink() or not path.is_file() or not re.fullmatch(r"[0-9a-f]{32}\.bin", path.name) or path.stat().st_mode & 0o077:
                raise Failure("invalid_local_state")
            count += 1; size += path.stat().st_size
            if count > ARCHIVE_COUNT or size > ARCHIVE_BYTES:
                raise Failure("archive_full")
        return count, size

    def body(self, item):
        if not hex_value(item.get("requestId"), 32):
            raise Failure("invalid_local_state")
        path = self.objects / (item["requestId"] + ".bin")
        if path.is_symlink():
            raise Failure("invalid_local_state")
        return path

    def enqueue(self, alias, source, kind, title="", base=None):
        device = self.device(alias)
        clean_label(title)
        name = "Push.md" if kind == "push" else Path(source).name
        clean_name(name)
        suffix = Path(name).suffix.lower()
        maximum = PUSH_MAX if kind == "push" else FILE_LIMITS.get(suffix)
        if maximum is None:
            raise Failure("unsupported_format")
        records = self.db["items"]
        object_count, object_bytes = self.object_usage()
        pending = [i for i in records if i["device"] == alias and i["state"] not in ("delivered", "cancelled", "abandoned", "superseded")]
        previous = next((i for i in pending if i["kind"] == "push"), None) if kind == "push" else None
        if previous and (previous["state"] != "queued" or previous["attempted"]):
            raise Failure("resolve_previous_push")
        files = [i for i in pending if i["kind"] == "file"]
        if len(records) >= ARCHIVE_COUNT or object_count >= ARCHIVE_COUNT or (kind == "file" and len(files) >= OUTBOX_FILES):
            raise Failure("archive_full" if max(len(records), object_count) >= ARCHIVE_COUNT else "outbox_full")
        if base is None and kind == "push":
            base = device.get("pushRevision")
        if base is not None and not integer(base):
            raise Failure("invalid_revision")
        request_id = secrets.token_hex(16)
        target = self.objects / (request_id + ".bin")
        size, digest = 0, hashlib.sha256()
        manifest_started = False
        try:
            with open(source, "rb") as incoming, open(os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), "wb") as outgoing:
                while True:
                    block = incoming.read(65536)
                    if not block:
                        break
                    size += len(block)
                    if size > maximum:
                        raise Failure("size_limit")
                    digest.update(block); outgoing.write(block)
                outgoing.flush(); os.fsync(outgoing.fileno())
            if not size:
                raise Failure("empty_document")
            if kind == "push":
                try:
                    target.read_bytes().decode("utf-8")
                except UnicodeError:
                    raise Failure("invalid_utf8") from None
            if object_bytes + size > ARCHIVE_BYTES:
                raise Failure("archive_full")
            if kind == "file" and sum(i["size"] for i in files) + size > OUTBOX_BYTES:
                raise Failure("outbox_full")
            item = {"device": alias, "requestId": request_id, "kind": kind, "name": name,
                    "title": title, "size": size, "sha256": digest.hexdigest(),
                    "baseRevision": base if kind == "push" else None,
                    "state": "queued", "attempted": False, "offset": 0, "created": int(time.time())}
            item["fingerprint"] = fingerprint(item)
            if kind == "push" and base is None:
                item["state"], item["code"] = "blocked", "base_revision_required"
            fd = os.open(self.objects, os.O_RDONLY)
            try:
                os.fsync(fd)
            finally:
                os.close(fd)
            if previous:
                previous["state"] = "superseded"
            records.append(item)
            manifest_started = True
            self.save()
            return item
        except BaseException:
            if not manifest_started:
                with contextlib.suppress(OSError):
                    target.unlink()
            raise


class Sender:
    def __init__(self, store, alias, *, timeout=3, budget=120, transport=None):
        self.store, self.alias = store, alias
        self.device = store.device(alias)
        self.deadline = time.monotonic() + budget
        self.transport = transport or PinnedHTTPS(self.device, timeout=timeout, deadline=self.deadline)

    def request(self, method, operation, form=None):
        return self.transport.request(method, API + operation, form)

    def capabilities(self):
        result = self.request("GET", "capabilities")
        if result.get("v") != 1 or result.get("deviceId") != self.device["deviceId"] or not integer(result.get("pushRevision")):
            raise Failure("device_identity_mismatch")
        self.device["pushRevision"] = result["pushRevision"]
        self.store.save()
        return result

    def complete(self, item, response):
        receipt = response.get("receipt", response)
        if not isinstance(receipt, dict) or any(receipt.get(key) != item[key] for key in ("requestId", "fingerprint", "kind", "size", "sha256")):
            raise Failure("invalid_receipt")
        if item["kind"] == "push":
            if not integer(receipt.get("revision")):
                raise Failure("invalid_receipt")
            self.device["pushRevision"] = max(self.device.get("pushRevision", 0), receipt["revision"])
        else:
            try:
                clean_name(receipt.get("name"))
            except Failure:
                raise Failure("invalid_receipt") from None
        item["state"], item["offset"], item["receipt"] = "delivered", item["size"], receipt
        item.pop("code", None)
        self.store.save()

    def receiving(self, item, response):
        if response.get("state") == "complete":
            self.complete(item, response)
            return None
        offset = response.get("offset")
        if response.get("state") not in ("receiving", "checking") or response.get("requestId") != item["requestId"] or response.get("fingerprint") != item["fingerprint"] or response.get("size") != item["size"] or not integer(offset, item["size"]) or (response["state"] == "checking" and offset != item["size"]):
            raise Failure("invalid_upload_status")
        item["offset"], item["state"] = offset, response["state"]
        self.store.save()
        return offset

    def resolve(self, item):
        query = "?requestId=" + item["requestId"]
        try:
            result = self.request("GET", "receipts" + query)
            self.complete(item, result)
            return None
        except RemoteFailure as error:
            if error.status != 404:
                raise
        try:
            return self.receiving(item, self.request("GET", "uploads/status" + query))
        except RemoteFailure as error:
            if error.status == 404:
                raise Failure("receipt_unknown") from None
            raise

    def send(self, item):
        if item["state"] in ("delivered", "cancelled", "abandoned", "superseded"):
            return item
        mutation_started = item["attempted"]
        try:
            caps = self.capabilities()
            limits = caps.get("limits", {})
            if not isinstance(limits, dict):
                raise Failure("invalid_response")
            chunk = limits.get("chunkBytes", CHUNK_MAX)
            if not integer(chunk) or not chunk:
                raise Failure("invalid_response")
            chunk = min(chunk, CHUNK_MAX)
            limit_key = "pushBytes" if item["kind"] == "push" else {".epub": "epubBytes", ".txt": "txtBytes", ".md": "markdownBytes", ".markdown": "markdownBytes"}[Path(item["name"]).suffix.lower()]
            if not integer(limits.get(limit_key, item["size"])) or item["size"] > limits.get(limit_key, item["size"]):
                raise Failure("size_limit")
            if item["kind"] == "push" and item["baseRevision"] is None:
                raise Failure("base_revision_required")
            path = self.store.body(item)
            if path.stat().st_size != item["size"]:
                raise Failure("archive_corrupt")
            digest = hashlib.sha256()
            with path.open("rb") as stream:
                for block in iter(lambda: stream.read(65536), b""):
                    digest.update(block)
            if digest.hexdigest() != item["sha256"]:
                raise Failure("archive_corrupt")
            if item["attempted"]:
                offset = self.resolve(item)
            else:
                form = {key: item[key] for key in ("requestId", "fingerprint", "kind", "name", "title", "size", "sha256")}
                form["baseRevision"] = "" if item["baseRevision"] is None else item["baseRevision"]
                # Persist before any possible mutation bytes. A crash is an
                # uncertain result until its original receipt/status is found.
                item["attempted"], item["state"] = True, "uncertain"
                self.store.save()
                try:
                    response = self.request("POST", "uploads/start", form)
                except Failure as error:
                    if not error.sent and not isinstance(error, RemoteFailure):
                        item["attempted"] = False
                    raise
                mutation_started = True
                offset = self.receiving(item, response)
            if offset is None:
                return item
            with path.open("rb") as stream:
                stream.seek(offset)
                while offset < item["size"]:
                    if time.monotonic() >= self.deadline:
                        raise Failure("budget_exhausted")
                    data = stream.read(min(chunk, item["size"] - offset))
                    if not data:
                        raise Failure("archive_corrupt")
                    response = self.request("POST", "uploads/chunk", {"requestId": item["requestId"], "offset": offset, "data": base64.b64encode(data).decode("ascii")})
                    next_offset = self.receiving(item, response)
                    if next_offset != offset + len(data):
                        raise Failure("invalid_upload_status")
                    offset = next_offset
            response = self.request("GET", "uploads/status?requestId=" + item["requestId"]) if item["state"] == "checking" else self.request("POST", "uploads/commit", {"requestId": item["requestId"]})
            while response.get("state") == "checking":
                self.receiving(item, response)
                if time.monotonic() + 0.05 >= self.deadline:
                    raise Failure("budget_exhausted")
                time.sleep(0.05)
                response = self.request("GET", "uploads/status?requestId=" + item["requestId"])
            self.complete(item, response)
        except Failure as error:
            attempted = item["attempted"] or mutation_started
            item["state"] = "uncertain" if attempted else "queued" if error.code in ("offline", "budget_exhausted") else "blocked"
            item["code"] = error.code
            # Definitive admission rejection proves this new ID was not
            # accepted. Receipt conflicts/unknown outcomes never erase it.
            if isinstance(error, RemoteFailure) and not mutation_started and error.code in ("busy", "revision_conflict", "size_limit", "unauthorized", "storage_unavailable", "library_full"):
                item["attempted"], item["state"] = False, "queued" if error.code == "busy" else "blocked"
            self.store.save()
        return item


def public_item(item):
    result = {key: item[key] for key in ("requestId", "kind", "state", "size", "offset")}
    if "code" in item:
        result["code"] = item["code"]
    return result


def physical_device(payload):
    """Validate the complete physical trust payload without persisting its secret."""
    if payload.get("v") != 1 or not hex_value(payload.get("secret"), 32) or not hex_value(payload.get("certSha256"), 64):
        raise Failure("invalid_pairing_payload")
    if not isinstance(payload.get("deviceId"), str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", payload["deviceId"]):
        raise Failure("invalid_pairing_payload")
    device = {key: payload[key] for key in ("deviceId", "host", "certSha256")}
    device["port"] = payload.get("port", 8443)
    endpoint(device["host"], device["port"])
    return device


def pair(store, alias, payload, label, *, transport=None):
    if alias in store.db["devices"]:
        raise Failure("device_already_paired")
    if not re.fullmatch(r"[A-Za-z0-9_.-]{1,32}", alias):
        raise Failure("invalid_device_alias")
    clean_label(label, 48)
    if not label:
        raise Failure("invalid_label")
    device = physical_device(payload)
    client = transport or PinnedHTTPS(device)
    try:
        result = client.request("POST", API + "pair", {"secret": payload["secret"], "label": label}, auth=False)
    except Failure as error:
        if (isinstance(error, RemoteFailure) and error.code == "pairing_not_saved") or (error.sent and not isinstance(error, RemoteFailure)):
            # A failed NVS commit/readback may already have created the client.
            raise Failure("pair_uncertain") from None
        raise
    if not hex_value(result.get("clientId"), 16) or not hex_value(result.get("token"), 64) or result.get("deviceId") != device["deviceId"] or not integer(result.get("pushRevision")):
        raise Failure("pair_uncertain")
    device.update({key: result[key] for key in ("clientId", "token", "pushRevision")})
    store.db["devices"][alias] = device
    try:
        store.save()
    except OSError:
        raise Failure("pair_uncertain") from None
    return {"state": "paired", "device": alias}


def physical_clients(payload, *, transport=None):
    device = physical_device(payload)
    client = transport or PinnedHTTPS(device)
    result = client.request("POST", API + "clients", {"secret": payload["secret"]}, auth=False)
    clients = result.get("clients")
    if result.get("deviceId") != device["deviceId"] or not isinstance(clients, list) or len(clients) > 4:
        raise Failure("invalid_client_inventory")
    inventory, seen = [], set()
    for item in clients:
        if not isinstance(item, dict) or set(item) != {"clientId", "label"} or not hex_value(item.get("clientId"), 16) or not item.get("label"):
            raise Failure("invalid_client_inventory")
        clean_label(item["label"], 48)
        if item["clientId"] in seen:
            raise Failure("invalid_client_inventory")
        seen.add(item["clientId"])
        inventory.append({"clientId": item["clientId"], "label": item["label"]})
    return {"state": "clients", "deviceId": device["deviceId"], "clients": inventory}


def physical_revoke(payload, client_id, *, transport=None):
    device = physical_device(payload)
    if not hex_value(client_id, 16):
        raise Failure("invalid_client_id")
    client = transport or PinnedHTTPS(device)
    try:
        result = client.request("POST", API + "clients/revoke", {"secret": payload["secret"], "clientId": client_id}, auth=False)
        if result.get("revoked") is not True:
            raise Failure("revoke_uncertain")
    except Failure as error:
        if error.sent or error.code == "revocation_uncertain":
            raise Failure("revoke_uncertain") from None
        raise
    return {"state": "revoked", "clientId": client_id}


def read_payload(path):
    if path == "-":
        data = sys.stdin.buffer.read(4097)
    else:
        with open(path, "rb") as stream:
            data = stream.read(4097)
    if len(data) > 4096:
        raise Failure("invalid_pairing_payload")
    return parse_json(data)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--state-dir", default=str(Path.home() / ".local" / "share" / "paperdiy"))
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("pair"); p.add_argument("--device", required=True); p.add_argument("--payload", required=True, help="physical QR JSON file, or - for stdin"); p.add_argument("--label", default="Local sender")
    for operation in ("clients", "revoke-client"):
        p = sub.add_parser(operation); p.add_argument("--payload", required=True, help="current physical QR JSON file, or - for stdin")
        if operation == "revoke-client":
            p.add_argument("--client-id", required=True)
    for operation in ("push", "send", "sync", "status", "endpoint", "revoke", "cancel", "abandon", "prune"):
        p = sub.add_parser(operation); p.add_argument("--device", required=True)
        if operation in ("push", "send"):
            p.add_argument("source"); p.add_argument("--title", default=""); p.add_argument("--queue-only", action="store_true")
            if operation == "push":
                p.add_argument("--base-revision", type=int)
        if operation in ("push", "send", "sync"):
            p.add_argument("--wait", type=int, default=0); p.add_argument("--budget", type=int, default=120)
        if operation == "endpoint":
            p.add_argument("--host", required=True); p.add_argument("--port", type=int, default=8443)
        if operation in ("cancel", "abandon"):
            p.add_argument("--request-id", required=True)
        if operation == "prune":
            p.add_argument("--delivered", required=True, action="store_true")
    args = parser.parse_args(argv)
    try:
        # Orphan recovery works without any locally paired identity or usable
        # manifest, and never saves the short-lived physical secret.
        if args.command in ("clients", "revoke-client"):
            payload = read_payload(args.payload)
            result = physical_clients(payload) if args.command == "clients" else physical_revoke(payload, args.client_id)
            print(json.dumps(result, separators=(",", ":")))
            return 0
        with Store(args.state_dir) as store:
            if args.command == "pair":
                result = pair(store, args.device, read_payload(args.payload), args.label)
            else:
                device = store.device(args.device)
                items = [i for i in store.db["items"] if i["device"] == args.device]
                if args.command == "status":
                    result = {"state": "local", "items": [public_item(i) for i in items]}
                elif args.command == "endpoint":
                    endpoint(args.host, args.port); device.update(host=args.host, port=args.port); store.save()
                    result = {"state": "endpoint_updated", "device": args.device}
                elif args.command == "abandon":
                    item = next((i for i in items if i["requestId"] == args.request_id), None)
                    if not item or item["state"] in ("delivered", "superseded"):
                        raise Failure("unknown_pending_request")
                    item["state"] = "abandoned"; store.save()
                    result = {"state": "abandoned", "requestId": item["requestId"], "remoteOutcome": "unknown" if item["attempted"] else "not_sent"}
                elif args.command == "cancel":
                    item = next((i for i in items if i["requestId"] == args.request_id), None)
                    if not item:
                        raise Failure("unknown_pending_request")
                    if item["state"] != "delivered":
                        if not item["attempted"]:
                            item["state"] = "cancelled"
                        else:
                            sender = Sender(store, args.device)
                            try:
                                response = sender.request("POST", "uploads/cancel", {"requestId": item["requestId"]})
                                if response.get("state") == "complete":
                                    sender.complete(item, response)
                                elif response.get("state") == "cancelled":
                                    item["state"] = "cancelled"
                                else:
                                    raise Failure("cancel_uncertain")
                            except RemoteFailure as error:
                                if error.code == "cancelled":
                                    item["state"] = "cancelled"
                                else:
                                    item["state"], item["code"] = "uncertain", "cancel_uncertain"
                            except Failure:
                                item["state"], item["code"] = "uncertain", "cancel_uncertain"
                        if item["state"] == "cancelled":
                            item.pop("code", None)
                        store.save()
                    result = {"items": [public_item(item)]}
                elif args.command == "prune":
                    pruned = [i for i in items if i["state"] == "delivered"]
                    store.db["items"] = [i for i in store.db["items"] if i not in pruned]
                    store.save()
                    for item in pruned:
                        store.body(item).unlink(missing_ok=True)
                    result = {"state": "pruned", "count": len(pruned)}
                elif args.command == "revoke":
                    try:
                        response = PinnedHTTPS(device).request("POST", API + "clients/revoke", {"clientId": device["clientId"]})
                        if response.get("revoked") is not True:
                            raise Failure("revoke_uncertain")
                    except Failure as error:
                        raise Failure("revoke_uncertain" if error.sent else error.code) from None
                    device.pop("token", None); store.save(); result = {"state": "revoked", "device": args.device}
                else:
                    if not 0 <= args.wait <= 600 or not 1 <= args.budget <= 600 or args.wait > args.budget:
                        raise Failure("invalid_budget")
                    if args.command in ("push", "send"):
                        items = [store.enqueue(args.device, args.source, "push" if args.command == "push" else "file", args.title, getattr(args, "base_revision", None))]
                    else:
                        items = [i for i in items if i["state"] not in ("delivered", "cancelled", "abandoned", "superseded")]
                    if not getattr(args, "queue_only", False):
                        sender = Sender(store, args.device, budget=args.budget)
                        wait_until = time.monotonic() + args.wait
                        while True:
                            for item in items:
                                sender.send(item)
                            if not any(i["state"] == "queued" and i.get("code") == "offline" for i in items) or time.monotonic() >= wait_until:
                                break
                            time.sleep(min(1, max(0, wait_until - time.monotonic())))
                    result = {"items": [public_item(i) for i in items]}
        print(json.dumps(result, separators=(",", ":")))
        return 0 if not any(i["state"] in ("uncertain", "blocked") for i in result.get("items", [])) else 2
    except Failure as error:
        print(json.dumps({"state": "error", "code": error.code}, separators=(",", ":")))
        return 2
    except (OSError, KeyError, TypeError, ValueError, ImportError):
        # Paths, source data, endpoints and credentials never escape via a raw
        # exception message. Filesystem faults are not delivery acknowledgments.
        print('{"state":"error","code":"local_state_error"}')
        return 2


if __name__ == "__main__":
    sys.exit(main())
