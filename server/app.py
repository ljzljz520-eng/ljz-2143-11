"""
HTTP API + web admin for background resource publishing.

Routes
------
GET  /                                   web admin
GET  /static/*                            static assets
POST /api/resources                       upload + sniff + decode-budget check
GET  /api/state                           revisions / devices / delivery / events
POST /api/revisions/<id>/dispatch         create per-device delivery records
POST /api/revisions/<id>/revoke           withdraw a resource
POST /api/devices/<id>/authorization      authorize / deauthorize a device
POST /api/devices/poll                    device long-ish poll (JSON)
GET  /artifacts/<id>/<file>               download (supports ?delay_ms=)
GET  /api/revisions/<id>/preview.png      decoded preview bytes
"""

from __future__ import annotations

import base64
import hashlib
import json
import os
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

from . import imaging
from .db import DB
from .multipart import parse_multipart

STATIC_DIR = os.path.join(os.path.dirname(__file__), "static")

# State reported by terminals is mapped onto the delivery state machine.
TERMINAL_STATES = {
    "idle": "pending",
    "fetching": "fetching",
    "ready": "ready",
    "switch_pending": "switch_pending",
    "showing": "showing",
    "error": "error",
}


class AppContext:
    def __init__(self, db: DB, data_dir: str):
        self.db = db
        self.data_dir = data_dir
        self.artifacts_dir = os.path.join(data_dir, "artifacts")
        os.makedirs(self.artifacts_dir, exist_ok=True)
        # revision_id -> per-request simulated download delay (test support)
        self.delay_ms: dict[str, int] = {}
        self.lock = threading.Lock()


def _json_response(handler: BaseHTTPRequestHandler, status: int,
                   payload) -> None:
    body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    handler.send_response(status)
    handler.send_header("Content-Type", "application/json; charset=utf-8")
    handler.send_header("Content-Length", str(len(body)))
    handler.send_header("Cache-Control", "no-store")
    handler.end_headers()
    handler.wfile.write(body)


def _read_json(handler: BaseHTTPRequestHandler):
    length = int(handler.headers.get("Content-Length", "0"))
    raw = handler.rfile.read(length) if length > 0 else b"{}"
    try:
        return json.loads(raw.decode("utf-8") or "{}")
    except json.JSONDecodeError:
        return {}


def _state_snapshot(ctx: AppContext) -> dict:
    revisions = [dict(r) for r in ctx.db.query(
        "SELECT * FROM revisions ORDER BY seq DESC")]
    devices = [dict(d) for d in ctx.db.query(
        "SELECT * FROM devices ORDER BY first_seen")]
    for d in devices:
        d["formats"] = json.loads(d["formats"])
    delivery = [dict(x) for x in ctx.db.query(
        "SELECT * FROM delivery ORDER BY updated_at DESC")]
    events = [dict(e) for e in ctx.db.query(
        "SELECT * FROM events ORDER BY id DESC LIMIT 60")]
    latest = ctx.db.latest_revision()
    latest_any = ctx.db.query_one(
        "SELECT * FROM revisions ORDER BY seq DESC LIMIT 1")
    return {
        "latest_revision": latest["id"] if latest else None,
        "latest_revision_any":
            latest_any["id"] if latest_any else None,
        "latest_revoked":
            bool(latest_any["revoked"]) if latest_any else False,
        "revisions": revisions,
        "devices": devices,
        "delivery": delivery,
        "events": events,
        "limits": {
            "max_pixels": imaging.MAX_PIXELS,
            "max_dimension": imaging.MAX_DIMENSION,
            "max_compressed_bytes": imaging.MAX_COMPRESSED_BYTES,
        },
    }


# ----------------------------------------------------------------------
# revision creation / dispatch / revoke
# ----------------------------------------------------------------------

def create_revision(ctx: AppContext, filename: str, payload: bytes,
                    policy: str, delay_ms: int) -> dict:
    """Sniff -> decode-budget check -> persist bytes -> create revision."""
    try:
        valid = imaging.validate(payload)
    except imaging.UploadError as exc:
        ctx.db.event("upload_rejected", detail=f"{filename}: {exc.message}")
        raise

    sha = hashlib.sha256(payload).hexdigest()
    db = ctx.db

    row = db.query_one("SELECT COALESCE(MAX(seq), 0) AS s FROM revisions")
    seq = row["s"] + 1
    revision_id = f"rev-{seq:04d}-{sha[:8]}"

    ext = {"png": "png", "jpeg": "jpg", "webp": "webp"}[valid.fmt]
    original_name = f"{revision_id}-original.{ext}"
    original_path = imaging.save_bytes(
        ctx.artifacts_dir, original_name, payload)

    png_path = None
    if policy == "server":
        # Canonical transcode: every device decodes the same PNG.
        png_bytes = imaging.transcode_png(valid.image)
        png_path = imaging.save_bytes(
            ctx.artifacts_dir, f"{revision_id}.png", png_bytes)

    preview = imaging.preview_png(valid.image)
    preview_name = f"{revision_id}-preview.png"
    imaging.save_bytes(ctx.artifacts_dir, preview_name, preview)
    preview_b64 = base64.b64encode(preview).decode("ascii")

    cur = db.execute(
        "INSERT INTO resources(created_at) VALUES (?)", (time.time(),))
    resource_id = cur.lastrowid
    db.execute(
        "INSERT INTO revisions(id, resource_id, seq, created_at,"
        " uploaded_name, original_format, width, height, pixels, sha256,"
        " size, original_path, png_path, preview_path, width_cap)"
        " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
        (revision_id, resource_id, seq, time.time(), filename, valid.fmt,
         valid.width, valid.height, valid.pixels, sha, len(payload),
         os.path.basename(original_path),
         os.path.basename(png_path) if png_path else None,
         preview_name, imaging.MAX_DIMENSION),
    )
    db.event("revision_created", revision_id=revision_id,
             detail=f"{filename} sniffed={valid.fmt} "
                    f"{valid.width}x{valid.height} policy={policy}")

    # API creates the resource revision; publishing immediately creates
    # per-device delivery rows for every authorized device.
    devices = db.query("SELECT device_id FROM devices WHERE authorized=1")
    targets = [d["device_id"] for d in devices]
    for device_id in targets:
        db.upsert_delivery(device_id, revision_id, "pending")
    if targets:
        db.event("revision_dispatched", revision_id=revision_id,
                 detail=f"{len(targets)} authorized device(s)")

    if delay_ms:
        with ctx.lock:
            ctx.delay_ms[revision_id] = delay_ms

    return {
        "revision_id": revision_id,
        "sniffed_format": valid.fmt,
        "declared_extension": os.path.splitext(filename)[1].lstrip("."),
        "width": valid.width,
        "height": valid.height,
        "pixels": valid.pixels,
        "sha256": sha,
        "size": len(payload),
        "policy": policy,
        "original_retained": True,
        "transcoded_png": png_path is not None,
        "preview_url": f"/api/revisions/{revision_id}/preview.png",
        "preview_png_data_url": f"data:image/png;base64,{preview_b64}",
        "targets": targets,
        "delivery_state": "pending",
        "delay_ms": delay_ms,
    }


def _artifact_for_device(ctx: AppContext, revision, device) -> dict | None:
    """
    Choose the bytes this device should fetch.
      * policy=server   : canonical PNG (format consistency across fleet)
      * policy=terminal : original bytes; if the device cannot decode the
        original format a server PNG is used when available, otherwise the
        device is told the content is incompatible.
    """
    if revision["png_path"]:
        return {
            "artifact_url": f"/artifacts/{revision['id']}/"
                            f"{revision['png_path']}",
            "sha256": _file_sha(ctx, revision["png_path"]),
            "size": _file_size(ctx, revision["png_path"]),
            "format": "png",
            "source": "transcoded",
        }

    # device row stores JSON; compare against the parsed list.
    formats = set(json.loads(device["formats"]))
    if revision["original_format"] in formats:
        return {
            "artifact_url": f"/artifacts/{revision['id']}/"
                            f"{revision['original_path']}",
            "sha256": revision["sha256"],
            "size": revision["size"],
            "format": revision["original_format"],
            "source": "original",
        }
    return None


def _file_sha(ctx: AppContext, name: str) -> str:
    path = os.path.join(ctx.artifacts_dir, name)
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def _file_size(ctx: AppContext, name: str) -> int:
    return os.path.getsize(os.path.join(ctx.artifacts_dir, name))


# ----------------------------------------------------------------------
# polling
# ----------------------------------------------------------------------

def handle_poll(ctx: AppContext, body: dict) -> tuple[int, dict]:
    db = ctx.db
    device_id = str(body.get("device_id", "")).strip()
    if not device_id:
        return 400, {"error": "device_id required"}
    name = str(body.get("name", device_id))[:128]
    formats = body.get("formats") or ["png", "jpeg"]
    formats = [str(f) for f in formats][:16]
    max_pixels = int(body.get("max_pixels", imaging.MAX_PIXELS))
    reported_state = str(body.get("state", "idle"))
    reported_error = str(body.get("error", "none"))
    actual = str(body.get("actual_revision", ""))

    now = time.time()
    device = db.query_one(
        "SELECT * FROM devices WHERE device_id=?", (device_id,))
    if device is None:
        # First contact: auto-authorize so freshly flashed terminals work;
        # an admin can deauthorize afterwards.
        db.execute(
            "INSERT INTO devices(device_id, name, formats, max_pixels,"
            " authorized, first_seen, last_seen) VALUES (?,?,?,?,1,?,?)",
            (device_id, name, json.dumps(formats), max_pixels, now, now),
        )
        db.event("device_registered", device_id=device_id, detail=name)
    else:
        db.execute(
            "UPDATE devices SET name=?, formats=?, max_pixels=?, last_seen=?"
            " WHERE device_id=?",
            (name, json.dumps(formats), max_pixels, now, device_id),
        )
    device = db.query_one(
        "SELECT * FROM devices WHERE device_id=?", (device_id,))

    # Latest revision regardless of revoked: a device showing it must learn
    # about the withdrawal; a fresh device without context should get no
    # desired content instead.
    latest_any = db.query_one(
        "SELECT * FROM revisions ORDER BY seq DESC LIMIT 1")
    latest = db.latest_revision()
    response = {
        "authorized": bool(device["authorized"]),
        "desired": None,
        "revoked": False,
        "server_time": now,
    }

    if not device["authorized"]:
        db.upsert_delivery(device_id,
                           latest_any["id"] if latest_any else "-",
                           "unauthorized")
        db.event("poll_unauthorized", device_id=device_id)
        return 200, response

    if latest_any is not None and latest_any["revoked"]:
        # Only meaningful for devices that were actually tracking it.
        affected = actual == latest_any["id"] or db.query_one(
            "SELECT 1 FROM delivery WHERE device_id=? AND revision_id=?",
            (device_id, latest_any["id"])) is not None
        if affected:
            response["revoked"] = True
            db.upsert_delivery(device_id, latest_any["id"], "revoked",
                               actual=actual)
            db.event("poll_revoked", device_id=device_id,
                     revision_id=latest_any["id"],
                     detail=f"actual={actual or '-'}")
        return 200, response

    if latest is None:
        return 200, response

    # Device capability gap in terminal-decode policy: record the exact
    # class of error; the original bytes remain available to other devices.
    art = _artifact_for_device(ctx, latest, device)
    if art is None:
        db.upsert_delivery(device_id, latest["id"], "error",
                           error_code="content_unsupported", actual=actual)
        response["desired"] = {
            "revision": latest["id"],
            "incompatible": True,
            "reason": f"device cannot decode {latest['original_format']}",
            "artifact_url": "",
            "sha256": "",
            "size": 0,
            "source": "original",
        }
        return 200, response

    with ctx.lock:
        delay = ctx.delay_ms.get(latest["id"], 0)
    if delay:
        art["artifact_url"] += f"?delay_ms={delay}"

    desired = {"revision": latest["id"], **art, "incompatible": False}
    response["desired"] = desired

    delivery_state = TERMINAL_STATES.get(reported_state, "pending")
    error_code = ""
    if delivery_state == "error":
        error_code = reported_error if reported_error != "none" else \
            "content_unsupported"
    if actual != latest["id"] and delivery_state == "showing":
        # Stale actual: the device still paints the old revision while the
        # new one is pending; do not mis-label the new row as showing.
        delivery_state = "pending"
    db.upsert_delivery(device_id, latest["id"], delivery_state,
                       error_code=error_code, actual=actual,
                       is_latest=True)

    # Earlier revisions this device is still on become obsolete.
    db.execute(
        "UPDATE delivery SET state='obsolete', updated_at=?"
        " WHERE device_id=? AND revision_id != ? AND state NOT IN"
        " ('obsolete','revoked','unauthorized')",
        (now, device_id, latest["id"]),
    )
    return 200, response


# ----------------------------------------------------------------------
# HTTP handler
# ----------------------------------------------------------------------

class Handler(BaseHTTPRequestHandler):
    ctx: AppContext = None  # type: ignore

    def log_message(self, fmt, *args):  # quiet but keep errors visible
        if os.environ.get("BG_HTTP_LOG"):
            super().log_message(fmt, *args)

    def _serve_file(self, path: str, ctype: str, cache: str = "no-store"):
        try:
            with open(path, "rb") as f:
                data = f.read()
        except OSError:
            _json_response(self, 404, {"error": "not found"})
            return
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", cache)
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path
        qs = parse_qs(parsed.query)

        if path in ("/", "/index.html"):
            self._serve_file(os.path.join(STATIC_DIR, "index.html"),
                             "text/html; charset=utf-8")
            return
        if path.startswith("/static/"):
            name = os.path.basename(path)
            self._serve_file(os.path.join(STATIC_DIR, name),
                             "application/octet-stream")
            return
        if path == "/api/state":
            _json_response(self, 200, _state_snapshot(self.ctx))
            return
        if path.startswith("/artifacts/"):
            self._serve_artifact(path, qs)
            return
        if path.startswith("/api/revisions/") and path.endswith("/preview.png"):
            rev_id = path.split("/")[3]
            rev = self.ctx.db.revision(rev_id)
            if rev is None or not rev["preview_path"]:
                _json_response(self, 404, {"error": "unknown revision"})
                return
            self._serve_file(
                os.path.join(self.ctx.artifacts_dir, rev["preview_path"]),
                "image/png", cache="public, max-age=3600")
            return
        _json_response(self, 404, {"error": "not found"})

    def _serve_artifact(self, path: str, qs: dict):
        parts = path.split("/")
        # /artifacts/<revision>/<filename>
        if len(parts) != 4:
            _json_response(self, 404, {"error": "bad artifact path"})
            return
        revision_id, filename = parts[2], parts[3]
        rev = self.ctx.db.revision(revision_id)
        if rev is None:
            _json_response(self, 404, {"error": "unknown revision"})
            return
        allowed = {rev["original_path"], rev["png_path"]}
        if filename not in allowed:
            _json_response(self, 404, {"error": "unknown artifact"})
            return

        delay_ms = 0
        if "delay_ms" in qs:
            try:
                delay_ms = max(0, min(30000, int(qs["delay_ms"][0])))
            except ValueError:
                delay_ms = 0
        else:
            with self.ctx.lock:
                delay_ms = self.ctx.delay_ms.get(revision_id, 0)
        # Used by the "first image arrives late" acceptance scenario.
        time.sleep(delay_ms / 1000.0)

        full = os.path.join(self.ctx.artifacts_dir, filename)
        ctype = {
            ".png": "image/png",
            ".jpg": "image/jpeg",
            ".jpeg": "image/jpeg",
            ".webp": "image/webp",
        }.get(os.path.splitext(filename)[1], "application/octet-stream")
        self._serve_file(full, ctype, cache="public, max-age=300")

    def do_POST(self):
        path = urlparse(self.path).path

        if path == "/api/resources":
            self._post_upload()
            return
        if path == "/api/devices/poll":
            body = _read_json(self)
            status, resp = handle_poll(self.ctx, body)
            _json_response(self, status, resp)
            return
        if path.startswith("/api/revisions/") and path.endswith("/dispatch"):
            self._post_dispatch(path.split("/")[3])
            return
        if path.startswith("/api/revisions/") and path.endswith("/revoke"):
            self._post_revoke(path.split("/")[3])
            return
        if path.startswith("/api/devices/") and path.endswith("/authorization"):
            device_id = path.split("/")[3]
            self._post_authorization(device_id)
            return
        _json_response(self, 404, {"error": "not found"})

    def _post_upload(self):
        ctype = self.headers.get("Content-Type", "")
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length) if length > 0 else b""
        if "multipart/form-data" not in ctype:
            _json_response(self, 400,
                           {"error": "expected multipart/form-data"})
            return
        fields = parse_multipart(body, ctype)
        file_field = next((f for f in fields if f[0] == "file"), None)
        policy_field = next((f for f in fields if f[0] == "policy"), None)
        delay_field = next((f for f in fields if f[0] == "delay_ms"), None)
        if file_field is None or not file_field[3]:
            _json_response(self, 400, {"error": "missing file part"})
            return
        policy = (policy_field[3].decode() if policy_field else
                  "terminal").strip()
        if policy not in ("terminal", "server"):
            policy = "terminal"
        delay_ms = 0
        if delay_field:
            try:
                delay_ms = max(0, min(30000,
                                      int(delay_field[3].decode())))
            except ValueError:
                delay_ms = 0

        _name, filename, _ct, payload = file_field
        try:
            result = create_revision(self.ctx, filename, payload, policy,
                                     delay_ms)
        except imaging.UploadError as exc:
            _json_response(self, 422,
                           {"error": exc.code, "message": exc.message})
            return

        _json_response(self, 201, result)

    def _post_dispatch(self, revision_id: str):
        rev = self.ctx.db.revision(revision_id)
        if rev is None:
            _json_response(self, 404, {"error": "unknown revision"})
            return
        devices = self.ctx.db.query(
            "SELECT * FROM devices WHERE authorized=1")
        targets = []
        for d in devices:
            self.ctx.db.upsert_delivery(d["device_id"], revision_id,
                                       "pending")
            targets.append(d["device_id"])
        self.ctx.db.event("revision_dispatched", revision_id=revision_id,
                          detail=f"{len(targets)} authorized device(s)")
        _json_response(self, 200,
                       {"revision_id": revision_id,
                        "targets": targets,
                        "state": "pending"})

    def _post_revoke(self, revision_id: str):
        rev = self.ctx.db.revision(revision_id)
        if rev is None:
            _json_response(self, 404, {"error": "unknown revision"})
            return
        self.ctx.db.execute(
            "UPDATE revisions SET revoked=1 WHERE id=?", (revision_id,))
        self.ctx.db.execute(
            "UPDATE delivery SET state='revoked', updated_at=?"
            " WHERE revision_id=? AND state NOT IN ('showing')",
            (time.time(), revision_id))
        # showing rows flip on next poll (device must finish final frame)
        self.ctx.db.event("revision_revoked", revision_id=revision_id)
        _json_response(self, 200, {"revision_id": revision_id,
                                   "revoked": True})

    def _post_authorization(self, device_id: str):
        body = _read_json(self)
        authorized = bool(body.get("authorized"))
        cur = self.ctx.db.execute(
            "UPDATE devices SET authorized=? WHERE device_id=?",
            (1 if authorized else 0, device_id))
        if cur.rowcount == 0:
            _json_response(self, 404, {"error": "unknown device"})
            return
        self.ctx.db.event(
            "device_authorization_changed", device_id=device_id,
            detail="authorized" if authorized else "deauthorized")
        _json_response(self, 200, {"device_id": device_id,
                                   "authorized": authorized})


def build_server(host: str, port: int, db_path: str,
                 data_dir: str) -> ThreadingHTTPServer:
    db = DB(db_path)
    ctx = AppContext(db, data_dir)

    class BoundHandler(Handler):
        pass

    BoundHandler.ctx = ctx
    server = ThreadingHTTPServer((host, port), BoundHandler)
    server.ctx = ctx  # type: ignore[attr-defined]
    return server
