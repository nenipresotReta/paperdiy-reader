#!/usr/bin/env python3
"""PaperDIY LAN sender: consent-based delivery to a reader on Settings + N.

Python 3.9+ standard library only; macOS, Linux (including containers) and
Windows. One foreground command per run, no daemon. Every batch is accepted
on the reader with a short A+B; nothing is reported delivered without the
reader's matching receipt.
"""
import argparse
import base64
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
import stat
import struct
import sys
import time
import urllib.parse

PORT = 8443
PUSH_MAX = 16384
FILE_LIMITS = {".txt": 4 * 1024 * 1024, ".epub": 64 * 1024 * 1024,
               ".md": 1024 * 1024, ".markdown": 1024 * 1024}
ARCHIVE_COUNT, ARCHIVE_BYTES = 256, 512 * 1024 * 1024
OUTBOX_FILES, OUTBOX_BYTES = 16, 128 * 1024 * 1024
RESPONSE_MAX, HEADER_MAX, CHUNK_MAX = 16384, 8192, 4096
# Matches BoundedHttpRequest::MAX_BODY, including worst-case percent-encoded
# base64 for a complete 4096-byte binary chunk.
REQUEST_BODY_MAX = 17000
BATCH_MAX, LABEL_MAX, TITLE_MAX = 8, 48, 96
POLL_SECONDS, REACH_RETRY_SECONDS = 1.0, 2.0

STATES = ("queued", "delivered", "uncertain", "blocked", "cancelled", "abandoned", "superseded")
TERMINAL = ("delivered", "cancelled", "abandoned", "superseded")
ADMISSION_CODES = ("invalid_metadata", "size_limit", "unsupported_format", "already_received",
                   "duplicate_push", "revision_conflict", "library_full", "no_space")
# Device refusals at uploads/start that prove nothing was started.
START_REFUSALS = ("size_limit", "library_full", "no_space", "revision_conflict", "invalid_metadata",
                  "unsupported_format", "storage_unavailable", "invalid_name")
UNPINNED_PATHS = ("/lan/v2/hello",)
NETWORK_CODES = ("device_unreachable", "connection_lost", "timeout", "budget_exhausted")


class Failure(Exception):
    def __init__(self, code, *, sent=False, detail=None):
        self.code, self.sent, self.detail = code, sent, detail
        super().__init__(code)


class RemoteFailure(Failure):
    def __init__(self, code, status):
        super().__init__(code, sent=True)
        self.status = status


# ---------------------------------------------------------------- validation

def hex_value(value, length):
    return isinstance(value, str) and bool(re.fullmatch(r"[0-9a-f]{%d}" % length, value))


def integer(value, maximum=0xffffffff):
    return type(value) is int and 0 <= value <= maximum


def clean_label(value, maximum=TITLE_MAX, code="invalid_label"):
    if not isinstance(value, str) or any(ord(c) < 32 or 127 <= ord(c) <= 159 for c in value):
        raise Failure(code)
    try:
        if len(value.encode("utf-8")) > maximum:
            raise Failure(code)
    except UnicodeError:
        raise Failure(code) from None
    return value


def clean_name(value):
    clean_label(value, code="invalid_name")
    if not value or value.startswith(".") or value.endswith((".", " ")) or any(c in value for c in '/\\:<>"|?*'):
        raise Failure("invalid_name")
    return value


def clean_sender_label(value):
    if not value:
        raise Failure("label_required")
    return clean_label(value, LABEL_MAX)


def ipv4(value):
    try:
        address = ipaddress.IPv4Address(value)
    except ValueError:
        return None
    if address.is_multicast or address.is_unspecified or str(address) == "255.255.255.255":
        return None
    return str(address)


def endpoint(host, port=PORT):
    try:
        ipaddress.ip_address(host)
    except ValueError:
        raise Failure("literal_ip_required") from None
    if not integer(port, 65535) or not port:
        raise Failure("invalid_port")
    return (host, port)


def fingerprint(item):
    values = [item["kind"], item["name"], item.get("title", ""), str(item["size"]),
              item["sha256"], str(item["baseRevision"]) if item["kind"] == "push" else ""]
    digest = hashlib.sha256()
    for value in values:
        encoded = value.encode("utf-8")
        digest.update(struct.pack(">I", len(encoded)))
        digest.update(encoded)
    return digest.hexdigest()


def pack_entry(item):
    """Packed batch entry: eight '<byte length>:<bytes>' fields (lan_batch.h)."""
    values = [item["requestId"], item["fingerprint"], item["kind"], item["name"], item["title"],
              str(item["size"]), item["sha256"], str(item["baseRevision"]) if item["kind"] == "push" else ""]
    return "".join("%d:%s" % (len(value.encode("utf-8")), value) for value in values)


# --------------------------------------------------------------- pairing math

def commitment(sender_nonce):
    return hashlib.sha256(sender_nonce).hexdigest()


def sas_value(certificate, sender_nonce, device_nonce, pairing_id, label):
    encoded = label.encode("utf-8")
    message = (b"paperdiy-pair-v2\0" + certificate + sender_nonce + device_nonce +
               pairing_id.encode("ascii") + struct.pack(">H", len(encoded)) + encoded)
    return struct.unpack(">I", hashlib.sha256(message).digest()[:4])[0] % 1000000


def sas_text(value):
    return "%03d %03d" % (value // 1000, value % 1000)


def status_proof(sender_nonce, batch_id):
    return hmac.new(sender_nonce, b"paperdiy-batch-status-v2" + batch_id.encode("ascii"), hashlib.sha256).hexdigest()


# ----------------------------------------------------------------------- JSON

def json_bytes(value):
    return json.dumps(value, ensure_ascii=True, separators=(",", ":"), allow_nan=False).encode("ascii")


def parse_json(data, code="invalid_response"):
    def pairs(entries):
        result = {}
        for key, value in entries:
            if key in result:
                raise Failure(code)
            result[key] = value
        return result

    def constant(_):
        raise Failure(code)
    try:
        value = json.loads(data.decode("utf-8"), object_pairs_hook=pairs, parse_constant=constant)
    except (ValueError, UnicodeError, RecursionError):
        raise Failure(code) from None
    if not isinstance(value, dict):
        raise Failure(code)
    return value


# ------------------------------------------------------------------ transport

class HTTPS:
    """One request per TLS connection. The live certificate is checked against
    the pin before any HTTP byte is written; only the identity probe may run
    unpinned, and it never carries credentials."""

    def __init__(self, timeout=6.0, clock=time.monotonic):
        self.timeout, self.clock = timeout, clock

    @staticmethod
    def packet(address, method, path, form=None, *, pin, credentials=None):
        """Exact request bytes; validation happens before any connection."""
        host, port = endpoint(*address)
        if pin is None and (path not in UNPINNED_PATHS or credentials is not None or form is not None):
            raise Failure("invalid_request")
        if pin is not None and not hex_value(pin, 64):
            raise Failure("invalid_pin")
        if not path.startswith(("/lan/v1/", "/lan/v2/")) or any(c in path for c in "\r\n "):
            raise Failure("invalid_request")
        body = urllib.parse.urlencode(form or {}).encode("ascii") if form is not None else b""
        if len(body) > REQUEST_BODY_MAX:
            raise Failure("request_too_large")
        host_header = "[%s]" % host if ":" in host else host
        headers = ["%s %s HTTP/1.1" % (method, path), "Host: %s:%d" % (host_header, port),
                   "Connection: close", "Accept: application/json", "Content-Length: %d" % len(body)]
        if form is not None:
            headers.append("Content-Type: application/x-www-form-urlencoded")
        if credentials is not None:
            client_id, token = credentials
            if not hex_value(client_id, 16) or not hex_value(token, 64):
                raise Failure("not_paired")
            headers.append("Authorization: Bearer %s:%s" % (client_id, token))
        return ("\r\n".join(headers) + "\r\n\r\n").encode("ascii") + body

    def request(self, address, method, path, form=None, *, pin, credentials=None, deadline):
        host, port = endpoint(*address)
        packet = self.packet(address, method, path, form, pin=pin, credentials=credentials)
        limit = min(deadline, self.clock() + self.timeout)

        def remaining():
            left = limit - self.clock()
            if left <= 0:
                raise Failure("budget_exhausted" if self.clock() >= deadline else "timeout")
            return left
        sent = False
        try:
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
            context.minimum_version = ssl.TLSVersion.TLSv1_2
            # The reader's self-signed certificate is the trust anchor, pinned
            # by exact SHA-256 of its DER encoding instead of CA validation.
            context.check_hostname = False
            context.verify_mode = ssl.CERT_NONE
            with socket.create_connection((host, port), timeout=remaining()) as raw:
                raw.settimeout(remaining())
                with context.wrap_socket(raw, server_hostname=host) as stream:
                    observed = hashlib.sha256(stream.getpeercert(binary_form=True)).hexdigest()
                    if pin is not None and not hmac.compare_digest(observed, pin):
                        raise Failure("pin_mismatch")
                    stream.settimeout(remaining())
                    sent = True
                    stream.sendall(packet)
                    return self._response(stream, remaining), observed
        except Failure as error:
            error.sent = error.sent or sent
            raise
        except (OSError, ssl.SSLError):
            raise Failure("connection_lost" if sent else "device_unreachable", sent=sent) from None

    @classmethod
    def parse(cls, data):
        """Parse a complete response held in memory (used by wire tests)."""
        class Buffer:
            def __init__(self, data):
                self.data = data
            def settimeout(self, _):
                pass
            def recv(self, size):
                block, self.data = self.data[:size], self.data[size:]
                return block
        return cls._response(Buffer(data), lambda: 1.0)

    @staticmethod
    def _response(stream, remaining):
        data = b""
        while b"\r\n\r\n" not in data:
            stream.settimeout(remaining())
            block = stream.recv(min(1024, HEADER_MAX + 4 - len(data)))
            if not block:
                raise Failure("connection_lost", sent=True)
            data += block
            if len(data) >= HEADER_MAX + 4 and b"\r\n\r\n" not in data:
                raise Failure("invalid_response", sent=True)
        head, data = data.split(b"\r\n\r\n", 1)
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
            raise Failure("invalid_response", sent=True)
        while len(data) < length:
            stream.settimeout(remaining())
            block = stream.recv(min(4096, length - len(data)))
            if not block:
                raise Failure("connection_lost", sent=True)
            data += block
        if 300 <= status < 400:
            raise Failure("invalid_response", sent=True)
        result = parse_json(data)
        if not 200 <= status < 300:
            code = result.get("code", "remote_error")
            if not isinstance(code, str) or not re.fullmatch(r"[a-z_]{1,48}", code):
                code = "remote_error"
            raise RemoteFailure(code, status)
        return result


# ----------------------------------------------------------------------- mDNS

def dns_name(name):
    out = b""
    for label in name.rstrip(".").split("."):
        encoded = label.encode("ascii")
        if not 0 < len(encoded) < 64:
            raise Failure("invalid_name")
        out += bytes([len(encoded)]) + encoded
    return out + b"\0"


def read_name(packet, offset):
    labels, jumps, end = [], 0, None
    while True:
        if offset >= len(packet):
            raise ValueError("truncated name")
        length = packet[offset]
        if length & 0xc0 == 0xc0:
            if offset + 1 >= len(packet) or jumps > 16:
                raise ValueError("bad pointer")
            if end is None:
                end = offset + 2
            offset = ((length & 0x3f) << 8) | packet[offset + 1]
            jumps += 1
            continue
        if length & 0xc0:
            raise ValueError("bad label")
        offset += 1
        if length == 0:
            break
        if offset + length > len(packet):
            raise ValueError("truncated label")
        labels.append(packet[offset:offset + length].decode("ascii", "replace").lower())
        offset += length
        if len(labels) > 64:
            raise ValueError("long name")
    return ".".join(labels), (end if end is not None else offset)


def parse_dns(packet, query_id):
    """Answer and additional records of one response, or [] if not ours."""
    if len(packet) < 12:
        return []
    ident, flags, qd, an, ns, ar = struct.unpack(">HHHHHH", packet[:12])
    if not flags & 0x8000 or ident not in (query_id, 0) or flags & 0x000f:
        return []
    records, offset = [], 12
    try:
        for _ in range(qd):
            _, offset = read_name(packet, offset)
            offset += 4
        for _ in range(an + ns + ar):
            name, offset = read_name(packet, offset)
            if offset + 10 > len(packet):
                raise ValueError("truncated record")
            kind, _, _, length = struct.unpack(">HHIH", packet[offset:offset + 10])
            offset += 10
            if offset + length > len(packet):
                raise ValueError("truncated data")
            data = offset
            offset += length
            if kind == 1 and length == 4:
                records.append(("A", name, socket.inet_ntoa(packet[data:offset])))
            elif kind == 12:
                records.append(("PTR", name, read_name(packet, data)[0]))
            elif kind == 33 and length >= 7:
                port = struct.unpack(">H", packet[data + 4:data + 6])[0]
                records.append(("SRV", name, (read_name(packet, data + 6)[0], port)))
            elif kind == 16:
                strings, at = [], data
                while at < offset:
                    size = packet[at]
                    strings.append(packet[at + 1:at + 1 + size].decode("utf-8", "replace"))
                    at += 1 + size
                records.append(("TXT", name, strings))
    except (ValueError, IndexError, struct.error):
        return records
    return records


class Mdns:
    """RFC 6762 one-shot queries from an ephemeral port (unicast replies)."""
    SERVICE = "_paperdiy._tcp.local"

    def __init__(self, address=("224.0.0.251", 5353), attempt=1.0, clock=time.monotonic):
        self.address, self.attempt, self.clock = address, attempt, clock

    def _query(self, name, kind, deadline, until_found):
        records = []
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
                sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 255)
                sock.bind(("", 0))
                for _ in range(2):
                    if self.clock() >= deadline:
                        break
                    query_id = secrets.randbelow(0xffff) + 1
                    # QU bit requests a unicast answer to this port.
                    packet = struct.pack(">HHHHHH", query_id, 0, 1, 0, 0, 0) + dns_name(name) + struct.pack(">HH", kind, 0x8001)
                    sock.sendto(packet, self.address)
                    stop = min(deadline, self.clock() + self.attempt)
                    while self.clock() < stop:
                        sock.settimeout(max(0.01, stop - self.clock()))
                        try:
                            data, _ = sock.recvfrom(9000)
                        except socket.timeout:
                            break
                        records.extend(parse_dns(data, query_id))
                        if until_found(records):
                            return records
                    if records:
                        return records
        except OSError:
            return records
        return records

    def resolve(self, device_id, deadline):
        if not hex_value(device_id, 16):
            return []
        host = "paperdiy-%s.local" % device_id
        records = self._query(host, 1, deadline, lambda r: any(k == "A" and n == host for k, n, _ in r))
        return sorted({v for k, n, v in records if k == "A" and n == host and ipv4(v)})

    def browse(self, deadline):
        records = self._query(self.SERVICE, 12, deadline, lambda r: False)
        instances = {v for k, n, v in records if k == "PTR" and n == self.SERVICE}
        candidates = {}
        for instance in sorted(instances):
            device_id = None
            for kind, name, value in records:
                if kind == "TXT" and name == instance:
                    for entry in value:
                        if entry.startswith("id=") and hex_value(entry[3:], 16):
                            device_id = entry[3:]
            target = next((v[0] for k, n, v in records if k == "SRV" and n == instance), None)
            if not device_id:
                # Instance and host names both default to paperdiy-<id>.
                match = (re.fullmatch(r"paperdiy-([0-9a-f]{16})\.local", target or "")
                         or re.fullmatch(r"paperdiy-([0-9a-f]{16})\._paperdiy\._tcp\.local", instance))
                device_id = match[1] if match else None
            if not device_id:
                continue
            host = "paperdiy-%s.local" % device_id
            addresses = sorted({v for k, n, v in records if k == "A" and n in (host, target) and ipv4(v)})
            if not addresses:
                addresses = self.resolve(device_id, deadline)
            if addresses:
                candidates[device_id] = {"deviceId": device_id, "host": addresses[0]}
        return [candidates[key] for key in sorted(candidates)]


# ---------------------------------------------------------------- local state

class PosixFiles:
    name = "posix"

    @staticmethod
    def default_root():
        base = os.environ.get("XDG_DATA_HOME") or str(Path.home() / ".local" / "share")
        return Path(base) / "paperdiy"

    @staticmethod
    def private_dir(path):
        try:
            path.mkdir(mode=0o700, parents=True, exist_ok=True)
            info = os.lstat(path)
        except OSError:
            raise Failure("state_dir_unavailable") from None
        if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
            raise Failure("state_dir_permissions")

    @staticmethod
    def private_file(path):
        info = os.lstat(path)
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
            raise Failure("state_dir_permissions")
        return info

    @staticmethod
    def create(path):
        return os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)

    @staticmethod
    def lock(path):
        import fcntl
        fd = os.open(path, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            os.close(fd)
            raise Failure("lock_held") from None
        return fd

    @staticmethod
    def unlock(fd):
        os.close(fd)

    @staticmethod
    def sync_dir(path):
        fd = os.open(path, os.O_RDONLY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)


class WindowsFiles:
    """Privacy rests on the per-user profile ACL of %LOCALAPPDATA%; this layer
    rejects symlinks and junctions. Weaker than the POSIX mode checks."""
    name = "windows"
    REPARSE = 0x400

    @staticmethod
    def default_root():
        base = os.environ.get("LOCALAPPDATA")
        if not base:
            raise Failure("state_dir_unavailable")
        return Path(base) / "paperdiy"

    @classmethod
    def _plain(cls, path, directory):
        info = os.lstat(path)
        if getattr(info, "st_file_attributes", 0) & cls.REPARSE or stat.S_ISLNK(info.st_mode):
            raise Failure("state_dir_permissions")
        if not (stat.S_ISDIR(info.st_mode) if directory else stat.S_ISREG(info.st_mode)):
            raise Failure("state_dir_permissions")
        if os.path.normcase(os.path.realpath(path)) != os.path.normcase(os.path.abspath(path)):
            raise Failure("state_dir_permissions")
        return info

    @classmethod
    def private_dir(cls, path):
        try:
            path.mkdir(parents=True, exist_ok=True)
        except OSError:
            raise Failure("state_dir_unavailable") from None
        cls._plain(path, True)

    @classmethod
    def private_file(cls, path):
        return cls._plain(path, False)

    @staticmethod
    def create(path):
        return os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_NOINHERIT", 0), 0o600)

    @staticmethod
    def lock(path):
        import msvcrt
        fd = os.open(path, os.O_CREAT | os.O_RDWR | getattr(os, "O_BINARY", 0) | getattr(os, "O_NOINHERIT", 0), 0o600)
        try:
            os.lseek(fd, 0, os.SEEK_SET)
            msvcrt.locking(fd, msvcrt.LK_NBLCK, 1)
        except OSError:
            os.close(fd)
            raise Failure("lock_held") from None
        return fd

    @staticmethod
    def unlock(fd):
        import msvcrt
        try:
            os.lseek(fd, 0, os.SEEK_SET)
            msvcrt.locking(fd, msvcrt.LK_UNLCK, 1)
        except OSError:
            pass
        os.close(fd)

    @staticmethod
    def sync_dir(path):
        # Windows has no directory fsync; os.replace is atomic on NTFS.
        return None


FILES = WindowsFiles if os.name == "nt" else PosixFiles


def migrate_v1(db):
    """v1 keyed devices by alias and paired with a physical QR secret."""
    devices, aliases = {}, {}
    for alias, device in db.get("devices", {}).items():
        if not isinstance(device, dict) or not hex_value(device.get("deviceId"), 16):
            raise Failure("store_corrupt")
        aliases[alias] = device["deviceId"]
        if not hex_value(device.get("token"), 64):
            continue  # revoked in v1: its open entries become unassigned
        record = {key: device.get(key) for key in ("deviceId", "host", "port", "certSha256", "clientId", "token")}
        record.update(pushRevision=device.get("pushRevision", 0), label="")
        devices[device["deviceId"]] = record
    items = []
    for item in db.get("items", []):
        if not isinstance(item, dict) or item.get("device") not in aliases:
            raise Failure("store_corrupt")
        state = item.get("state")
        attempted = item.get("attempted") is True
        record = {key: item.get(key) for key in ("requestId", "kind", "name", "title", "size", "sha256", "baseRevision", "fingerprint", "created")}
        owner = aliases[item["device"]]
        record.update(device=owner if owner in devices or state in TERMINAL else None,
                      batchId=None, offset=0, startSent=attempted, commitSent=False)
        if state in ("receiving", "checking", "uncertain") or (state == "queued" and attempted):
            # v1 kept no commit marker: resolve through receipts before anything new.
            record.update(state="uncertain", code="commit_unconfirmed", commitSent=True)
        elif state in STATES:
            record["state"] = state
            if "code" in item and state != "delivered":
                record["code"] = item["code"]
            if state == "delivered" and isinstance(item.get("receipt"), dict):
                record["receipt"] = item["receipt"]
        else:
            raise Failure("store_corrupt")
        if record["kind"] == "push" and record.get("baseRevision") is None:
            record["fingerprint"] = None
        items.append(record)
    return {"version": 2, "devices": devices, "items": items, "pending": []}


class Store:
    def __init__(self, directory=None, files=None):
        self.files = files or FILES
        self.root = Path(directory) if directory else self.files.default_root()
        self.db = {"version": 2, "devices": {}, "items": [], "pending": []}
        self.lock = None

    def __enter__(self):
        self.files.private_dir(self.root)
        self.objects = self.root / "objects"
        self.files.private_dir(self.objects)
        self.lock = self.files.lock(self.root / "lock")
        try:
            manifest = self.root / "state.json"
            if os.path.lexists(manifest):
                try:
                    info = self.files.private_file(manifest)
                    if info.st_size > 1024 * 1024:
                        raise Failure("store_corrupt")
                    db = parse_json(manifest.read_bytes(), "store_corrupt")
                except OSError:
                    raise Failure("state_dir_unavailable") from None
                if db.get("version") == 1:
                    db = migrate_v1(db)
                self.db = db
                self.validate()
            self.object_usage()
            return self
        except BaseException:
            self.files.unlock(self.lock)
            raise

    def __exit__(self, *_):
        self.files.unlock(self.lock)

    def validate(self):
        db = self.db
        if db.get("version") != 2 or not isinstance(db.get("devices"), dict) or not isinstance(db.get("items"), list) or not isinstance(db.get("pending"), list):
            raise Failure("store_corrupt")
        for key, device in db["devices"].items():
            if not isinstance(device, dict) or device.get("deviceId") != key or not hex_value(key, 16) or not hex_value(device.get("certSha256"), 64):
                raise Failure("store_corrupt")
            if not hex_value(device.get("clientId"), 16) or not hex_value(device.get("token"), 64) or not integer(device.get("pushRevision")):
                raise Failure("store_corrupt")
            try:
                endpoint(device.get("host"), device.get("port"))
            except Failure:
                raise Failure("store_corrupt") from None
        if len(db["items"]) > ARCHIVE_COUNT or len(db["pending"]) > 16:
            raise Failure("store_corrupt")
        for item in db["items"]:
            try:
                ok = (isinstance(item, dict) and hex_value(item.get("requestId"), 32) and item.get("kind") in ("push", "file")
                      and item.get("state") in STATES and integer(item.get("size"), 64 * 1024 * 1024) and integer(item.get("offset"), item["size"])
                      and hex_value(item.get("sha256"), 64) and type(item.get("startSent")) is bool and type(item.get("commitSent")) is bool
                      and (item.get("device") is None or hex_value(item.get("device"), 16))
                      and (item.get("batchId") is None or hex_value(item.get("batchId"), 16))
                      and (item.get("code") is None or isinstance(item.get("code"), str))
                      and type(item.get("accepted", False)) is bool)
                if ok:
                    clean_label(item.get("name")); clean_label(item.get("title"))
                    base = item.get("baseRevision")
                    if item["kind"] == "file":
                        ok = base is None and item.get("fingerprint") == fingerprint(item)
                    elif base is None:
                        ok = item.get("fingerprint") is None
                    else:
                        ok = integer(base) and item.get("fingerprint") == fingerprint(item)
            except (Failure, KeyError, TypeError):
                ok = False
            if not ok:
                raise Failure("store_corrupt")
        for record in db["pending"]:
            if not isinstance(record, dict) or not all(hex_value(record.get(k), n) for k, n in (("batchId", 16), ("pairingId", 16), ("senderNonce", 64), ("deviceNonce", 64), ("certSha256", 64), ("deviceId", 16))):
                raise Failure("store_corrupt")

    def save(self):
        temporary = self.root / (".state-%s.tmp" % secrets.token_hex(8))
        try:
            fd = self.files.create(temporary)
            with open(fd, "wb") as stream:
                stream.write(json_bytes(self.db)); stream.flush(); os.fsync(stream.fileno())
            os.replace(temporary, self.root / "state.json")
            self.files.sync_dir(self.root)
        finally:
            if os.path.lexists(temporary):
                try:
                    temporary.unlink()
                except OSError:
                    pass

    def object_usage(self):
        count, size = 0, 0
        for path in self.objects.iterdir():
            if not re.fullmatch(r"[0-9a-f]{32}\.bin", path.name):
                raise Failure("store_corrupt")
            info = self.files.private_file(path)
            count += 1; size += info.st_size
        return count, size

    def body(self, item):
        return self.objects / (item["requestId"] + ".bin")

    def paired(self):
        return list(self.db["devices"].values())

    def stage(self, source, kind, title):
        """Copy one source into the archive; returns an unsaved item."""
        clean_label(title, TITLE_MAX, "invalid_title")
        name = "Push.md" if kind == "push" else clean_name(Path(source).name)
        maximum = PUSH_MAX if kind == "push" else FILE_LIMITS.get(Path(name).suffix.lower())
        if maximum is None:
            raise Failure("unsupported_format")
        request_id = secrets.token_hex(16)
        target = self.body({"requestId": request_id})
        size, digest = 0, hashlib.sha256()
        try:
            with open(source, "rb") as incoming:
                fd = self.files.create(target)
                with open(fd, "wb") as outgoing:
                    while True:
                        block = incoming.read(65536)
                        if not block:
                            break
                        size += len(block)
                        if size > maximum:
                            raise Failure("size_limit")
                        digest.update(block); outgoing.write(block)
                    outgoing.flush(); os.fsync(outgoing.fileno())
        except Failure:
            self.discard(target); raise
        except OSError:
            self.discard(target); raise Failure("source_unreadable") from None
        if not size:
            self.discard(target); raise Failure("empty_document")
        if kind == "push":
            try:
                target.read_bytes().decode("utf-8")
            except UnicodeError:
                self.discard(target); raise Failure("invalid_utf8") from None
        return {"requestId": request_id, "device": None, "kind": kind, "name": name, "title": title,
                "size": size, "sha256": digest.hexdigest(), "baseRevision": None, "fingerprint": None,
                "state": "queued", "batchId": None, "startSent": False, "commitSent": False,
                "offset": 0, "created": int(time.time())}

    @staticmethod
    def discard(path):
        try:
            path.unlink()
        except OSError:
            pass

    def enqueue(self, device_id, sources, kind, title=""):
        """Stage every source, then commit them to the manifest in one save."""
        records = self.db["items"]
        pending = [i for i in records if i["state"] not in TERMINAL and i.get("device") in (device_id, None)]
        previous = next((i for i in pending if i["kind"] == "push"), None) if kind == "push" else None
        if previous and (previous["startSent"] or previous.get("batchId")):
            raise Failure("resolve_previous_push")
        count, size = self.object_usage()
        staged = []
        try:
            for source in sources:
                if len(records) + len(staged) >= ARCHIVE_COUNT or count + len(staged) >= ARCHIVE_COUNT:
                    raise Failure("archive_full")
                item = self.stage(source, kind, title)
                staged.append(item)
                size += item["size"]
                if size > ARCHIVE_BYTES:
                    raise Failure("archive_full")
            files = [i for i in pending if i["kind"] == "file"] + [i for i in staged if i["kind"] == "file"]
            if len(files) > OUTBOX_FILES or sum(i["size"] for i in files) > OUTBOX_BYTES:
                raise Failure("outbox_full")
            device = self.db["devices"].get(device_id) if device_id else None
            for item in staged:
                item["device"] = device_id
                if kind == "push" and device is not None:
                    item["baseRevision"] = device["pushRevision"]
                if kind == "file" or item["baseRevision"] is not None:
                    item["fingerprint"] = fingerprint(item)
            self.files.sync_dir(self.objects)
            if previous:
                previous["state"] = "superseded"; previous.pop("code", None)
            records.extend(staged)
            self.save()
            return staged
        except BaseException:
            for item in staged:
                if item not in records:
                    self.discard(self.body(item))
            raise


# ------------------------------------------------------------------- delivery

def public_item(item):
    result = {key: item[key] for key in ("requestId", "kind", "name", "state", "size", "offset")}
    if item.get("title"):
        result["title"] = item["title"]
    if item.get("code"):
        result["code"] = item["code"]
    if item.get("batchId") and item["state"] == "queued":
        result["batchId"] = item["batchId"]
    return result


def set_state(item, state, code=None):
    item["state"] = state
    if code:
        item["code"] = code
    else:
        item.pop("code", None)


class Session:
    """One command's interaction with one reader. Times are monotonic."""

    def __init__(self, store, *, transport=None, resolver=None, wait=120, budget=600,
                 clock=time.monotonic, sleep=time.sleep, notify=None):
        self.store, self.clock, self.sleep = store, clock, sleep
        self.transport = transport or HTTPS(clock=clock)
        self.resolver = resolver or Mdns(clock=clock)
        start = clock()
        self.wait_until, self.deadline = start + wait, start + budget
        self.notify = notify
        self.offered = set()   # never offer the same entry twice in one command
        self.result = {}

    # ------------------------------------------------------------- plumbing
    def waiting(self):
        return self.clock() < self.wait_until

    def pause(self, seconds):
        left = min(self.wait_until, self.deadline) - self.clock()
        if left > 0:
            self.sleep(min(seconds, left))

    def call(self, address, method, path, form=None, *, pin, device=None):
        credentials = (device["clientId"], device["token"]) if device else None
        result, _ = self.transport.request(address, method, path, form, pin=pin, credentials=credentials, deadline=self.deadline)
        return result

    def hello(self, address):
        result, observed = self.transport.request(address, "GET", "/lan/v2/hello", pin=None, deadline=self.deadline)
        device_id = result.get("deviceId")
        if result.get("v") != 2 or not hex_value(device_id, 16) or not integer(result.get("pushRevision")):
            raise Failure("invalid_response")
        # The device ID is the first 16 hex digits of its certificate SHA-256.
        if observed[:16] != device_id:
            raise Failure("pin_mismatch")
        return device_id, observed, result["pushRevision"]

    def items_for(self, device_id):
        return [i for i in self.store.db["items"] if i.get("device") == device_id]

    # ------------------------------------------------------------ addressing
    def reach(self, device, host=None):
        """Pinned capabilities at the stored (or given) host, then by mDNS."""
        port = device["port"]

        def attempt(address):
            caps = self.call(address, "GET", "/lan/v1/capabilities", pin=device["certSha256"], device=device)
            if caps.get("v") != 2 or caps.get("deviceId") != device["deviceId"] or not integer(caps.get("pushRevision")):
                raise Failure("device_identity_mismatch")
            return caps
        first = (host or device["host"], port)
        while True:
            tried, last = set(), None
            for address in [first] + [None]:
                if address is None:
                    addresses = [(ip, port) for ip in self.resolver.resolve(device["deviceId"], min(self.deadline, self.clock() + 2.5))]
                else:
                    addresses = [address]
                for candidate in addresses:
                    if candidate in tried:
                        continue
                    tried.add(candidate)
                    try:
                        caps = attempt(candidate)
                    except RemoteFailure as error:
                        if error.code == "unauthorized":
                            raise Failure("unauthorized") from None
                        if error.code in ("recovering", "rate_limited", "credentials_unavailable"):
                            last = error; continue
                        raise
                    except Failure as error:
                        # Never persist an endpoint whose certificate differs.
                        if error.code in ("device_unreachable", "connection_lost", "timeout", "pin_mismatch", "device_identity_mismatch"):
                            last = error; continue
                        raise
                    changed = candidate[0] != device["host"] or caps["pushRevision"] != device["pushRevision"]
                    device["host"], device["pushRevision"] = candidate[0], caps["pushRevision"]
                    if changed:
                        self.store.save()
                    return candidate, caps
            if not self.waiting() or self.clock() >= self.deadline:
                raise Failure("device_unreachable", detail=last.code if last else None)
            self.pause(REACH_RETRY_SECONDS)

    def discover(self, host=None):
        """Unpaired target: --host, else exactly one advertised reader."""
        while True:
            if host:
                candidates = [{"deviceId": None, "host": host}]
            else:
                candidates = self.resolver.browse(min(self.deadline, self.clock() + 2.5))
            if len(candidates) > 1:
                raise Failure("multiple_devices", detail=candidates)
            if candidates:
                address = endpoint(candidates[0]["host"], PORT)
                try:
                    device_id, observed, revision = self.hello(address)
                    if candidates[0]["deviceId"] not in (None, device_id):
                        raise Failure("pin_mismatch")
                    return address, device_id, observed, revision
                except RemoteFailure as error:
                    if error.code not in ("recovering", "rate_limited"):
                        raise
                except Failure as error:
                    if error.code not in ("device_unreachable", "connection_lost", "timeout"):
                        raise
            if not self.waiting() or self.clock() >= self.deadline:
                raise Failure("device_unreachable" if host else "device_not_found")
            self.pause(REACH_RETRY_SECONDS)

    # -------------------------------------------------------------- receipts
    def complete(self, device, item, response):
        receipt = response.get("receipt")
        if not isinstance(receipt, dict) or any(receipt.get(key) != item[key] for key in ("requestId", "fingerprint", "kind", "size", "sha256")):
            raise Failure("invalid_receipt")
        if item["kind"] == "push":
            if not integer(receipt.get("revision")):
                raise Failure("invalid_receipt")
            device["pushRevision"] = max(device["pushRevision"], receipt["revision"])
        else:
            try:
                clean_name(receipt.get("name"))
            except Failure:
                raise Failure("invalid_receipt") from None
        item["offset"], item["receipt"], item["batchId"] = item["size"], receipt, None
        set_state(item, "delivered")
        self.store.save()

    def progress(self, item, response):
        """Validate an upload status; return the next offset, None when done."""
        state = response.get("state")
        if state == "cancelled" and response.get("requestId") == item["requestId"]:
            item["batchId"] = None
            set_state(item, "cancelled"); self.store.save()
            return None
        offset = response.get("offset")
        if state not in ("receiving", "checking") or response.get("requestId") != item["requestId"] or response.get("fingerprint") != item["fingerprint"] or response.get("size") != item["size"] or not integer(offset, item["size"]) or (state == "checking" and offset != item["size"]):
            raise Failure("invalid_upload_status")
        item["offset"] = offset
        return offset

    def lookup(self, address, device, item):
        """Receipt, else live transaction status, else None (device has nothing)."""
        query = "?requestId=" + item["requestId"]
        try:
            response = self.call(address, "GET", "/lan/v1/receipts" + query, pin=device["certSha256"], device=device)
        except RemoteFailure as error:
            if error.status != 404:
                raise
        else:
            if response.get("state") == "complete":
                self.complete(device, item, response)
            else:
                self.progress(item, response)
            return response
        try:
            response = self.call(address, "GET", "/lan/v1/uploads/status" + query, pin=device["certSha256"], device=device)
        except RemoteFailure as error:
            if error.status == 404:
                return None
            raise
        if response.get("state") == "complete":
            self.complete(device, item, response)
        else:
            self.progress(item, response)
        return response

    def resolve_uncertain(self, address, device):
        """Receipt ring discipline: settle every uncertain entry first."""
        for item in self.items_for(device["deviceId"]):
            if item["state"] != "uncertain":
                continue
            try:
                response = self.lookup(address, device, item)
            except Failure as error:
                if error.code in ("invalid_receipt", "invalid_upload_status"):
                    item["code"] = error.code; self.store.save(); continue
                if error.code in NETWORK_CODES:
                    return False
                raise
            if item["state"] != "uncertain":
                continue
            if response is None:
                # Nothing on the reader: published files always leave a receipt,
                # so a missing one is unknowable once the ring may have moved on.
                item["code"] = "receipt_unknown"
            elif response.get("state") == "receiving":
                item.update(commitSent=False, batchId=None)
                set_state(item, "queued", "transfer_interrupted")
            elif response.get("state") == "checking":
                self.finish_checking(address, device, item)
            self.store.save()
        return not any(i["state"] == "uncertain" for i in self.items_for(device["deviceId"]))

    def finish_checking(self, address, device, item):
        query = "?requestId=" + item["requestId"]
        while True:
            if self.clock() + 0.1 >= self.deadline:
                set_state(item, "uncertain", "commit_unconfirmed"); return
            self.sleep(0.1)
            try:
                response = self.call(address, "GET", "/lan/v1/uploads/status" + query, pin=device["certSha256"], device=device)
            except RemoteFailure as error:
                if error.status == 404:
                    set_state(item, "uncertain", "receipt_unknown"); return
                raise
            except Failure:
                set_state(item, "uncertain", "commit_unconfirmed"); return
            if response.get("state") == "complete":
                self.complete(device, item, response); return
            if self.progress(item, response) is None:
                return
            if response.get("state") == "receiving":
                item.update(commitSent=False, batchId=None)
                set_state(item, "queued", "transfer_interrupted"); return

    # --------------------------------------------------------------- batches
    def next_batch(self, device_id, reoffer):
        batch, push = [], False
        for item in self.items_for(device_id):
            if item["state"] != "queued" or item.get("batchId") or item["requestId"] in self.offered:
                continue
            if item.get("code") == "not_accepted" and not reoffer:
                continue
            if item["kind"] == "push":
                if push:
                    continue
                push = True
            batch.append(item)
            if len(batch) == BATCH_MAX:
                break
        return batch

    def admit(self, address, device, entries, response):
        """Apply per-entry admission; returns entries the reader will prompt for."""
        listed = response.get("entries")
        if not isinstance(listed, list) or len(listed) != len(entries):
            raise Failure("invalid_response")
        admitted = []
        for item, entry in zip(entries, listed):
            if not isinstance(entry, dict) or entry.get("requestId") != item["requestId"]:
                raise Failure("invalid_response")
            code = entry.get("admission")
            if code == "ok":
                admitted.append(item)
            elif code == "already_received" and device is not None:
                try:
                    self.lookup(address, device, item)
                except Failure:
                    pass
                if item["state"] == "queued":
                    set_state(item, "uncertain", "receipt_unknown")
            elif code in ADMISSION_CODES:
                item["batchId"] = None
                set_state(item, "blocked", code)
            else:
                raise Failure("invalid_response")
        return admitted

    def mark(self, items, code, batch_id=None):
        for item in items:
            if item["state"] == "queued":
                item["code"] = code
                if batch_id is not None:
                    item["batchId"] = batch_id or None
                    if not batch_id:
                        item["accepted"] = False
        self.store.save()

    def lost(self, items):
        """The reader no longer holds this batch (N switched off, or expired)."""
        accepted = [i for i in items if i.get("accepted")]
        self.mark(accepted, "transfer_interrupted", "")
        self.mark([i for i in items if i not in accepted], "not_accepted", "")

    def offer(self, address, device, entries):
        """Paired offer. Returns (batchId, admitted) or (None, []) if blocked."""
        for item in entries:
            if item["kind"] == "push" and item["baseRevision"] is None:
                item["baseRevision"] = device["pushRevision"]; item["fingerprint"] = fingerprint(item)
        form = {"e%d" % n: pack_entry(item) for n, item in enumerate(entries)}
        self.offered.update(i["requestId"] for i in entries)
        while True:
            try:
                response = self.call(address, "POST", "/lan/v2/batch/offer", form, pin=device["certSha256"], device=device)
                break
            except RemoteFailure as error:
                if error.code == "batch_busy" and self.waiting():
                    self.mark(entries, "batch_busy"); self.pause(REACH_RETRY_SECONDS); continue
                if error.code in ("batch_busy", "rate_limited", "recovering"):
                    self.mark(entries, error.code); return None, []
                if error.code == "unauthorized":
                    raise Failure("unauthorized") from None
                raise
            except Failure as error:
                if error.sent:
                    # A lost offer response may have left a prompt on screen;
                    # its unknown batch is never accepted for these entries.
                    self.mark(entries, "not_accepted"); return None, []
                raise
        admitted = self.admit(address, device, entries, response)
        if response.get("state") == "blocked":
            if admitted:
                raise Failure("invalid_response")
            self.store.save(); return None, []
        batch_id = response.get("batchId")
        if response.get("state") != "awaiting_acceptance" or not hex_value(batch_id, 16) or not admitted:
            raise Failure("invalid_response")
        for item in admitted:
            item["accepted"] = False
        self.mark(admitted, "awaiting_acceptance", batch_id)
        return batch_id, admitted

    def await_acceptance(self, address, device, batch_id, entries):
        """'accepted'; 'lost' when the reader no longer holds the batch;
        None when still pending or unreachable (entries keep the batch ID)."""
        path = "/lan/v2/batch/status?batchId=" + batch_id
        unreachable = False
        while True:
            try:
                response = self.call(address, "GET", path, pin=device["certSha256"], device=device)
                unreachable = False
            except RemoteFailure as error:
                if error.status == 404:
                    self.lost(entries); return "lost"
                if error.code not in ("rate_limited", "recovering"):
                    raise
                response = None
            except Failure as error:
                if error.code not in ("device_unreachable", "connection_lost", "timeout"):
                    raise
                response, unreachable = None, True
            state = response.get("state") if response else None
            if state == "accepted":
                for item in entries:
                    item["accepted"] = True
                self.store.save()
                return "accepted"
            if state == "expired":
                self.lost(entries); return "lost"
            if state not in (None, "awaiting_acceptance"):
                raise Failure("invalid_response")
            if not self.waiting():
                # Still pending, or unreachable with the batch possibly pending:
                # keep the batch ID so the next sync asks the reader.
                self.mark(entries, "device_unreachable" if unreachable else "awaiting_acceptance")
                return None
            self.pause(POLL_SECONDS)

    # --------------------------------------------------------------- uploads
    def upload(self, address, device, item, limits):
        """Upload one accepted entry. Returns False when the batch is gone."""
        path = self.store.body(item)
        try:
            data_ok = path.stat().st_size == item["size"]
            digest = hashlib.sha256()
            with path.open("rb") as stream:
                for block in iter(lambda: stream.read(65536), b""):
                    digest.update(block)
            data_ok = data_ok and digest.hexdigest() == item["sha256"]
        except OSError:
            data_ok = False
        if not data_ok:
            item["batchId"] = None
            set_state(item, "blocked", "archive_corrupt"); self.store.save(); return True
        chunk = limits.get("chunkBytes", CHUNK_MAX) if isinstance(limits, dict) else CHUNK_MAX
        chunk = min(chunk, CHUNK_MAX) if integer(chunk) and chunk else CHUNK_MAX
        form = {key: item[key] for key in ("requestId", "fingerprint", "kind", "name", "title", "size", "sha256")}
        form["baseRevision"] = item["baseRevision"] if item["kind"] == "push" else ""
        item["startSent"] = True; self.store.save()
        pin = device["certSha256"]
        try:
            response = self.call(address, "POST", "/lan/v1/uploads/start", form, pin=pin, device=device)
        except RemoteFailure as error:
            if error.code == "not_accepted":
                # The reader dropped the accepted batch (N was switched off).
                item.update(batchId=None, accepted=False)
                set_state(item, "queued", "transfer_interrupted"); self.store.save(); return False
            if error.code in START_REFUSALS:
                item.update(startSent=False, batchId=None)
                set_state(item, "blocked", error.code); self.store.save(); return True
            if error.code in ("busy", "rate_limited", "recovering"):
                set_state(item, "queued", "transfer_interrupted"); self.store.save(); return True
            raise
        except Failure as error:
            if not error.sent:
                item["startSent"] = False
            set_state(item, "queued", "budget_exhausted" if error.code == "budget_exhausted" else "transfer_interrupted")
            self.store.save(); return False
        if response.get("state") == "complete":
            self.complete(device, item, response); return True
        offset = self.progress(item, response)
        if offset is None:
            return True
        try:
            with path.open("rb") as stream:
                stream.seek(offset)
                while offset < item["size"]:
                    if self.clock() >= self.deadline:
                        raise Failure("budget_exhausted")
                    data = stream.read(min(chunk, item["size"] - offset))
                    response = self.call(address, "POST", "/lan/v1/uploads/chunk",
                                         {"requestId": item["requestId"], "offset": offset, "data": base64.b64encode(data).decode("ascii")},
                                         pin=pin, device=device)
                    next_offset = self.progress(item, response)
                    if next_offset != offset + len(data):
                        raise Failure("invalid_upload_status")
                    offset = next_offset
        except RemoteFailure as error:
            if error.code in ("busy", "rate_limited"):
                set_state(item, "queued", "transfer_interrupted"); self.store.save(); return True
            item["batchId"] = None
            set_state(item, "blocked", error.code); self.store.save(); return True
        except Failure as error:
            if error.code in ("invalid_upload_status",):
                item["batchId"] = None
                set_state(item, "blocked", error.code); self.store.save(); return True
            set_state(item, "queued", "budget_exhausted" if error.code == "budget_exhausted" else "transfer_interrupted")
            self.store.save(); return False
        # Persist before the request: from here a lost answer is uncertain.
        item["commitSent"] = True; self.store.save()
        try:
            response = self.call(address, "POST", "/lan/v1/uploads/commit", {"requestId": item["requestId"]}, pin=pin, device=device)
        except RemoteFailure:
            try:
                found = self.lookup(address, device, item)
            except Failure:
                found = None
            if item["state"] == "delivered":
                return True
            if found is not None and found.get("state") == "receiving":
                item["commitSent"] = False
                set_state(item, "queued", "transfer_interrupted")
            else:
                set_state(item, "uncertain", "commit_unconfirmed")
            self.store.save(); return True
        except Failure as error:
            if not error.sent:
                item["commitSent"] = False
                set_state(item, "queued", "budget_exhausted" if error.code == "budget_exhausted" else "transfer_interrupted")
            else:
                set_state(item, "uncertain", "commit_unconfirmed")
            self.store.save(); return False
        if response.get("state") == "complete":
            self.complete(device, item, response); return True
        if self.progress(item, response) is None:
            return True
        self.finish_checking(address, device, item)
        self.store.save()
        return item["state"] == "delivered"

    def upload_batch(self, address, device, caps, entries):
        for item in entries:
            if item["state"] != "queued":
                continue
            try:
                ok = self.upload(address, device, item, caps.get("limits", {}))
            except Failure as error:
                if error.code not in ("invalid_receipt", "invalid_upload_status", "invalid_response"):
                    raise
                # The reader answered inconsistently: never call this delivered.
                item["batchId"] = None
                set_state(item, "uncertain" if item["startSent"] else "blocked", error.code)
                self.store.save()
                ok = False
            if not ok:
                rest = [i for i in entries if i["state"] == "queued" and i is not item]
                code = item.get("code") if item["state"] == "queued" else "transfer_interrupted"
                self.mark(rest, code or "transfer_interrupted", "" if item.get("batchId") is None else None)
                return False
        return True

    # ---------------------------------------------------------------- flows
    def run_paired(self, device, *, host=None, batch_id=None, reoffer=False):
        device_id = device["deviceId"]
        try:
            address, caps = self.reach(device, host)
        except Failure as error:
            if error.code == "device_unreachable":
                self.mark([i for i in self.items_for(device_id) if i["state"] == "queued"], "device_unreachable")
                self.result["next"] = "device_unreachable"
                return
            raise
        if not self.resolve_uncertain(address, device):
            self.result["next"] = "uncertain"
            return
        # Continue batches the reader may still hold, oldest first.
        live = []
        for item in self.items_for(device_id):
            if item["state"] == "queued" and item.get("batchId") and item["batchId"] not in live:
                live.append(item["batchId"])
        if batch_id:
            live = [b for b in live if b == batch_id]
        for current in live:
            entries = [i for i in self.items_for(device_id) if i.get("batchId") == current and i["state"] == "queued"]
            self.result["batchId"] = current
            outcome = self.await_acceptance(address, device, current, entries)
            if outcome == "lost":
                continue   # interrupted entries are offered again below
            if outcome != "accepted":
                self.result["next"] = entries[0].get("code") if entries else None
                return
            if not self.upload_batch(address, device, caps, entries):
                return
        if batch_id:
            return
        while self.waiting() or not self.offered:
            entries = self.next_batch(device_id, reoffer)
            if not entries:
                return
            batch, admitted = self.offer(address, device, entries)
            if batch is None:
                if any(i["state"] == "queued" for i in entries):
                    self.result["next"] = next(i.get("code") for i in entries if i["state"] == "queued")
                    return
                continue
            self.result["batchId"] = batch
            if self.notify:
                self.notify("awaiting_acceptance", batch=batch, count=len(admitted))
            if self.await_acceptance(address, device, batch, admitted) != "accepted":
                self.result["next"] = admitted[0].get("code")
                return
            if not self.upload_batch(address, device, caps, admitted):
                return

    def first_contact(self, label, *, host=None, wait_for_acceptance=False, reoffer=False):
        """Offer unassigned entries (possibly none) to an unpaired reader."""
        clean_sender_label(label)
        address, device_id, observed, revision = self.discover(host)
        entries = self.next_batch(None, reoffer)
        for item in entries:
            if item["kind"] == "push" and item["baseRevision"] is None:
                item["baseRevision"] = revision; item["fingerprint"] = fingerprint(item)
        nonce = secrets.token_bytes(32)
        form = {"label": label, "commit": commitment(nonce)}
        form.update({"e%d" % n: pack_entry(item) for n, item in enumerate(entries)})
        self.offered.update(i["requestId"] for i in entries)
        try:
            response = self.call(address, "POST", "/lan/v2/batch/offer", form, pin=observed)
        except RemoteFailure as error:
            if error.code in ("batch_busy", "client_limit", "rate_limited", "recovering"):
                self.mark(entries, error.code); self.result["next"] = error.code
                return
            raise
        except Failure as error:
            if error.sent:
                self.mark(entries, "not_accepted"); self.result["next"] = "not_accepted"
                return
            raise
        admitted = self.admit(address, None, entries, response)
        if response.get("state") == "blocked":
            self.store.save(); self.result["next"] = "blocked"
            return
        batch_id, pairing_id = response.get("batchId"), response.get("pairingId")
        device_nonce = response.get("deviceNonce")
        if (response.get("state") != "awaiting_reveal" or response.get("deviceId") != device_id or not hex_value(batch_id, 16)
                or not hex_value(pairing_id, 16) or not hex_value(device_nonce, 64) or (entries and not admitted)):
            raise Failure("invalid_response")
        record = {"batchId": batch_id, "pairingId": pairing_id, "senderNonce": nonce.hex(), "deviceNonce": device_nonce,
                  "certSha256": observed, "deviceId": device_id, "host": address[0], "port": address[1],
                  "label": label, "created": int(time.time())}
        self.store.db["pending"].append(record)
        for item in admitted:
            item["device"] = device_id
        self.mark(admitted, "awaiting_pairing_confirmation", batch_id)
        try:
            self.call(address, "POST", "/lan/v2/pair/reveal", {"pairingId": pairing_id, "senderNonce": nonce.hex()}, pin=observed)
        except Failure as error:
            self.store.db["pending"].remove(record)
            for item in admitted:
                item["device"] = None
            self.mark(admitted, "not_accepted", "")
            self.result["next"] = "not_accepted"
            if isinstance(error, RemoteFailure) or error.sent:
                return
            raise
        sas = sas_text(sas_value(bytes.fromhex(observed), nonce, bytes.fromhex(device_nonce), pairing_id, label))
        self.result.update(batchId=batch_id, sas=sas, device=device_id, label=label, next="awaiting_pairing_confirmation")
        if self.notify:
            self.notify("awaiting_pairing_confirmation", batch=batch_id, sas=sas, label=label, count=len(admitted))
        if wait_for_acceptance:
            return self.continue_first_contact(record)
        return None

    def continue_first_contact(self, record):
        """Poll with the Ns proof; save the device only after acceptance."""
        address, pin = (record["host"], record["port"]), record["certSha256"]
        nonce = bytes.fromhex(record["senderNonce"])
        path = "/lan/v2/batch/status?batchId=%s&proof=%s" % (record["batchId"], status_proof(nonce, record["batchId"]))
        entries = [i for i in self.store.db["items"] if i.get("batchId") == record["batchId"] and i["state"] == "queued"]
        self.result.update(batchId=record["batchId"], device=record["deviceId"], label=record["label"])

        def end(code):
            self.store.db["pending"].remove(record)
            for item in entries:
                item["device"] = None
            self.mark(entries, code, "")
            self.result["next"] = code
        unreachable = False
        while True:
            try:
                response = self.call(address, "GET", path, pin=pin)
                unreachable = False
            except RemoteFailure as error:
                if error.status == 404 or error.code == "unknown_batch":
                    end("not_accepted"); return None
                if error.code not in ("rate_limited", "recovering"):
                    raise
                response = None
            except Failure as error:
                if error.code == "pin_mismatch":
                    raise
                if error.code not in ("device_unreachable", "connection_lost", "timeout"):
                    raise
                response, unreachable = None, True
            state = response.get("state") if response else None
            if state == "accepted":
                break
            if state == "expired":
                end("not_accepted"); return None
            if state not in (None, "awaiting_acceptance"):
                raise Failure("invalid_response")
            if not self.waiting():
                code = "device_unreachable" if unreachable else "awaiting_pairing_confirmation"
                self.mark(entries, code)
                self.result["next"] = code
                return None
            self.pause(POLL_SECONDS)
        client_id, token = response.get("clientId"), response.get("token")
        if response.get("deviceId") != record["deviceId"] or not hex_value(client_id, 16) or not hex_value(token, 64) or not integer(response.get("pushRevision")):
            raise Failure("invalid_response")
        device = {"deviceId": record["deviceId"], "host": record["host"], "port": record["port"], "certSha256": pin,
                  "clientId": client_id, "token": token, "pushRevision": response["pushRevision"], "label": record["label"]}
        self.store.db["devices"][record["deviceId"]] = device
        self.store.db["pending"].remove(record)
        self.store.save()
        self.result.pop("sas", None)
        self.result["paired"] = True
        self.result["next"] = None
        for item in entries:
            item["accepted"] = True
        self.store.save()
        caps = {"limits": {"chunkBytes": CHUNK_MAX}}
        if entries:
            self.upload_batch(address, device, caps, entries)
        return device

    def sync_pending(self, batch_id=None):
        """Continue first contacts. Returns the newest paired device, or None."""
        device = None
        for record in [r for r in self.store.db["pending"] if batch_id in (None, r["batchId"])]:
            device = self.continue_first_contact(record) or device
            if record in self.store.db["pending"]:
                break
        return device


# -------------------------------------------------------------------- output

ADVICE = {
    "device_not_found": "On the reader: Settings, then press A until N.",
    "device_unreachable": "On the reader: Settings, then press A until N.",
    "awaiting_acceptance": "On the reader, short-press A+B to accept, then stay until it shows Done.",
    "awaiting_pairing_confirmation": "If the reader shows the same number, short-press A+B and stay until Done; otherwise switch off N.",
    "not_accepted": "It was not accepted on the reader (cancelled, timed out, or the connection dropped).",
    "transfer_interrupted": "The transfer stopped before completion; nothing was published. Return to N to continue.",
    "budget_exhausted": "The transfer stopped before completion; nothing was published. Return to N to continue.",
    "uncertain": "The reader may or may not have published an entry. Return to N to check its receipt.",
    "batch_busy": "The reader is busy with another sender's files.",
    "multiple_devices": "More than one reader answered. Use --host with the IP shown on the reader's N screen.",
}


def summarize(items, result):
    counts = {state: 0 for state in ("delivered", "queued", "uncertain", "blocked")}
    for item in items:
        if item["state"] in counts:
            counts[item["state"]] += 1
    result["items"] = [public_item(i) for i in items]
    result["summary"] = dict(counts, total=len(items))
    if "next" not in result or result["next"] is None:
        queued = [i for i in items if i["state"] == "queued"]
        result["next"] = (queued[0].get("code") if queued else None) or ("uncertain" if counts["uncertain"] else None)
    waiting = result["next"] in ("awaiting_pairing_confirmation", "awaiting_acceptance", "device_unreachable",
                                 "device_not_found", "batch_busy")
    result["state"] = "pending" if counts["queued"] or counts["uncertain"] or waiting else "done"
    return result


def emit(result, as_json, stream=None):
    stream = stream or sys.stdout
    if as_json:
        stream.write(json.dumps(result, ensure_ascii=True, separators=(",", ":")) + "\n")
        return
    lines = []
    if result.get("state") == "error":
        lines.append("error: %s" % result.get("code"))
        for candidate in result.get("candidates", []):
            lines.append("  reader %s at %s" % (candidate["deviceId"], candidate["host"]))
    for item in result.get("items", []):
        label = item["name"] if item["kind"] == "file" else "Push" + (": " + item["title"] if item.get("title") else "")
        lines.append("%-10s %s%s" % (item["state"], label, "  (%s)" % item["code"] if item.get("code") else ""))
    for device in result.get("devices", []):
        lines.append("reader %s  %s  at %s" % (device["deviceId"], device.get("label") or "", device["host"]))
    if result.get("sas"):
        lines.append("New sender %s · %s" % (result.get("label", ""), result["sas"]))
    advice = ADVICE.get(result.get("next") or result.get("code"))
    if advice:
        lines.append(advice)
    text = "\n".join(lines) + "\n"
    try:
        stream.write(text)
    except UnicodeEncodeError:
        encoding = getattr(stream, "encoding", None) or "ascii"
        stream.write(text.encode(encoding, "replace").decode(encoding, "replace"))
    stream.flush()


# ----------------------------------------------------------------------- CLI

def choose_device(store, device_id=None):
    devices = store.db["devices"]
    if device_id:
        if device_id not in devices:
            raise Failure("unknown_device")
        return devices[device_id]
    if len(devices) == 1:
        return next(iter(devices.values()))
    if devices:
        raise Failure("ambiguous_device")
    return None


def target_for_host(session, store, host):
    """Identify the reader at --host by its certificate before any credential."""
    address = endpoint(host, PORT)
    device_id, observed, _ = session.hello(address)
    device = store.db["devices"].get(device_id)
    return device if device and hmac.compare_digest(device["certSha256"], observed) else None


def parser():
    root = argparse.ArgumentParser(prog="paperdiy", description=__doc__.splitlines()[0])
    root.add_argument("--state-dir", default=None, help=argparse.SUPPRESS)
    sub = root.add_subparsers(dest="command", required=True)

    def common(p, *, target=True, timing=True):
        p.add_argument("--json", action="store_true")
        if target:
            group = p.add_mutually_exclusive_group()
            group.add_argument("--device")
            group.add_argument("--host")
            p.add_argument("--label")
        if timing:
            p.add_argument("--wait", type=int, default=120)
            p.add_argument("--budget", type=int, default=600)
    p = sub.add_parser("push"); p.add_argument("source"); p.add_argument("--title", default=""); common(p)
    p = sub.add_parser("send"); p.add_argument("sources", nargs="+"); common(p)
    p = sub.add_parser("sync"); p.add_argument("--batch-id"); p.add_argument("--reoffer", action="store_true"); common(p)
    p = sub.add_parser("pair"); p.add_argument("--label", required=True); p.add_argument("--host"); p.add_argument("--json", action="store_true")
    p.add_argument("--wait", type=int, default=120); p.add_argument("--budget", type=int, default=600)
    p = sub.add_parser("status"); p.add_argument("--json", action="store_true")
    p = sub.add_parser("endpoint"); p.add_argument("--host", required=True); p.add_argument("--device"); p.add_argument("--json", action="store_true")
    p = sub.add_parser("revoke"); p.add_argument("--device"); p.add_argument("--json", action="store_true")
    for name in ("cancel", "abandon"):
        p = sub.add_parser(name); p.add_argument("--request-id", required=True); p.add_argument("--json", action="store_true")
    p = sub.add_parser("prune"); p.add_argument("--delivered", action="store_true", required=True); p.add_argument("--json", action="store_true")
    return root


def adopt(store, device):
    """Entries queued before any pairing go to the reader this command reaches."""
    for item in store.db["items"]:
        if item.get("device") is None and item["state"] == "queued" and not item.get("batchId"):
            item["device"] = device["deviceId"]


def deliver(store, args, session):
    """Shared step for push/send/sync: pick the reader, then offer and upload."""
    batch_id = getattr(args, "batch_id", None)
    reoffer = getattr(args, "reoffer", False)
    pending = store.db["pending"]
    if pending and (batch_id is None or any(r["batchId"] == batch_id for r in pending)):
        # A first contact still open on the reader comes first: a new offer
        # would only meet its prompt (batch_busy).
        device = session.sync_pending(batch_id)
        if batch_id or store.db["pending"] or device is None or not session.waiting():
            return
        adopt(store, device)
        session.run_paired(device, reoffer=reoffer)
        return
    device = target_for_host(session, store, args.host) if args.host else choose_device(store, args.device)
    if device is None:
        if args.command == "sync" and not session.next_batch(None, reoffer):
            return
        if not args.label:
            raise Failure("label_required")
        device = session.first_contact(args.label, host=args.host, wait_for_acceptance=not args.json, reoffer=reoffer)
        if device is None or not session.waiting():
            return
    adopt(store, device)
    session.run_paired(device, host=args.host, batch_id=batch_id, reoffer=reoffer)


def main(argv=None, *, transport=None, resolver=None, clock=time.monotonic, sleep=time.sleep, stdout=None):
    args = parser().parse_args(argv)
    as_json = getattr(args, "json", False)
    stdout = stdout or sys.stdout
    try:
        if hasattr(args, "wait") and (not 0 <= args.wait <= 600 or not 1 <= args.budget <= 600 or args.wait > args.budget):
            raise Failure("invalid_budget")
        with Store(args.state_dir) as store:
            def notify(kind, **values):
                if not as_json and kind == "awaiting_pairing_confirmation":
                    emit({"sas": values["sas"], "label": values["label"], "next": kind}, False, stdout)
                elif not as_json:
                    emit({"next": kind}, False, stdout)
            make = lambda wait=0, budget=60: Session(store, transport=transport, resolver=resolver, wait=wait, budget=budget,
                                                    clock=clock, sleep=sleep, notify=notify)
            result = {}
            if args.command in ("push", "send", "sync", "pair"):
                session = make(args.wait, args.budget)
                if args.command == "pair":
                    session.first_contact(args.label, host=args.host, wait_for_acceptance=not as_json)
                    scope = [i for i in store.db["items"] if i.get("batchId") == session.result.get("batchId")]
                else:
                    before = {i["requestId"]: (i["state"], i.get("code")) for i in store.db["items"]}
                    if args.command in ("push", "send") and args.wait and not store.db["devices"] and not store.db["pending"] and not args.label:
                        raise Failure("label_required")
                    if args.command in ("push", "send"):
                        target = None if args.host else choose_device(store, args.device)
                        staged = store.enqueue(target["deviceId"] if target else None,
                                               [args.source] if args.command == "push" else args.sources,
                                               "push" if args.command == "push" else "file",
                                               args.title if args.command == "push" else "")
                        scope_ids = {i["requestId"] for i in staged}
                    else:
                        scope_ids = None
                    if args.wait:
                        deliver(store, args, session)
                    if scope_ids is None:
                        # sync reports what is still open plus what changed now.
                        scope = [i for i in store.db["items"] if i["requestId"] in before and
                                 (i["state"] not in TERMINAL or before[i["requestId"]] != (i["state"], i.get("code")))]
                    else:
                        scope = [i for i in store.db["items"] if i["requestId"] in scope_ids]
                result = summarize(scope, dict(session.result))
                if args.command == "pair" and result.get("paired"):
                    result["state"] = "done"
            elif args.command == "status":
                result = {"state": "local",
                          "devices": [{k: d[k] for k in ("deviceId", "label", "host", "port", "pushRevision")} for d in store.paired()],
                          "pending": [{"batchId": r["batchId"], "deviceId": r["deviceId"], "label": r["label"]} for r in store.db["pending"]],
                          "items": [public_item(i) for i in store.db["items"]]}
            elif args.command == "endpoint":
                device = choose_device(store, args.device)
                if device is None:
                    raise Failure("no_paired_device")
                session = make(0, 30)
                session.reach(device, endpoint(args.host)[0])
                result = {"state": "endpoint_updated", "deviceId": device["deviceId"], "host": device["host"]}
            elif args.command == "revoke":
                device = choose_device(store, args.device)
                if device is None:
                    raise Failure("no_paired_device")
                session = make(10, 30)
                try:
                    address, _ = session.reach(device)
                    response = session.call(address, "POST", "/lan/v1/clients/revoke", {"clientId": device["clientId"]}, pin=device["certSha256"], device=device)
                    if response.get("revoked") is not True:
                        raise Failure("revoke_uncertain")
                except Failure as error:
                    if error.sent or error.code == "unauthorized":
                        raise Failure("revoke_uncertain") from None
                    raise
                del store.db["devices"][device["deviceId"]]
                for item in store.db["items"]:
                    if item.get("device") == device["deviceId"] and item["state"] == "queued":
                        item.update(device=None, batchId=None)
                store.save()
                result = {"state": "revoked", "deviceId": device["deviceId"]}
            elif args.command in ("cancel", "abandon"):
                item = next((i for i in store.db["items"] if i["requestId"] == args.request_id), None)
                if not item or item["state"] in ("delivered", "superseded", "cancelled", "abandoned"):
                    raise Failure("unknown_pending_request")
                if args.command == "abandon":
                    outcome = "unknown" if item["startSent"] else "not_sent"
                    item["batchId"] = None
                    set_state(item, "abandoned"); store.save()
                    result = {"state": "abandoned", "requestId": item["requestId"], "remoteOutcome": outcome}
                elif not item["startSent"]:
                    item["batchId"] = None
                    set_state(item, "cancelled"); store.save()
                    result = summarize([item], {})
                else:
                    device = store.db["devices"].get(item.get("device"))
                    if device is None:
                        raise Failure("no_paired_device")
                    session = make(10, 30)
                    try:
                        address, _ = session.reach(device)
                        response = session.call(address, "POST", "/lan/v1/uploads/cancel", {"requestId": item["requestId"]}, pin=device["certSha256"], device=device)
                        if response.get("state") == "complete":
                            session.complete(device, item, response)
                        elif response.get("state") == "cancelled":
                            item["batchId"] = None
                            set_state(item, "cancelled")
                        else:
                            set_state(item, "uncertain", "cancel_unconfirmed")
                    except RemoteFailure as error:
                        set_state(item, "cancelled" if error.code == "cancelled" else "uncertain",
                                  None if error.code == "cancelled" else "cancel_unconfirmed")
                    except Failure as error:
                        if error.sent:
                            set_state(item, "uncertain", "cancel_unconfirmed")
                        else:
                            raise
                    store.save()
                    result = summarize([item], {})
            elif args.command == "prune":
                pruned = [i for i in store.db["items"] if i["state"] == "delivered"]
                store.db["items"] = [i for i in store.db["items"] if i["state"] != "delivered"]
                store.save()
                for item in pruned:
                    store.discard(store.body(item))
                result = {"state": "pruned", "count": len(pruned)}
        emit(result, as_json, stdout)
        items = result.get("items", [])
        return 2 if any(i["state"] in ("uncertain", "blocked") for i in items) else 0
    except Failure as error:
        result = {"state": "error", "code": error.code}
        if error.code == "multiple_devices" and isinstance(error.detail, list):
            result["candidates"] = [{"deviceId": c["deviceId"], "host": c["host"]} for c in error.detail]
        emit(result, as_json, stdout)
        return 2
    except (OSError, KeyError, TypeError, ValueError, ImportError):
        # Paths, sources and credentials never escape through exception text.
        emit({"state": "error", "code": "local_state_error"}, as_json, stdout)
        return 2


if __name__ == "__main__":
    sys.exit(main())
