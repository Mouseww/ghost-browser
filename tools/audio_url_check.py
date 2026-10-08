#!/usr/bin/env python3
"""Acceptance for reading an audio challenge's clip over the DevTools pipe.

Google is refusing this network, so a live reCAPTCHA audio challenge cannot be
produced here at the moment, and the live half of this capability is graded by
`captcha_check.py` where a real challenge is already set up. What is checked here
is the mechanism, against a stand-in that reproduces the only structural facts
that matter: the clip is an `<audio src>` inside an iframe, on a different origin
from the page embedding it.

That is exactly reCAPTCHA's shape, and it is deliberately the hard half. The
stand-in frame is served from 127.0.0.2 inside a page on 127.0.0.1, which is a
different site, so Chromium gives it a renderer of its own -- the page's DOM tree
cannot contain it and the clip is only reachable by attaching to the frame's own
target. A same-origin iframe would have tested the easy path and passed while the
real one stayed broken.

The clip is referenced as `/clip.wav`, not as an absolute URL, because that is
what the DevTools tree hands back for a written attribute, and resolving it is
part of the job.

Exit 0 means every check passed. A check that could not be run at all is reported
as `[skip]` and counted separately; it is never silently a pass.
"""
from __future__ import annotations

import argparse
import hashlib
import http.server
import math
import os
import struct
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.stdout.reconfigure(encoding="utf-8", errors="replace")

from ghost_client import Ghost, GhostError  # noqa: E402

OUTER_HOST = "127.0.0.1"
INNER_HOST = "127.0.0.2"


def tone_wav(seconds: float = 0.5, rate: int = 16000, freq: float = 440.0) -> bytes:
    """A real, small WAV, so a byte comparison means something."""
    frames = int(seconds * rate)
    body = b"".join(struct.pack("<h", int(12000 * math.sin(2 * math.pi * freq * i / rate)))
                    for i in range(frames))
    return (b"RIFF" + struct.pack("<I", 36 + len(body)) + b"WAVE"
            + b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16)
            + b"data" + struct.pack("<I", len(body)) + body)


CLIP = tone_wav()
CLIP_SHA = hashlib.sha256(CLIP).hexdigest()


class Checks:
    def __init__(self) -> None:
        self.passed = 0
        self.failed = 0
        self.skipped = 0

    def eq(self, name: str, actual, expected, detail: str = "") -> None:
        if actual == expected:
            self.passed += 1
            print(f"[ ok ] {name}")
        else:
            self.failed += 1
            print(f"[FAIL] {name}")
            print(f"        expected: {expected!r}")
            print(f"        actual:   {actual!r}")
        if detail:
            print(f"        {detail}")

    def true(self, name: str, value, detail: str = "") -> None:
        self.eq(name, bool(value), True, detail)

    def gap(self, name: str, why: str) -> None:
        self.skipped += 1
        print(f"[skip] {name}")
        print(f"        {why}")


class Site:
    """A one-host HTTP server, so two of them can be two origins."""

    def __init__(self, host: str, routes: dict) -> None:
        self.host = host
        self.routes = routes
        owner = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                payload = owner.routes.get(self.path.split("?")[0])
                if payload is None:
                    self.send_response(404)
                    self.end_headers()
                    return
                body, ctype = payload
                self.send_response(200)
                self.send_header("Content-Type", ctype)
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *args):
                pass

        self.server = http.server.ThreadingHTTPServer((host, 0), Handler)
        self.port = self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def url(self, path: str) -> str:
        return f"http://{self.host}:{self.port}{path}"

    def stop(self) -> None:
        self.server.shutdown()
        self.server.server_close()


def kill_chrome() -> None:
    subprocess.run(["taskkill", "/F", "/IM", "chrome.exe"],
                   capture_output=True, check=False)
    time.sleep(2)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--timeout", type=float, default=90.0)
    args = ap.parse_args()

    # Two shapes of frame, because the reader has two paths and only one of them is
    # the common case. `/frame` is named after nothing, so only the fallback can
    # find it; `/recaptcha/bframe` is named after a vendor, so the vendor pass finds
    # it. A build that only handled one of the two would pass half of this.
    inner = Site(INNER_HOST, {
        "/frame": (b'<!doctype html><html><body><p>inner</p>'
                   b'<audio id="audio-source" src="/clip.wav" preload="none"></audio>'
                   b'</body></html>', "text/html; charset=utf-8"),
        "/recaptcha/bframe": (b'<!doctype html><html><body><p>inner</p>'
                              b'<audio id="audio-source" src="/clip.wav" preload="none">'
                              b'</audio></body></html>', "text/html; charset=utf-8"),
        "/clip.wav": (CLIP, "audio/wav"),
    })

    def page(frame_path: str) -> bytes:
        return (f'<!doctype html><html><body><h1>stand-in</h1>'
                f'<iframe src="{inner.url(frame_path)}" width="400" height="200">'
                f'</iframe></body></html>').encode()

    outer = Site(OUTER_HOST, {
        "/": (page("/frame"), "text/html; charset=utf-8"),
        "/named": (page("/recaptcha/bframe"), "text/html; charset=utf-8"),
        "/plain": (b'<!doctype html><html><body><p>nothing here plays anything</p>'
                   b'</body></html>', "text/html; charset=utf-8"),
    })

    checks = Checks()
    print(f"outer {outer.url('/')}")
    print(f"frame {inner.url('/frame')}")
    print(f"clip  {inner.url('/clip.wav')}  {len(CLIP)} bytes\n")

    kill_chrome()
    stamp = int(time.time())
    dest = Path(tempfile.gettempdir()) / f"ghost-clip-check-{os.getpid()}.wav"

    def open_session(label: str, path: str, extra=()) -> Ghost:
        g = Ghost(f"audiourl-{label}-{os.getpid()}-{stamp}")
        g.start(outer.url(path), extra_args=extra, wait=args.timeout)
        return g

    # The fallback path: an unnamed frame on another origin.
    g = open_session("plain", "/")
    try:
        # The stand-in carries no widget at all, which is the point: the clip is a
        # property of the document, not of a challenge the tree has recognised.
        detect = g.call("captcha", action="detect")
        checks.eq("the stand-in page has no challenge on it",
                  detect.get("state"), "absent")

        info = g.call("captcha", action="audio-url", path=str(dest), keep=True)
        checks.true("the clip's URL was read out of an unnamed frame",
                    info.get("found"), f"detail: {info.get('detail', '')}")
        checks.eq("the URL is absolute, not the reference as authored",
                  info.get("url"), inner.url("/clip.wav"))

        # Proof the hard path is the one that ran: if the frame were same-process
        # the page's own tree would have answered and the attach path never used.
        targets = g.call("cdp", session="browser", method="Target.getTargets")
        frames = [t for t in targets.get("result", {}).get("targetInfos", [])
                  if t.get("type") == "iframe" and "/frame" in t.get("url", "")]
        checks.true("the frame is out of process, so the attach path is what ran",
                    frames, f"{len(frames)} iframe target(s)")

        checks.true("the clip was fetched", info.get("fetched"),
                    f"detail: {info.get('detail', '')}")
        checks.eq("the fetched length is the clip's length",
                  info.get("bytes"), len(CLIP))
        got = hashlib.sha256(dest.read_bytes()).hexdigest() if dest.exists() else None
        checks.eq("the fetched bytes are the clip", got, CLIP_SHA)
    finally:
        g.stop()
        if dest.exists():
            dest.unlink()
        time.sleep(1)

    # The vendor path: a frame named after a challenge provider.
    g = open_session("named", "/named")
    try:
        named = g.call("captcha", action="audio-url")
        checks.true("the clip's URL was read out of a vendor-named frame",
                    named.get("found"), f"detail: {named.get('detail', '')}")
        checks.eq("the vendor path resolves the same clip",
                  named.get("url"), inner.url("/clip.wav"))
    finally:
        g.stop()
        time.sleep(1)

    # A page with nothing to play must say so rather than inventing a clip.
    g = open_session("quiet", "/plain")
    try:
        quiet = g.call("captcha", action="audio-url")
        checks.eq("a page with no audio element reports nothing found",
                  quiet.get("found"), False)
        checks.true("and says why",
                    "no audio element" in (quiet.get("detail") or ""),
                    f"detail: {quiet.get('detail', '')}")
    finally:
        g.stop()
        time.sleep(1)

    # --no-cdp must refuse rather than quietly fall back to something else.
    g = open_session("nocdp", "/", extra=("--no-cdp",))
    try:
        off = g.call("captcha", action="audio-url")
        checks.eq("a session without the DevTools channel finds nothing",
                  off.get("found"), False)
        checks.true("and names the missing channel",
                    "DevTools pipe" in (off.get("detail") or ""),
                    f"detail: {off.get('detail', '')}")
    finally:
        g.stop()
        time.sleep(1)

    inner.stop()
    outer.stop()

    print(f"\n{checks.passed} checks, {checks.failed} failed, "
          f"{checks.skipped} not measurable")
    return 0 if checks.failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
