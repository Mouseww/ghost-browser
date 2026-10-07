"""Acceptance for the third tier: the solving-service path.

There is no key here and there is not going to be one, so the service is stood in for
by a local HTTP server that speaks the same two endpoints. That checks the part this
repository can actually get wrong -- the request shape, the upload, the polling, and
getting the answer into the page -- and leaves the vendor's own accuracy to the vendor.

Run: python tools/solve_api_check.py [--timeout 90]
"""

import argparse
import base64
import os
import pathlib
import shutil
import struct
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from ghost_client import Ghost, GhostError  # noqa: E402

TARGET = "https://www.google.com/recaptcha/api2/demo"
API_KEY = "test-key-not-a-real-one"
ANSWER = "385124"

seen: dict = {}


class StandIn(BaseHTTPRequestHandler):
    """The two calls a solving service is asked for, and nothing else."""

    def log_message(self, *_args):
        pass

    def _reply(self, text):
        body = text.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        length = int(self.headers.get("Content-Length") or 0)
        form = parse_qs(self.rfile.read(length).decode("utf-8", "replace"))
        seen["submit"] = {k: v[0] for k, v in form.items()}
        seen["content_type"] = self.headers.get("Content-Type")
        self._reply("OK|12345")

    def do_GET(self):
        seen["polls"] = seen.get("polls", 0) + 1
        # The first poll is deliberately not-ready, because a service that answers
        # instantly is not the service this code has to survive.
        if seen["polls"] < 2:
            self._reply("CAPCHA_NOT_READY")
        else:
            self._reply(f"OK|{ANSWER}")


def wav_facts(payload: bytes):
    """(rate, channels, bits) from a RIFF header, or None when it is not a WAV."""
    if len(payload) < 44 or payload[0:4] != b"RIFF" or payload[8:12] != b"WAVE":
        return None
    channels, rate = struct.unpack_from("<HI", payload, 22)
    bits = struct.unpack_from("<H", payload, 34)[0]
    return rate, channels, bits


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout", type=float, default=90.0)
    args = parser.parse_args()

    server = ThreadingHTTPServer(("127.0.0.1", 0), StandIn)
    port = server.server_address[1]
    threading.Thread(target=server.serve_forever, daemon=True).start()
    print(f"stand-in    http://127.0.0.1:{port}")

    os.environ["GHOST_CAPTCHA_URL"] = f"http://127.0.0.1:{port}"
    os.environ["GHOST_CAPTCHA_KEY"] = API_KEY

    profile = f"solve-api-check-{os.getpid()}-{int(time.time())}"
    ghost = Ghost(profile)
    checks = 0
    failures: list[str] = []
    gaps: list[str] = []

    def check(name, ok, detail=""):
        nonlocal checks
        checks += 1
        if not ok:
            failures.append(name)
        print(f"[{'ok' if ok else 'FAIL'}] {name}" + (f": {detail}" if detail else ""))

    def gap(name, detail=""):
        nonlocal checks
        checks += 1
        gaps.append(name)
        print(f"[skip] {name}" + (f": {detail}" if detail else ""))

    try:
        ghost.start(TARGET, extra_args=("--chrome-arg=--autoplay-policy=no-user-gesture-required",))
        data_dir = ghost.data_dir()
        # Opening the audio challenge is the flaky part of this, and it gets worse with
        # repetition: reCAPTCHA stops offering the audio route after several attempts
        # from the same address. A fresh browser, not just a fresh request -- a
        # half-clicked widget keeps its state, and clicking a checkbox that is already
        # answered is itself how a retry fails. Reloading in place needs synthesized
        # input, which a session without a foreground window cannot deliver, so restart.
        answer: dict = {}
        attempts = 4
        for attempt in range(attempts):
            if attempt:
                print(f"retry       restarting for attempt {attempt + 1}")
                try:
                    ghost.stop()
                except Exception:  # noqa: BLE001 - shutdown best effort
                    pass
                shutil.rmtree(data_dir, ignore_errors=True)
                # Backing off is the point: the refusal is rate-shaped, so hammering it
                # is what causes it.
                time.sleep(10)
                ghost.start(TARGET, extra_args=(
                    "--chrome-arg=--autoplay-policy=no-user-gesture-required",))
            answer = ghost.call("captcha", action="solve-audio", seconds=5, language="en",
                                timeout=int(args.timeout * 1000))
            if seen.get("submit"):
                break
            print(f"            no audio challenge (state={answer.get('state')!r})")
        print(f"answer      {answer}")

        if not seen.get("submit"):
            # Nothing was uploaded, so there is nothing to assert about the protocol.
            # Grading this as a failure would blame the tier for the challenge refusing;
            # grading it as a pass would claim a run that never happened. It is a gap,
            # and it is reported as one.
            gap("the tier ran against a live audio challenge",
                f"reCAPTCHA never opened its audio route in {attempts} attempts "
                f"(last state {answer.get('state')!r}); the protocol was not exercised")
        else:
            submit = seen.get("submit") or {}
            check("the service was asked to solve audio", submit.get("method") == "audio",
                  f"method={submit.get('method')!r}")
            check("the request carried the configured key", submit.get("key") == API_KEY)
            check("the request carried the language hint", submit.get("language") == "en",
                  f"language={submit.get('language')!r}")
            check("the request was a form post",
                  "application/x-www-form-urlencoded" in (seen.get("content_type") or ""),
                  f"content-type={seen.get('content_type')!r}")

            payload = None
            if submit.get("body"):
                try:
                    payload = base64.b64decode(submit["body"], validate=True)
                except Exception as exc:  # noqa: BLE001 - a decode failure is a failure
                    check("the upload was valid base64", False, str(exc))
            if payload is not None:
                facts = wav_facts(payload)
                check("the upload was a WAV", facts is not None)
                if facts:
                    rate, channels, bits = facts
                    check("the upload was 16 kHz mono 16-bit",
                          (rate, channels, bits) == (16000, 1, 16),
                          f"{rate} Hz {channels}ch {bits}-bit")
                    check("the upload was small enough to send", len(payload) < 1_000_000,
                          f"{len(payload)} bytes")

            check("the not-ready answer was polled again", seen.get("polls", 0) >= 2,
                  f"polls={seen.get('polls')}")
            check("the answer is attributed to the service",
                  answer.get("solved_by") == "api",
                  f"solved_by={answer.get('solved_by')!r}")
            check("the service's digits were accepted", answer.get("heard") == ANSWER,
                  f"heard={answer.get('heard')!r}")
            check("the digits reached the answer field", answer.get("typed") is True,
                  f"typed={answer.get('typed')!r}")
            check("the answer was delivered through a real channel",
                  answer.get("input") in ("synthesized", "accessibility"),
                  f"input={answer.get('input')!r}")
    except GhostError as exc:
        check("the tier ran", False, str(exc))
    finally:
        try:
            ghost.stop()
        except Exception:  # noqa: BLE001 - shutdown best effort
            pass
        try:
            shutil.rmtree(data_dir, ignore_errors=True)
        except Exception:  # noqa: BLE001 - best effort
            pass
        server.shutdown()

    print(f"\n{checks} checks, {len(failures)} failed, {len(gaps)} not measurable")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
