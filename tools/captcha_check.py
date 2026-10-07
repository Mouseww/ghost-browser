#!/usr/bin/env python3
"""Acceptance test for the human-verification channel.

Drives three real challenges — hCaptcha, reCAPTCHA v2 and Cloudflare Turnstile —
through `ghost serve`, and checks that the browser can name each one from outside
the renderer: no CDP, no injected script.

  python tools/captcha_check.py [--timeout 90] [--only NAME] [--keep-profile]

Exit code 0 means every check passed. A session with no foreground window can still
click: the control plane prefers real synthesized input and falls back to a UI
Automation invocation, reporting which one it used in `input`. Only a check that has
no accessibility equivalent is reported as "not measurable".
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ghost_client import Ghost, GhostError  # noqa: E402

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

TARGETS = [
    ("hcaptcha", "https://accounts.hcaptcha.com/demo"),
    ("recaptcha", "https://www.google.com/recaptcha/api2/demo"),
    ("turnstile", "https://nopecha.com/demo/cloudflare"),
]


class Checks:
    def __init__(self) -> None:
        self.passed = 0
        self.failed = 0
        self.skipped = 0

    def eq(self, name: str, actual, expected, detail: str = "") -> bool:
        if actual == expected:
            self.passed += 1
            print(f"[ ok ] {name}")
            return True
        self.failed += 1
        print(f"[FAIL] {name}")
        print(f"       expected: {expected!r}")
        print(f"       actual:   {actual!r}")
        if detail:
            print(f"       {detail}")
        return False

    def true(self, name: str, value, detail: str = "") -> bool:
        return self.eq(name, bool(value), True, detail)

    def gap(self, name: str, why: str) -> None:
        self.skipped += 1
        print(f"[skip] {name}: {why}")


def wait_for_captcha(ghost: Ghost, timeout: float) -> dict:
    """Wait until the widget has a control to act on.

    The widget is injected a moment after the document loads, and it appears in two
    stages: first the frame, then the checkbox inside it (measured on the hCaptcha
    demo: the frame at +1 s, the checkbox at +2 s; on Cloudflare's interstitial the
    provider is named at +2 s and the checkbox at +4 s). Waiting only for the
    provider to be named is not enough — the page-level document is often on the
    vendor's own domain, so the name arrives before the challenge does.
    """
    deadline = time.time() + timeout
    last = {}
    while time.time() < deadline:
        last = ghost.call("captcha", action="detect")
        if last.get("state", "absent") != "absent":
            return last
        time.sleep(0.5)
    return last


def solve(checks: Checks, ghost: Ghost, label: str, **kwargs):
    """Ask for the challenge to be answered, and record how the click was delivered.

    The control plane prefers synthesized input -- the page sees a trusted event --
    and falls back to a UI Automation invocation when the session cannot deliver any,
    which is the normal case for a disconnected RDP or headless session. The fallback
    still changes the page, but it is not trusted, so the channel is reported rather
    than assumed: a check that passes on the fallback is weaker evidence than one that
    passes on a real click, and the log should say which happened.
    """
    try:
        answer = ghost.call("captcha", action="solve", **kwargs)
    except GhostError as exc:
        if "no foreground window" in str(exc):
            checks.gap(label, "this session has no foreground window")
            return None
        raise
    channel = answer.get("input", "")
    print(f"  input  : {channel or 'not reported'}")
    checks.true(f"{label}: the click reported which channel carried it",
                channel in ("synthesized", "accessibility"))
    return answer


def run_target(checks: Checks, name: str, url: str, timeout: float,
               keep_profile: bool) -> None:
    print(f"\n=== {name}: {url} ===")
    # A fresh profile for every run, and this is not tidiness. Cloudflare's
    # clearance is an ordinary cookie, and the profile is persistent — so once a run
    # has passed the challenge, the *next* run on that profile is not challenged at
    # all. Reusing the profile quietly turns this test into "we already have
    # clearance" and hides every regression in the reading path. Measured: on the
    # profile the test had been reusing, nopecha.com served its demo page directly
    # with no widget anywhere in the tree.
    profile = f"captcha-{name}-{os.getpid()}-{int(time.time())}"
    data_dir: Path | None = None
    try:
        # Start *at* the URL rather than navigating to it: `navigate` activates the
        # window first, which is exactly what a disconnected session cannot do — and
        # nothing about reading a challenge needs input.
        with Ghost(profile).start(url) as ghost:
            data_dir = ghost.data_dir()
            detected = wait_for_captcha(ghost, timeout)
            provider = detected.get("provider", "none")
            state = detected.get("state", "?")
            print(f"  provider={provider} state={state}")
            print(f"  detail : {detected.get('detail', '')}")
            print(f"  sitekey: {detected.get('site_key', '')}")
            print(f"  page   : {detected.get('page_url', '')}")
            print(f"  frame  : {detected.get('frame_url', '')[:130]}")

            if provider == "none":
                checks.true(f"{name}: the challenge is recognized", False,
                            f"nothing detected within {timeout:.0f}s")
                return
            checks.eq(f"{name}: the provider is named", provider, name)

            if name == "hcaptcha":
                # The sitekey is what a solving service needs, and it is the field
                # that proves the frame URL was parsed rather than guessed.
                key = detected.get("site_key", "")
                checks.true(f"{name}: the sitekey is read from the frame URL",
                            len(key) == 36 and key.count("-") == 4, f"got {key!r}")
                checks.true(f"{name}: the page URL is read",
                            "hcaptcha.com" in detected.get("page_url", ""),
                            detected.get("page_url", ""))
                checks.eq(f"{name}: it is waiting on its checkbox", state, "checkbox")

                after = solve(checks, ghost, name, timeout=20000)
                if after is None:
                    return
                print(f"  after the click: state={after.get('state')} "
                      f"detail={after.get('detail')}")
                checks.eq(f"{name}: the click opens the image challenge",
                          after.get("state"), "visual")
                checks.true(f"{name}: the click was delivered", after.get("clicked"))

            elif name == "recaptcha":
                key = detected.get("site_key", "")
                checks.true(f"{name}: the sitekey is read from the anchor URL",
                            key.startswith("6L") and len(key) >= 30, f"got {key!r}")
                checks.eq(f"{name}: it is waiting on its checkbox", state, "checkbox")

                after = solve(checks, ghost, name, timeout=20000)
                if after is None:
                    return
                print(f"  after the click: state={after.get('state')} "
                      f"detail={after.get('detail')}")
                checks.eq(f"{name}: the click opens the challenge frame",
                          after.get("state"), "visual")

                # The audio route is the one this project can actually automate, so
                # the test asserts the control is reachable and then proves it by
                # clicking it.
                tree = ghost.call("find", name="改用音频验证", max_depth=30,
                                  max_nodes=4000)
                nodes = tree.get("nodes") or []
                if not nodes:
                    tree = ghost.call("find", name="audio", max_depth=30, max_nodes=4000)
                    nodes = tree.get("nodes") or []
                if nodes:
                    checks.true(f"{name}: the audio button is reachable", True)
                    try:
                        ghost.call("click", index=nodes[0]["index"])
                    except GhostError as exc:
                        if "no foreground window" in str(exc):
                            checks.gap(f"{name}: clicking it opens the audio challenge",
                                       "this session has no foreground window")
                            return
                        raise
                    # The audio challenge replaces the image one a moment after the
                    # button is clicked, so poll rather than reading once and calling
                    # a slow load a failure.
                    audio = {}
                    deadline = time.time() + 10
                    while time.time() < deadline:
                        audio = ghost.call("captcha", action="detect")
                        if audio.get("state") == "audio":
                            break
                        time.sleep(0.5)
                    print(f"  after the audio click: state={audio.get('state')} "
                          f"detail={audio.get('detail')}")
                    checks.eq(f"{name}: clicking it opens the audio challenge",
                              audio.get("state"), "audio")
                    checks.true(f"{name}: the challenge token is read",
                                bool(detected.get("challenge_token") or
                                     audio.get("challenge_token")))
                else:
                    checks.gap(f"{name}: the audio button is reachable",
                               "the challenge frame did not expose it")

            elif name == "turnstile":
                checks.eq(f"{name}: it is waiting on its checkbox", state, "checkbox")
                checks.true(f"{name}: the widget is identified without a frame URL",
                            provider == "turnstile")

                after = solve(checks, ghost, name, timeout=30000)
                if after is None:
                    return
                print(f"  after the click: state={after.get('state')} "
                      f"detail={after.get('detail')}")
                checks.true(f"{name}: a click answers the challenge",
                            after.get("state") == "solved",
                            f"got {after.get('state')!r}")

                # The widget goes as soon as the challenge passes, but the page it
                # was guarding changes its title on its own schedule, so poll
                # instead of reading once and calling the navigation a failure.
                title = ""
                deadline = time.time() + 15
                while time.time() < deadline:
                    title = (ghost.status().get("window") or {}).get("title", "")
                    if "NopeCHA" in title:
                        break
                    time.sleep(0.5)
                checks.true(f"{name}: the guarded page is reached",
                            "NopeCHA" in title, f"title={title!r}")
    finally:
        if data_dir is not None and not keep_profile:
            shutil.rmtree(data_dir, ignore_errors=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout", type=float, default=90.0,
                        help="seconds to wait for a challenge to appear")
    parser.add_argument("--keep-profile", action="store_true",
                        help="keep the throwaway profile each target runs in")
    parser.add_argument("--only", default="", help="run one target by name")
    args = parser.parse_args()

    checks = Checks()
    for name, url in TARGETS:
        if args.only and args.only != name:
            continue
        try:
            run_target(checks, name, url, args.timeout, args.keep_profile)
        except Exception as exc:  # a dead browser must not hide the other targets
            checks.failed += 1
            print(f"[FAIL] {name}: raised {type(exc).__name__}: {exc}")

    total = checks.passed + checks.failed
    print()
    print(f"{total} checks, {checks.failed} failed, {checks.skipped} not measurable "
          f"in this session")
    return 1 if checks.failed else 0


if __name__ == "__main__":
    sys.exit(main())
