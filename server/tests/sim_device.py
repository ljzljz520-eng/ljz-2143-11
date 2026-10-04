#!/usr/bin/env python3
"""
Reference terminal implementing the exact device protocol used by the
C client, so acceptance scenarios can be exercised without SDL.

State machine mirrors src/bg_manager.c:
  idle -> fetching -> ready -> switch_pending -> showing
                 \\-> error (download_incomplete / content_unsupported /
                             vram_alloc_failed)

Rules modelled:
  * content-addressed cache + atomic writes (tmp -> rename)
  * late arrival of rev #1 is discarded when rev #2 is already desired
  * revocation while offline: applied on reconnect; old "texture" is
    released only after a final frame is drawn
  * crash mid cache-write leaves only .part/.tmp, cleaned at next start
  * VRAM failure injection: --vram-fail-at <rev>
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import time
import urllib.request
import urllib.error


class SimDevice:
    def __init__(self, server, cache, name, formats=None,
                 poll_interval=0.3, vram_fail_at=None,
                 die_during_cache_write=False, chunk_delay=0.0):
        self.server = server.rstrip("/")
        self.cache = cache
        self.name = name
        self.formats = formats or ["png", "jpeg"]
        self.poll_interval = poll_interval
        self.vram_fail_at = vram_fail_at
        self.die_during_cache_write = die_during_cache_write
        self.chunk_delay = chunk_delay
        os.makedirs(cache, exist_ok=True)
        self.device_id = self._load_device_id()
        self.state = "idle"
        self.error = "none"
        self.actual = ""
        self.desired = None
        self.revoked = False
        self.frames = 0
        self.cache_committed = False
        self.staged = None         # decoded bytes awaiting first frame
        self.staged_rev = None
        self.staged_sha = None
        self.staged_source = None
        self._cleanup_staging()

    # ---- infra -------------------------------------------------------
    def _load_device_id(self):
        path = os.path.join(self.cache, "device.id")
        if os.path.exists(path):
            return open(path).read().strip()
        did = hashlib.sha256(os.urandom(32)).hexdigest()[:32]
        self._write_atomic(path, did, binary=False)
        return did

    def _cleanup_staging(self):
        for name in os.listdir(self.cache):
            if name.endswith((".part", ".tmp")):
                os.remove(os.path.join(self.cache, name))

    @staticmethod
    def _write_atomic(path, data, binary=True):
        tmp = path + ".tmp"
        with open(tmp, "wb" if binary else "w") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)

    def _post(self, url, payload):
        req = urllib.request.Request(
            url, data=json.dumps(payload).encode(),
            headers={"Content-Type": "application/json"}, method="POST")
        with urllib.request.urlopen(req, timeout=10) as r:
            return json.loads(r.read())

    @staticmethod
    def _get(url):
        with urllib.request.urlopen(url, timeout=20) as r:
            return r.read()

    def _get_streaming(self, url, part_path, abort_check, chunk_delay=0.0):
        """Download in chunks, calling abort_check() between chunks so a
        newer desired revision cancels a slow first-image transfer."""
        with urllib.request.urlopen(url, timeout=20) as r:
            with open(part_path, "wb") as f:
                while True:
                    chunk = r.read(4096)
                    if not chunk:
                        break
                    if abort_check():
                        f.flush()
                        return False
                    f.write(chunk)
                    f.flush()
                    if chunk_delay:
                        time.sleep(chunk_delay)
        return True

    def _blob_path(self, sha):
        return os.path.join(self.cache, sha + ".bin")

    def _part_path(self, sha):
        return os.path.join(self.cache, sha + ".part")

    def _manifest_path(self):
        return os.path.join(self.cache, "manifest.json")

    # ---- main loop ---------------------------------------------------
    def poll_once(self):
        body = {
            "device_id": self.device_id,
            "name": self.name,
            "actual_revision": self.actual,
            "state": self.state,
            "error": self.error,
            "formats": self.formats,
            "max_pixels": 8_300_000,
        }
        resp = self._post(self.server + "/api/devices/poll", body)
        self.revoked = resp.get("revoked", False)
        self.desired = resp.get("desired")
        return resp

    def restore_cache(self):
        """Boot: validate manifest+blob (acceptance: crash during write)."""
        mp = self._manifest_path()
        if not os.path.exists(mp):
            return None
        manifest = json.load(open(mp))
        path = self._blob_path(manifest["sha256"])
        if not os.path.exists(path):
            os.remove(mp)
            return None
        with open(path, "rb") as f:
            data = f.read()
        if hashlib.sha256(data).hexdigest() != manifest["sha256"] or \
                len(data) != manifest["size"]:
            os.remove(path)
            os.remove(mp)
            return None
        self.actual = manifest["revision"]
        self.state = "showing"
        self.frames = 1
        self.cache_committed = True
        return manifest

    def apply_poll(self):
        if self.revoked:
            # Old texture stays until the final frame finishes, then drops.
            if self.staged_rev is not None:
                self.staged = None
                self.staged_rev = None
            if self.actual:
                self.draw_frame(final_before_release=True)
                self.actual = ""
            if os.path.exists(self._manifest_path()):
                os.remove(self._manifest_path())
            self.state = "idle"
            self.error = "none"
            return

        desired = self.desired
        if not desired:
            return
        if desired.get("incompatible"):
            self.state = "error"
            self.error = "content_unsupported"
            return
        rev = desired["revision"]
        if rev == self.actual and self.state == "showing":
            return
        if self.staged_rev == rev and self.state in ("ready",
                                                     "switch_pending"):
            return

        # Newer desired rev supersedes a pending/late artifact.
        if self.staged_rev is not None and self.staged_rev != rev:
            self.staged = None
            self.staged_rev = None
        self.fetch(desired)

    def fetch(self, desired):
        rev = desired["revision"]
        sha = desired["sha256"]
        size = desired["size"]
        self.state = "fetching"
        self.error = "none"

        final = self._blob_path(sha)
        data = None
        if os.path.exists(final):
            with open(final, "rb") as f:
                data = f.read()
        else:
            part = self._part_path(sha)
            url = self.server + desired["artifact_url"]

            def abort_check():
                """True if a newer revision arrived mid-transfer."""
                if abort_rev is None:
                    return False
                try:
                    resp = self._post(
                        self.server + "/api/devices/poll",
                        {"device_id": self.device_id, "name": self.name,
                         "actual_revision": self.actual,
                         "state": "fetching", "error": "none",
                         "formats": self.formats,
                         "max_pixels": 8_300_000})
                    new_desired = resp.get("desired")
                    if resp.get("revoked"):
                        self.revoked = True
                        return True
                    if new_desired and \
                            new_desired.get("revision") != abort_rev:
                        self.desired = new_desired
                        return True
                except (urllib.error.URLError, ConnectionError, OSError):
                    pass
                return False

            abort_rev = rev
            try:
                ok = self._get_streaming(url, part, abort_check,
                                         chunk_delay=self.chunk_delay)
            except (urllib.error.URLError, TimeoutError, ConnectionError):
                self.state = "error"
                self.error = "download_incomplete"
                if os.path.exists(part):
                    os.remove(part)
                return
            if not ok:
                # superseded or revoked mid-download: drop partial bytes
                if os.path.exists(part):
                    os.remove(part)
                return
            with open(part, "rb") as f:
                data = f.read()
            if self.die_during_cache_write:
                # Half-write then hard exit: exact S3 crash scenario.
                with open(part, "wb") as f:
                    f.write(data[: max(1, len(data) // 2)])
                    f.flush()
                    os.fsync(f.fileno())
                sys.stderr.write(
                    "[sim] injected crash during cache write for "
                    + rev + "\n")
                os._exit(3)
            os.replace(part, final)

        if len(data) != size or hashlib.sha256(data).hexdigest() != sha:
            self.state = "error"
            self.error = "download_incomplete"
            if os.path.exists(final):
                os.remove(final)
            return
        if not data.startswith((b"\x89PNG\r\n\x1a\n", b"\xff\xd8\xff",
                                b"RIFF")):
            self.state = "error"
            self.error = "content_unsupported"
            return
        if data[:4] == b"RIFF" and "webp" not in self.formats:
            self.state = "error"
            self.error = "content_unsupported"
            return

        self.staged = data
        self.staged_rev = rev
        self.staged_sha = sha
        self.staged_source = desired.get("source", "original")
        self.state = "ready"  # 资源已准备

    def draw_frame(self, final_before_release=False):
        """One frame; returns True if a new revision committed."""
        committed = False
        if final_before_release and self.actual:
            # revoke path: final frame drawn, now release
            self.frames += 1
            return False
        if self.staged_rev is not None and self.staged is not None:
            self.state = "switch_pending"  # 等待切换
            if self.vram_fail_at == self.staged_rev:
                self.state = "error"
                self.error = "vram_alloc_failed"
                self.staged = None  # old texture remains displayed
                return False
            old = self.actual
            # first frame succeeds: 正在显示; old texture released here
            self._write_atomic(self._blob_path(self.staged_sha),
                               self.staged)
            manifest = {
                "revision": self.staged_rev,
                "sha256": self.staged_sha,
                "size": len(self.staged),
                "source": self.staged_source,
            }
            self._write_atomic(self._manifest_path(),
                               json.dumps(manifest), binary=False)
            self.cache_committed = True
            self.actual = self.staged_rev
            self.staged = None
            self.staged_rev = None
            self.state = "showing"
            self.error = "none"
            committed = True
            if old:
                pass  # previous texture released; old blob may linger
        self.frames += 1
        return committed

    def run(self, iterations=None, quiet=False):
        restored = self.restore_cache()
        if restored and not quiet:
            print("[sim] restored " + restored["revision"] + " from cache")
        n = 0
        while iterations is None or n < iterations:
            try:
                self.poll_once()
                self.apply_poll()
                self.draw_frame()
            except (urllib.error.URLError, ConnectionError, OSError) as e:
                if not quiet:
                    print("[sim] offline (" + str(e) + "); showing "
                          + (self.actual or "-"))
            if not quiet:
                want = None
                if self.desired:
                    want = self.desired.get("revision")
                print("[sim] state=%s error=%s want=%s actual=%s frames=%d"
                      % (self.state, self.error, want or "-",
                         self.actual or "-", self.frames))
            n += 1
            time.sleep(self.poll_interval)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--server", default="http://127.0.0.1:8080")
    ap.add_argument("--cache", default="/tmp/sim-device-cache")
    ap.add_argument("--name", default="sim-device-01")
    ap.add_argument("--formats", default="png,jpeg")
    ap.add_argument("--iterations", type=int, default=None)
    ap.add_argument("--vram-fail-at", default=None)
    ap.add_argument("--die-during-cache-write", action="store_true")
    ap.add_argument("--chunk-delay", type=float, default=0.0)
    args = ap.parse_args()
    dev = SimDevice(
        args.server, args.cache, args.name,
        formats=args.formats.split(","),
        vram_fail_at=args.vram_fail_at,
        die_during_cache_write=args.die_during_cache_write,
        chunk_delay=args.chunk_delay,
    )
    dev.run(iterations=args.iterations)


if __name__ == "__main__":
    main()
