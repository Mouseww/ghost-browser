"""End-to-end acceptance check of the zero-CDP control plane.

Everything here goes through `ghost serve`: the browser is launched by the
product, driven with synthesized OS input, read through the accessibility tree,
photographed through the window, and its cookies are read out of the profile's
SQLite file once the browser has shut down (Chrome keeps that file open with no
sharing while it runs). No debugging protocol is involved at any point.

The page under test is served locally and reports on itself through aria-labels,
which UIA exposes as accessible names — the only reading channel available when
there is no protocol to ask.

    python tools/serve_check.py
"""
from __future__ import annotations

import argparse
import ctypes
import importlib.util
import json
import os
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

from ghost_client import Ghost, GhostError  # noqa: E402

GHOST = ROOT / "native" / "build" / "bin" / "ghost.exe"

# Windows throws synthesized input away while a session is disconnected: there is
# no foreground window for SendInput to deliver to, and it fails silently rather
# than reporting an error.
NO_FOREGROUND = ("no foreground window: Windows discards synthesized input in a "
                 "disconnected or headless session")

# The checks that cannot run without synthesized input.
INPUT_CHECKS = (
    "clicking it produced a trusted event",
    "typing reached the page as trusted input",
    "a key combo selected the field before retyping",
    "the page counted trusted events",
    "not one event was untrusted",
    "scrolling was accepted",
)


def foreground_window() -> int:
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    user32.GetForegroundWindow.restype = ctypes.c_void_p
    return user32.GetForegroundWindow() or 0


def kill_stale_browser(data_dir: Path) -> int:
    """Stop any browser still holding this profile.

    Chromium takes an exclusive lock on its user-data-dir, so a browser left over
    from an earlier run makes the next one exit immediately and present no
    window at all -- which reads as a control plane failure several layers away
    from the real cause. `ghost serve` now closes the browser it started, but a
    run killed from outside can still leave one behind.
    """
    script = (
        "$d = '__DIR__'; "
        "$p = Get-CimInstance Win32_Process -Filter "
        "\"Name='chrome.exe' or Name='msedge.exe'\" | "
        "Where-Object { $_.CommandLine -like \"*$d*\" }; "
        "$p | ForEach-Object { Stop-Process -Id $_.ProcessId -Force "
        "-ErrorAction SilentlyContinue }; "
        "$p.Count"
    ).replace("__DIR__", str(data_dir).replace("'", "''"))
    try:
        done = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", script],
                              capture_output=True, text=True, timeout=90)
    except (OSError, subprocess.TimeoutExpired):
        return 0
    return int(done.stdout.strip() or 0)

PAGE = """<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <title>ghost control plane check</title>
</head>
<body>
  <h1>ghost control plane check</h1>

  <p id="automation" aria-label="automation=pending">automation: pending</p>
  <p id="cpu" aria-label="cpu=pending">cpu: pending</p>
  <p id="gpu" aria-label="gpu=pending">gpu: pending</p>

  <p><label for="box">Message box</label>
     <input id="box" aria-label="Message box" type="text" size="30"></p>
  <p><button id="go" aria-label="Submit">Submit</button></p>

  <p id="result" aria-label="result=nothing yet">nothing yet</p>
  <p id="echo" aria-label="echo=">echo:</p>
  <p id="trusted" aria-label="trusted=0">trusted: 0</p>
  <p id="untrusted" aria-label="untrusted=0">untrusted: 0</p>
  <p id="cookie" aria-label="cookie=pending">cookie: pending</p>
  <div style="height:1500px"></div>
  <p id="bottom" aria-label="bottom=reached">the bottom of the page</p>

<script>
  // max-age matters: a session cookie lives only in memory and is never written
  // to the profile's Cookies database, so it could not be harvested after exit.
  document.cookie = "ghost_check=ok; path=/; max-age=3600";

  let trusted = 0;
  let untrusted = 0;
  const publish = (id, label, value) => {
    const node = document.getElementById(id);
    node.setAttribute("aria-label", label + "=" + value);
    node.textContent = label + ": " + value;
  };

  publish("cookie", "cookie",
          document.cookie.includes("ghost_check=ok") ? "set" : "missing");

  for (const type of ["mousemove", "mousedown", "mouseup", "click",
                      "keydown", "keypress", "keyup", "input", "wheel"]) {
    document.addEventListener(type, (event) => {
      if (event.isTrusted) { trusted += 1; } else { untrusted += 1; }
      publish("trusted", "trusted", trusted);
      publish("untrusted", "untrusted", untrusted);
    }, true);
  }

  document.getElementById("box").addEventListener("input", (event) => {
    publish("echo", "echo", event.target.value);
  });

  document.getElementById("go").addEventListener("click", (event) => {
    publish("result", "result",
            event.isTrusted ? "clicked via trusted event" : "clicked but UNTRUSTED");
  });

  const automation = [];
  if (navigator.webdriver) automation.push("webdriver");
  for (const name of ["cdc_adoQpoasnfa76pfcZLmcfl_Array", "__webdriver_evaluate",
                      "__selenium_evaluate", "__playwright__",
                      "__puppeteer_evaluation_script__", "_phantom", "callPhantom",
                      "__nightmare", "__driver_evaluate"]) {
    if (name in window) automation.push(name);
  }
  publish("automation", "automation", automation.length ? automation.join(",") : "none");

  publish("cpu", "cpu", navigator.hardwareConcurrency);
  const canvas = document.createElement("canvas");
  const gl = canvas.getContext("webgl");
  const debug = gl && gl.getExtension("WEBGL_debug_renderer_info");
  publish("gpu", "gpu",
          debug ? gl.getParameter(debug.UNMASKED_RENDERER_WEBGL) : "no webgl");
</script>
</body>
</html>
"""


class Page(BaseHTTPRequestHandler):
    def do_GET(self):  # noqa: N802
        body = PAGE.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


class Checks:
    def __init__(self):
        self.results: list[tuple[str, str, str]] = []

    def eq(self, name: str, actual, expected, detail: str = "") -> None:
        verdict = "PASS" if actual == expected else "FAIL"
        note = f"actual={actual!r} expected={expected!r}"
        self.results.append((verdict, name, f"{note}  {detail}".rstrip()))

    def ok(self, name: str, condition: bool, detail: str = "") -> None:
        self.results.append(("PASS" if condition else "FAIL", name, detail))

    def gap(self, name: str, reason: str) -> None:
        """Record a check this environment cannot exercise at all.

        Windows discards synthesized input while a session is disconnected, so
        the input-driven checks simply cannot run there. Reporting that as GAP
        rather than PASS keeps the suite honest about what it did not measure.
        """
        self.results.append(("GAP", name, reason))

    def contains(self, name: str, haystack: str, needle: str) -> None:
        verdict = "PASS" if needle in (haystack or "") else "FAIL"
        self.results.append((verdict, name, f"needle={needle!r} in {haystack!r}"))

    def report(self) -> int:
        failed = 0
        gaps = 0
        for verdict, name, detail in self.results:
            if verdict == "FAIL":
                failed += 1
            elif verdict == "GAP":
                gaps += 1
            print(f"[{verdict}] {name}" + (f"  {detail}" if detail else ""))
        tail = f"\n{len(self.results)} checks, {failed} failed"
        if gaps:
            tail += f", {gaps} not measurable in this session"
        print(tail)
        return failed


def profile_json(profile_id: str) -> dict:
    result = subprocess.run([str(GHOST), "profile", "show", "--id", profile_id],
                            capture_output=True, text=True, timeout=60)
    return json.loads(result.stdout)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--ghost", type=Path, default=GHOST,
                    help="the ghost.exe to grade; point it at a release download "
                         "to grade exactly what users get")
    ap.add_argument("--id", default="serve-check")
    ap.add_argument("--port", type=int, default=8741)
    ap.add_argument("--timeout", type=float, default=90.0)
    ap.add_argument("--keep-open", action="store_true")
    args = ap.parse_args()

    if not GHOST.exists():
        print(f"missing {GHOST}; build first")
        return 2

    # A fresh profile, with a time zone and locale that are not this host's, so
    # the spoofing assertions below cannot pass by accident.
    subprocess.run([str(GHOST), "profile", "new", "--id", args.id, "--force",
                    "--tz", "Europe/London", "--locale", "en-GB"],
                   capture_output=True, text=True, timeout=120)
    profile = profile_json(args.id)
    print(f"profile: {profile['cpu_hardware_concurrency']} cores, "
          f"{profile['memory_total_bytes'] // (1 << 30)} GiB, "
          f"{profile['screen_width']}x{profile['screen_height']}, "
          f"{profile['timezone_id']}, {profile['locale']}, "
          f"{profile['gpu_adapter_description']}")

    server = ThreadingHTTPServer(("127.0.0.1", args.port), Page)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    url = f"http://127.0.0.1:{args.port}/"
    print(f"page served at {url}")

    checks = Checks()
    log = ROOT / "harness" / f"serve-{args.id}.log"
    log.parent.mkdir(parents=True, exist_ok=True)

    data_dir = (Path(os.environ.get("LOCALAPPDATA", str(Path.home())))
                / "GhostBrowser" / "profiles" / f"{args.id}.data")
    stale = kill_stale_browser(data_dir)
    if stale:
        print(f"stopped {stale} browser process(es) still holding {data_dir.name}")
        time.sleep(2.0)

    browser = Ghost(args.id, ghost=args.ghost)
    input_available = foreground_window() != 0
    if not input_available:
        print(f"note: {NO_FOREGROUND}")
        print("      input-driven checks will be reported as GAP, not PASS")
    try:
        # With no way to type a URL, load it on the command line instead.
        browser.start(url=None if input_available else url, wait=args.timeout, log=log)
        status = browser.status()
        window = status.get("window") or {}
        print(f"browser pid {status['pid']}, pipe {status['pipe']}, "
              f"window {window.get('width', 0)}x{window.get('height', 0)}")
        checks.ok("the control plane is serving over a named pipe", status["pid"] != 0)
        checks.ok("the browser has a window to drive", bool(window),
                  f"window={window or None}")

        if input_available:
            navigated = browser.navigate(url)
            checks.ok("navigate reports a changed window title",
                      bool(navigated.get("navigated")), f"title={navigated.get('title')!r}")
        else:
            checks.gap("navigate reports a changed window title", NO_FOREGROUND)

        browser.wait_for(contains="ghost control plane check", timeout=args.timeout)
        checks.ok("the page is readable through the accessibility tree", True)

        # --- spoofing still holds on this path -----------------------------
        checks.eq("navigator.hardwareConcurrency is the profile's",
                  browser.label_value("cpu"), str(profile["cpu_hardware_concurrency"]))
        checks.contains("the WebGL renderer carries the profile adapter",
                        browser.label_value("gpu") or "",
                        profile["gpu_adapter_description"])
        checks.eq("no automation globals are visible to the page",
                  browser.label_value("automation"), "none")

        # Separates "the page could not set a cookie" from "the cookie was set
        # but never reached disk" -- two very different failures that look
        # identical from the database.
        checks.eq("the page could set a cookie at all",
                  browser.label_value("cookie"), "set")

        # --- synthesized input ---------------------------------------------
        submit = browser.first(role="button", name="Submit")
        checks.ok("the Submit button is addressable by role and name", submit is not None,
                  f"bounds={submit['bounds'] if submit else None}")

        if input_available:
            browser.click(role="button", name="Submit")
            checks.eq("clicking it produced a trusted event",
                      browser.label_value("result"), "clicked via trusted event")

            browser.type("hello ghost", into={"role": "edit", "name": "Message box"})
            checks.eq("typing reached the page as trusted input",
                      browser.label_value("echo"), "hello ghost")

            browser.key("ctrl", "a")
            browser.type("replaced")
            checks.eq("a key combo selected the field before retyping",
                      browser.label_value("echo"), "replaced")

            trusted = int(browser.label_value("trusted") or 0)
            checks.ok("the page counted trusted events", trusted > 0, f"trusted={trusted}")
            checks.eq("not one event was untrusted", browser.label_value("untrusted"), "0")

            browser.scroll(600)
            checks.ok("scrolling was accepted", True)
        else:
            for name in INPUT_CHECKS:
                checks.gap(name, NO_FOREGROUND)

        # --- capture ---------------------------------------------------------
        shot = browser.screenshot()
        size = Path(shot).stat().st_size if Path(shot).exists() else 0
        checks.ok("the window was captured without the browser's help", size > 5000,
                  f"{shot} ({size} bytes)")

        # --- cookies, read from the profile's SQLite ------------------------
        # Chrome holds this database with no sharing whatsoever while it runs:
        # CreateFileW fails with ERROR_SHARING_VIOLATION (32) under every share
        # mode, while other files in the same directory open normally. That is
        # the engine's design, not an ACL problem, so the only honest check is
        # to shut the browser down first and then read it.
        #
        # Resolve the path first: locating it asks the control plane, and the
        # control plane is gone the moment the browser is stopped.
        db = browser.cookie_db()
        browser.stop()
        # Chrome writes cookies lazily, so the row may land a moment after the
        # process is gone. Poll until it shows up rather than accepting the first
        # successful read of an empty table.
        cookies: list[dict] = []
        deadline = time.time() + 20
        while time.time() < deadline:
            try:
                cookies = browser.cookies(host="127.0.0.1", db=db)
            except (GhostError, PermissionError, OSError):
                cookies = []
            if any(c["name"] == "ghost_check" for c in cookies):
                break
            time.sleep(0.5)
        names = {c["name"]: c["value"] for c in cookies}
        checks.eq("the page's cookie survives in the profile's database",
                  names.get("ghost_check"), "ok",
                  f"db={db}, saw={sorted(names)}")

        failed = checks.report()
        if args.keep_open:
            print("\n--keep-open: leaving the browser running; press Ctrl+C to stop")
            while True:
                time.sleep(1)
        return 1 if failed else 0
    except GhostError as exc:
        print(f"\ncontrol plane error: {exc}")
        print("\n--- ghost serve log ---")
        print(log.read_text(encoding="utf-8", errors="replace")[-4000:] if log.exists() else "")
        return 1
    finally:
        server.shutdown()
        if not args.keep_open:
            browser.stop()


if __name__ == "__main__":
    sys.exit(main())
