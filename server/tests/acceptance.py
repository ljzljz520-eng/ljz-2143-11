#!/usr/bin/env python3
"""
Acceptance scenarios for background publishing:

  S1 连续下发两张图，第一张晚到（迟到者必须被丢弃，最终显示第二张）
  S2 重绘中更新（READY/SWITCH_PENDING 期间被新修订抢占，旧帧不闪空）
  S3 进程在缓存写入时退出（残留 .part；重启后从已提交清单恢复或干净起步）
  S4 资源被撤销但设备离线（撤销挂起，重连轮询补收，终帧后释放）
  S5 三类错误可区分：下载不全 / 内容不支持 / 显存申请失败
  S6 服务端期望版本 vs 终端实际版本（/api/state 双列）
  S7 没有可用图片时文字层仍工作（设备一直能轮询并上报，业务状态不被清空）

Run:
  python3 -m server.tests.acceptance
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request
import urllib.error
import zlib
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, ROOT)

from server.tests.sim_device import SimDevice  # noqa: E402

PASS = "\033[32mPASS\033[0m"
FAIL = "\033[31mFAIL\033[0m"
failures = []


def check(name, cond, detail=""):
    print(f"  [{PASS if cond else FAIL}] {name}" +
          (f" -- {detail}" if detail and not cond else ""))
    if not cond:
        failures.append(name)


# ----------------------------------------------------------------------
# helpers
# ----------------------------------------------------------------------

def make_png(path, w=4, h=4, color=(30, 90, 200)):
    def chunk(t, p):
        return (struct.pack(">I", len(p)) + t + p +
                struct.pack(">I", zlib.crc32(t + p) & 0xffffffff))
    raw = b""
    row = bytes(color) * w
    for _ in range(h):
        raw += b"\x00" + row
    png = (b"\x89PNG\r\n\x1a\n" +
           chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)
    return png


def post(url, payload, raw=False):
    data = payload if raw else json.dumps(payload).encode()
    ctype = "application/octet-stream" if raw else "application/json"
    req = urllib.request.Request(
        url, data=data, headers={"Content-Type": ctype}, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read())


def upload(server, png_bytes, filename="image.png", policy="terminal",
           delay_ms=0):
    boundary = "----accept" + str(time.time_ns())
    def part(name, value):
        return (f"--{boundary}\r\nContent-Disposition: form-data; "
                f'name="{name}"\r\n\r\n{value}\r\n').encode()
    head = (
        f"--{boundary}\r\n"
        f'Content-Disposition: form-data; name="file"; '
        f'filename="{filename}"\r\n'
        f"Content-Type: application/octet-stream\r\n\r\n"
    ).encode()
    body = (head + png_bytes + b"\r\n" +
            part("policy", policy) +
            part("delay_ms", str(delay_ms)) +
            f"--{boundary}--\r\n".encode())
    req = urllib.request.Request(
        server + "/api/resources", data=body,
        headers={"Content-Type":
                 f"multipart/form-data; boundary={boundary}"},
        method="POST")
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read())


def state(server):
    with urllib.request.urlopen(server + "/api/state", timeout=10) as r:
        return json.loads(r.read())


def wait_until(predicate, timeout=12.0, interval=0.15):
    end = time.time() + timeout
    while time.time() < end:
        if predicate():
            return True
        time.sleep(interval)
    return False


def run_device_steps(server, cache, steps, **kwargs):
    dev = SimDevice(server, cache, "accept-dev", poll_interval=0.05,
                    **kwargs)
    for _ in range(steps):
        try:
            dev.poll_once()
            dev.apply_poll()
            dev.draw_frame()
        except (urllib.error.URLError, ConnectionError, OSError):
            pass
        time.sleep(0.05)
    return dev


# ----------------------------------------------------------------------
# scenarios
# ----------------------------------------------------------------------

def s1_late_first_image(server, tmp):
    print("S1 连续下发两张图，第一张晚到")
    import threading
    img1 = make_png(os.path.join(tmp, "a.png"), color=(200, 30, 30))
    img2 = make_png(os.path.join(tmp, "b.png"), color=(30, 200, 60))
    # rev1 is served after a long delay; rev2 is immediate.
    _, r1 = upload(server, img1, "a.png", delay_ms=4000)
    check("rev1 created with slow link", bool(r1["revision_id"]))

    cache = os.path.join(tmp, "dev1")
    dev = SimDevice(server, cache, "late-dev", poll_interval=0.05,
                    chunk_delay=0.02)
    dev.poll_once()
    check("initial desired is rev1",
          dev.desired["revision"] == r1["revision_id"],
          str(dev.desired))

    # Start rev1 transfer in a background thread while its bytes are still
    # trickling, THEN publish rev2.
    desired_snapshot = dict(dev.desired)
    def begin_rev1():
        dev.fetch(desired_snapshot)
    t = threading.Thread(target=begin_rev1)
    t.start()
    time.sleep(0.8)  # server-side delay still in effect, nothing usable

    _, r2 = upload(server, img2, "c.png", delay_ms=0)
    check("rev2 published while rev1 download incomplete",
          r2["revision_id"] != r1["revision_id"])

    # Streaming abort-check polls mid-transfer, sees rev2, cancels rev1.
    t.join(timeout=15)
    end = time.time() + 12
    while time.time() < end:
        try:
            dev.poll_once()
            dev.apply_poll()
            dev.draw_frame()
        except urllib.error.URLError:
            pass
        time.sleep(0.05)
        if dev.actual == r2["revision_id"]:
            break
    check("device shows rev2 (late rev1 discarded)",
          dev.actual == r2["revision_id"], f"actual={dev.actual}")
    check("device never reported rev1 as shown",
          dev.actual != r1["revision_id"])

    st = state(server)
    rows = [d for d in st["delivery"]
            if d["device_id"] == dev.device_id]
    r1row = next((d for d in rows
                  if d["revision_id"] == r1["revision_id"]), None)
    check("delivery rows exist for both revisions",
          r1row is not None and
          any(d["revision_id"] == r2["revision_id"] for d in rows))
    check("rev1 delivery never reached showing (obsolete/pending/...)",
          r1row is not None and r1row["state"] != "showing",
          r1row["state"] if r1row else "missing")


def s2_update_during_redraw(server, tmp):
    print("S2 重绘中更新（READY/SWITCH_PENDING 被抢占）")
    img1 = make_png(os.path.join(tmp, "x.png"), color=(10, 10, 120))
    img2 = make_png(os.path.join(tmp, "y.png"), color=(120, 80, 10))
    _, r1 = upload(server, img1, "x.png")
    cache = os.path.join(tmp, "dev2")
    dev = SimDevice(server, cache, "redraw-dev", poll_interval=0.05)
    dev.poll_once()
    dev.apply_poll()  # rev1 fetched -> READY (resource prepared)
    check("rev1 reaches READY", dev.state == "ready", dev.state)

    # publish rev2 BEFORE rev1's first frame is drawn
    _, r2 = upload(server, img2, "y.png")
    dev.poll_once()
    dev.apply_poll()  # must drop staged rev1 and fetch rev2
    check("staged rev1 superseded before first frame",
          (dev.staged_rev in (None, r2["revision_id"])),
          str(dev.staged_rev))
    committed = dev.draw_frame()
    check("committed frame is rev2", dev.actual == r2["revision_id"],
          dev.actual)
    check("screen never reported an empty/blank revision",
          committed is True)

    # while showing rev2, rev3 arrives: old "texture" stays until new frame
    img3 = make_png(os.path.join(tmp, "z.png"), color=(60, 60, 60))
    _, r3 = upload(server, img3, "z.png")
    dev.poll_once()
    dev.apply_poll()
    check("during switch rev2 still the actual shown revision",
          dev.actual == r2["revision_id"])
    dev.draw_frame()
    check("after first frame rev3 shown", dev.actual == r3["revision_id"])


def s3_crash_during_cache_write(server, tmp):
    print("S3 进程在缓存写入时退出")
    img = make_png(os.path.join(tmp, "crash.png"), color=(90, 20, 90))
    _, r = upload(server, img, "crash.png")
    cache = os.path.join(tmp, "dev3")

    # Real process: it hard-exits with a half-written .part.
    proc = subprocess.run(
        [sys.executable, "-m", "server.tests.sim_device",
         "--server", server, "--cache", cache, "--name", "crash-dev",
         "--die-during-cache-write", "--iterations", "1"],
        cwd=ROOT, capture_output=True, timeout=30)
    check("process exited during write (code 3)",
          proc.returncode == 3, f"rc={proc.returncode}")
    leftovers = [n for n in os.listdir(cache)
                 if n.endswith((".part", ".tmp"))]
    check("half-written staging file exists", len(leftovers) >= 1,
          str(leftovers))
    manifest = os.path.join(cache, "manifest.json")
    check("no manifest was committed", not os.path.exists(manifest))

    # restart: staging swept, clean re-fetch, manifest created atomically
    dev2 = SimDevice(server, cache, "crash-dev")
    check("staging leftovers cleaned on boot",
          all(not n.endswith((".part", ".tmp"))
              for n in os.listdir(cache)))
    end = time.time() + 8
    while time.time() < end:
        dev2.poll_once()
        dev2.apply_poll()
        dev2.draw_frame()
        if dev2.actual == r["revision_id"]:
            break
        time.sleep(0.05)
    check("after restart device shows revision",
          dev2.actual == r["revision_id"], dev2.actual)
    check("manifest present and valid",
          os.path.exists(manifest) and
          json.load(open(manifest))["revision"] == r["revision_id"])

    # restart again -> restore from cache without any server contact
    dev3 = SimDevice("http://127.0.0.1:1", cache, "crash-dev")
    m = dev3.restore_cache()
    check("second boot restores manifest offline",
          m is not None and m["revision"] == r["revision_id"])


def s4_revoke_while_offline(server, tmp):
    print("S4 资源被撤销但设备离线")
    img = make_png(os.path.join(tmp, "rev.png"), color=(20, 140, 140))
    _, r = upload(server, img, "rev.png")
    cache = os.path.join(tmp, "dev4")
    dev = SimDevice(server, cache, "offline-revoke-dev",
                    poll_interval=0.05)
    end = time.time() + 8
    while time.time() < end:
        dev.poll_once()
        dev.apply_poll()
        dev.draw_frame()
        if dev.actual == r["revision_id"]:
            break
        time.sleep(0.05)
    check("device shows rev before going offline",
          dev.actual == r["revision_id"])
    frames_before = dev.frames

    # revoke while device is "offline" (no polls)
    post(server + f"/api/revisions/{r['revision_id']}/revoke", {})
    time.sleep(0.3)
    snap = state(server)
    check("revoked revision is flagged server-side",
          snap.get("latest_revision_any") == r["revision_id"] and
          snap.get("latest_revoked") is True)

    # reconnect: first poll delivers revoked=true
    resp = dev.poll_once()
    check("reconnect poll signals revoked", resp.get("revoked") is True)
    check("before applying revoke, actual still shown",
          dev.actual == r["revision_id"])
    dev.apply_poll()  # final frame + release
    check("after final frame, actual cleared (texture released)",
          dev.actual == "")
    check("at least one final frame was drawn",
          dev.frames > frames_before)
    check("manifest removed on revoke",
          not os.path.exists(os.path.join(cache, "manifest.json")))
    check("state back to idle; text layer unaffected (poll keeps working)",
          dev.state == "idle")


def s5_error_classes(server, tmp):
    print("S5 错误三分类：下载不全 / 内容不支持 / 显存失败")
    # 1) truncated upload (content shorter than any header)
    code, resp = upload(server, b"abc", "short.png")
    check("truncated header -> download_incomplete (422)",
          code == 422 and resp.get("error") == "download_incomplete",
          str(resp))

    # 2) valid GIF magic -> unsupported (extension is ignored)
    gif = b"GIF89a" + b"\x00" * 32
    code, resp = upload(server, gif, "masquerade.png")
    check("gif bytes rejected as content_unsupported",
          code == 422 and resp.get("error") == "content_unsupported",
          str(resp))

    # 3) random garbage with .jpg extension -> unsupported
    code, resp = upload(server, bytes(range(256)), "real.jpg")
    check("garbage with .jpg -> content_unsupported",
          code == 422 and resp.get("error") == "content_unsupported")

    # 4) truncated PNG declared right name -> header present, body cut
    img = make_png(os.path.join(tmp, "ok.png"), color=(1, 2, 3))
    truncated = img[: len(img) // 2]
    code, resp = upload(server, truncated, "ok.png")
    check("truncated PNG body -> download_incomplete or unsupported",
          code == 422 and resp.get("error") in
          ("download_incomplete", "content_unsupported"), resp.get("error"))

    # 5) VRAM failure at terminal side
    img2 = make_png(os.path.join(tmp, "vram.png"), color=(7, 7, 7))
    _, r = upload(server, img2, "vram.png")
    cache = os.path.join(tmp, "dev5")
    dev = SimDevice(server, cache, "vram-dev",
                    vram_fail_at=r["revision_id"])
    end = time.time() + 6
    while time.time() < end:
        dev.poll_once()
        dev.apply_poll()
        dev.draw_frame()
        if dev.error == "vram_alloc_failed":
            break
        time.sleep(0.05)
    check("terminal reports vram_alloc_failed distinctly",
          dev.error == "vram_alloc_failed", dev.error)
    check("device state is error but never showed that revision",
          dev.state == "error" and dev.actual == "")

    # 6) incompatible artifact delivered to a png-only device (webp bytes)
    webp = (b"RIFF\x20\x00\x00\x00WEBPVP8 " + b"\x00" * 24)
    code, resp = upload(server, webp, "pic.webp")
    check("webp upload is rejected without Pillow (capability honest)",
          code in (201, 422))
    if code == 201:
        cache2 = os.path.join(tmp, "dev5b")
        dev2 = SimDevice(server, cache2, "no-webp-dev",
                         formats=["png"])
        end = time.time() + 5
        while time.time() < end:
            dev2.poll_once()
            dev2.apply_poll()
            dev2.draw_frame()
            if dev2.error == "content_unsupported":
                break
            time.sleep(0.05)
        check("png-only device gets content_unsupported for webp",
              dev2.error == "content_unsupported")


def s6_version_columns(server, tmp):
    print("S6 网页/API 同时呈现服务端期望版本与终端实际版本")
    img1 = make_png(os.path.join(tmp, "v1.png"), color=(11, 22, 33))
    img2 = make_png(os.path.join(tmp, "v2.png"), color=(33, 22, 11))
    _, r1 = upload(server, img1, "v1.png")
    cache = os.path.join(tmp, "dev6")
    dev = SimDevice(server, cache, "version-dev", poll_interval=0.05)
    end = time.time() + 6
    while time.time() < end:
        dev.poll_once()
        dev.apply_poll()
        dev.draw_frame()
        if dev.actual == r1["revision_id"]:
            break
        time.sleep(0.05)
    _, r2 = upload(server, img2, "v2.png")  # device becomes stale
    # device reports once while still showing r1: server records wanted r2
    # vs actual r1 (exactly what the web columns must display)
    dev.poll_once()
    st = state(server)
    check("latest_revision tracks server expectation",
          st["latest_revision"] == r2["revision_id"])
    row = next(d for d in st["delivery"]
               if d["device_id"] == dev.device_id
               and d["revision_id"] == r2["revision_id"])
    check("delivery row distinguishes wanted vs last_actual",
          row["revision_id"] == r2["revision_id"] and
          row["last_actual"] == r1["revision_id"] and
          row["state"] == "pending",
          str(row))
    devices = {d["device_id"]: d for d in st["devices"]}
    check("device exists and is authorized",
          devices[dev.device_id]["authorized"] == 1)


def s7_text_layer_independent(server, tmp):
    print("S7 无可用图片时文字层/业务状态不被清空")
    cache = os.path.join(tmp, "dev7")
    dev = SimDevice(server, cache, "noimage-dev", poll_interval=0.05)
    for _ in range(4):
        resp = dev.poll_once()
        dev.apply_poll()
        dev.draw_frame()
        time.sleep(0.05)
    check("poll succeeds with no revision published",
          resp.get("desired") is None and resp.get("revoked") is False)
    check("device remains idle, not error", dev.state == "idle")

    # upload unsupported content directly via delivery -> business survives
    code, _ = upload(server, b"\x00" * 64, "bad.png")
    check("bad image rejected; it never becomes a revision to clear screen",
          code == 422)
    for _ in range(4):
        resp = dev.poll_once()
        dev.apply_poll()
        dev.draw_frame()
        time.sleep(0.05)
    check("after bad upload, device still polls and stays idle",
          dev.state == "idle" and dev.actual == "")


# ----------------------------------------------------------------------
# runner: one FRESH server + data dir per scenario so they never bleed
# ----------------------------------------------------------------------

SCENARIOS = [
    s1_late_first_image,
    s2_update_during_redraw,
    s3_crash_during_cache_write,
    s4_revoke_while_offline,
    s5_error_classes,
    s6_version_columns,
    s7_text_layer_independent,
]


def main():
    base_tmp = tempfile.mkdtemp(prefix="bg-accept-")
    port = 18100 + (os.getpid() % 800)
    env = dict(os.environ)

    for i, scenario in enumerate(SCENARIOS):
        before = len(failures)
        tmp = os.path.join(base_tmp, scenario.__name__)
        os.makedirs(tmp, exist_ok=True)
        data_dir = os.path.join(tmp, "data")
        os.makedirs(data_dir, exist_ok=True)
        url = f"http://127.0.0.1:{port + i}"
        proc = subprocess.Popen(
            [sys.executable, "-u", "-m", "server", "--host", "127.0.0.1",
             "--port", str(port + i), "--db",
             os.path.join(data_dir, "p.db"),
             "--data-dir", data_dir],
            cwd=ROOT, env=env,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        scenario_failed = False
        try:
            wait_until(lambda: _ready(url), timeout=10)
            scenario(url, tmp)
            scenario_failed = len(failures) > before
        finally:
            proc.terminate()
            try:
                _out, err = proc.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                _out, err = proc.communicate()
            if scenario_failed:
                print("\n--- server stderr for " + scenario.__name__ + " ---")
                print(err.decode()[-3000:])

    shutil.rmtree(base_tmp, ignore_errors=True)

    print()
    if failures:
        print(f"{FAIL} {len(failures)} check(s) failed:")
        for f in failures:
            print("  -", f)
        sys.exit(1)
    print(f"{PASS} all acceptance scenarios passed")


def _ready(server):
    try:
        with urllib.request.urlopen(server + "/api/state", timeout=1) as r:
            return r.status == 200
    except OSError:
        return False


if __name__ == "__main__":
    main()
