#!/usr/bin/env python3
"""End-to-end fingerprint verification for the ghost browser vertical slice.

Ground truth lives *inside the renderer*. harness/detect.html reads the fingerprint
from page JavaScript exactly as a detector would, then POSTs it back to a collector
bound to loopback. Nothing is read out of the browser over CDP, because the control
plane is deliberately CDP-free -- and that is also the only honest way to measure
what a page can actually see.

Usage:
    python harness/run_detect.py
    python harness/run_detect.py --profile harness/profiles/slice-test-001.json --verbose
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HARNESS = ROOT / "harness"
DEFAULT_CHROME = Path(r"C:\Program Files\Google\Chrome\Application\chrome.exe")
DEFAULT_LAUNCHER = ROOT / "native" / "build" / "bin" / "ghost_launch.exe"
DEFAULT_PROFILE = HARNESS / "profiles" / "slice-test-001.json"


# ---------------------------------------------------------------------------
# collector
# ---------------------------------------------------------------------------
class Collector(BaseHTTPRequestHandler):
    report: dict | None = None
    reports: list[dict] = []
    report_event = threading.Event()
    requests_seen: list[str] = []

    def log_message(self, *args):  # silence the default stderr chatter
        pass

    def do_GET(self):  # noqa: N802
        path = urllib.parse.urlparse(self.path).path
        Collector.requests_seen.append("GET " + path)
        if path in ("/", "/index.html", "/detect.html"):
            body = (HARNESS / "detect.html").read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_response(404)
            self.end_headers()

    def do_POST(self):  # noqa: N802
        path = urllib.parse.urlparse(self.path).path
        length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(length)
        Collector.requests_seen.append("POST " + path)
        if path == "/report":
            try:
                payload = json.loads(raw.decode("utf-8"))
            except Exception as exc:  # noqa: BLE001
                print(f"[collector] report decode failed: {exc}")
                payload = {"phase": 2, "__decode_error": str(exc)}
            Collector.reports.append(payload)
            Collector.report = payload
            # The page reports twice: phase 1 (synchronous fields, immediate) and
            # phase 2 (async probes, timeout-guarded). Only phase 2 completes the run.
            if (payload.get("phase") == 2 or "__pageError" in payload
                    or "__unhandledRejection" in payload):
                Collector.report_event.set()
            self.send_response(204)
            self.end_headers()
        else:
            self.send_response(404)
            self.end_headers()


# ---------------------------------------------------------------------------
# process helpers
# ---------------------------------------------------------------------------
def kill_chrome_for(profile_dir: Path) -> int:
    """Kill only the Chrome instances that belong to *our* user-data-dir."""
    script = (
        "Get-CimInstance Win32_Process -Filter \"Name='chrome.exe'\" | "
        f"Where-Object {{ $_.CommandLine -like '*{profile_dir}*' }} | "
        "ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue; "
        "$_.ProcessId }"
    )
    try:
        out = subprocess.run(["pwsh", "-NoProfile", "-Command", script],
                             capture_output=True, text=True, timeout=60)
        killed = [l for l in out.stdout.splitlines() if l.strip().isdigit()]
        return len(killed)
    except Exception:  # noqa: BLE001
        return 0


def shim_log_tail(lines: int = 25) -> str:
    import os
    path = Path(os.environ.get("TEMP", ".")) / "ghost_shim.log"
    if not path.exists():
        return "(no shim log)"
    try:
        content = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except Exception as exc:  # noqa: BLE001
        return f"(unreadable: {exc})"
    return "\n".join(content[-lines:])


# ---------------------------------------------------------------------------
# assertions
# ---------------------------------------------------------------------------
class Checks:
    def __init__(self) -> None:
        self.rows: list[tuple[str, str, str, str]] = []

    def add(self, verdict: str, name: str, actual, expected: str) -> None:
        self.rows.append((verdict, name, repr(actual), expected))

    def eq(self, name, actual, expected) -> None:
        self.add("PASS" if actual == expected else "FAIL", name, actual, repr(expected))

    def is_false(self, name, actual) -> None:
        self.add("PASS" if actual is False else "FAIL", name, actual, "False")

    def is_true(self, name, actual) -> None:
        self.add("PASS" if actual is True else "FAIL", name, actual, "True")

    def contains(self, name, haystack, needle) -> None:
        ok = isinstance(haystack, str) and needle in haystack
        self.add("PASS" if ok else "FAIL", name, haystack, f"contains {needle!r}")

    def not_contains(self, name, haystack, needle) -> None:
        ok = isinstance(haystack, str) and needle not in haystack
        self.add("PASS" if ok else "FAIL", name, haystack, f"excludes {needle!r}")

    def note(self, name, actual, expected: str = "known gap") -> None:
        self.add("INFO", name, actual, expected)

    def gap(self, name, actual, reason: str) -> None:
        """Structurally unreachable in the current sandbox mode — not a defect.

        Distinct from FAIL so the suite never reports green by lowering the bar, and
        never reports red for something no amount of Track A work can reach.
        """
        self.add("GAP", name, actual, reason)

    def report(self) -> int:
        width = max(len(r[1]) for r in self.rows) if self.rows else 10
        fails = 0
        for verdict, name, actual, expected in self.rows:
            if verdict == "FAIL":
                fails += 1
            print(f"  [{verdict}] {name:<{width}}  actual={actual}  expected={expected}")
        print()
        print(f"  {len(self.rows)} checks, {fails} failed")
        return fails


def build_checks(report: dict, profile: dict, sandboxed: bool = False) -> Checks:
    c = Checks()

    page_errors = {k: v for k, v in report.items() if k.startswith("__")}
    if page_errors:
        c.add("FAIL", "page-side error", page_errors, "none")

    # --- the automation signals ------------------------------------------
    c.is_false("navigator.webdriver", report.get("webdriver"))

    # hardwareConcurrency and deviceMemory are computed *inside the renderer* (Blink
    # calls GetNativeSystemInfo / GlobalMemoryStatusEx there). With the renderer sandbox
    # on, the renderer's restricted token has a restricted SID list of [S-1-0-0] (the
    # NULL SID, grantable by no ordinary ACE), so LoadLibraryW of the shim fails with
    # STATUS_ACCESS_DENIED and no renderer-side hook can install. Those two values are
    # then unreachable by Track A and only Track B (source-level patches) can move them.
    expected_mem_gib = max(1, min(8, round(profile["memory_total_bytes"] / (1024 ** 3))))
    if sandboxed:
        c.gap("navigator.hardwareConcurrency", report.get("hardwareConcurrency"),
              f"renderer sandbox: unreachable (profile wants {profile['cpu_hardware_concurrency']})")
        c.gap("navigator.deviceMemory", report.get("deviceMemory"),
              f"renderer sandbox: unreachable (profile wants {expected_mem_gib})")
    else:
        c.eq("navigator.hardwareConcurrency", report.get("hardwareConcurrency"),
             profile["cpu_hardware_concurrency"])
        c.eq("navigator.deviceMemory", report.get("deviceMemory"), expected_mem_gib)

    c.eq("navigator.platform", report.get("platform"), profile["platform"])

    # --- UA / UA-CH coherence --------------------------------------------
    ua = report.get("userAgent", "")
    c.not_contains("userAgent excludes HeadlessChrome", ua, "HeadlessChrome")
    c.not_contains("userAgent excludes --enable-automation marker", ua, "automation")
    c.contains("userAgent carries the profile Chrome version", ua, "Chrome/154.0.0.0")
    c.contains("userAgent platform token matches navigator.platform", ua,
               "Windows NT 10.0; Win64; x64")

    # --- screen geometry and the DPR/screen coherence rule ----------------
    scr = report.get("screen", {})
    c.eq("screen.width", scr.get("width"), profile["screen_width"])
    c.eq("screen.height", scr.get("height"), profile["screen_height"])
    c.eq("screen.availWidth", scr.get("availWidth"), profile["screen_avail_width"])
    c.eq("screen.availHeight", scr.get("availHeight"), profile["screen_avail_height"])
    c.eq("devicePixelRatio", scr.get("devicePixelRatio"), profile["device_pixel_ratio"])
    c.eq("screen.colorDepth", scr.get("colorDepth"), profile["color_depth"])
    # coherence: physical pixels reported by the OS == CSS pixels * DPR
    c.eq("screen.width * DPR == physical width", scr.get("width", 0) * scr.get("devicePixelRatio", 1),
         profile["screen_width"] * profile["device_pixel_ratio"])
    c.is_true("window fits inside the screen", scr.get("outerWidth", 0) <= scr.get("width", 0))

    # --- timezone / locale coherence -------------------------------------
    intl = report.get("intl", {})
    c.eq("Intl timeZone", intl.get("timeZone"), profile["timezone_id"])
    c.eq("locale", intl.get("locale"), profile["locale"])
    c.eq("tz offset January (GMT)", intl.get("offsetJan"), 0)
    c.eq("tz offset July (BST)", intl.get("offsetJul"), -60)
    c.contains("January abbreviation is GMT", intl.get("tzAbbrevJan", ""), "GMT")
    c.contains("July abbreviation is BST", intl.get("tzAbbrevJul", ""), "BST")
    c.contains("number formatting follows en-GB", intl.get("numberFormat", ""), "1,234,567.891")

    # --- framework leakage ------------------------------------------------
    leaked = [k for k, v in (report.get("automationGlobals") or {}).items() if v]
    c.add("PASS" if not leaked else "FAIL", "no automation globals leak", leaked, "[]")

    # --- CDP surface ------------------------------------------------------
    probe = report.get("cdpProbe") or {}
    open_ports = [p for p, v in probe.items() if v == "OPEN"]
    c.add("PASS" if not open_ports else "FAIL", "no CDP port answers", probe, "all closed")

    # --- native-ness of the hooked surfaces ------------------------------
    getters = report.get("nativeGetters") or {}
    non_native = []
    for key in ("webdriver", "hardwareConcurrency", "deviceMemory", "platform",
                "userAgent", "languages", "screenWidth", "screenHeight"):
        src = getters.get(key)
        if not src or "[native code]" not in src:
            non_native.append(key)
    c.add("PASS" if not non_native else "FAIL", "hooked getters still read as native",
          non_native, "[]")

    # --- WebGL: the GPU-process surface Track A can now move --------------
    #
    # ANGLE builds GL_RENDERER itself, as
    #   "ANGLE (<vendor>, <adapter description> (0x<device id>) Direct3D11 vs_5_0 ps_5_0, D3D11)"
    # so the honest assertion is that ANGLE saw *our* adapter identity — not that the
    # finished string equals a literal we hard-coded. Asserting the literal would pass
    # even if ANGLE's own formatting had changed underneath us.
    gl = report.get("webgl") or {}
    renderer = gl.get("unmaskedRenderer") or ""
    desc = profile.get("gpu_adapter_description")
    dev = profile.get("gpu_adapter_device_id")
    if desc:
        c.contains("webgl renderer carries the profile adapter", renderer, desc)
        if dev:
            c.contains("webgl renderer carries the profile device id", renderer,
                       f"(0x{dev:08X})")
        c.note("webgl.unmaskedRenderer (composed by ANGLE)", renderer,
               "ANGLE formats it from the spoofed DXGI adapter")
    else:
        c.note("webgl.unmaskedRenderer", renderer, f"profile wants {profile['gpu_renderer']!r}")

    # --- known gaps: Blink-internal surfaces not yet covered (Track A) ----
    c.note("canvas.dataUrlHash", (report.get("canvas") or {}).get("dataUrlHash"),
           "unspoofed Blink/Skia path")
    c.note("audio.fingerprint", (report.get("audio") or {}).get("fingerprint"),
           "unspoofed Blink/DSP path")
    c.note("fonts.presentCount", (report.get("fonts") or {}).get("presentCount"),
           "font *metrics* still come from the real host font set")
    c.note("plugins.pluginCount", (report.get("plugins") or {}).get("pluginCount"))
    c.note("chrome.hasChrome", (report.get("chrome") or {}).get("hasChrome"))

    return c


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------
def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--profile", type=Path, default=DEFAULT_PROFILE)
    ap.add_argument("--chrome", type=Path, default=DEFAULT_CHROME)
    ap.add_argument("--launcher", type=Path, default=DEFAULT_LAUNCHER)
    ap.add_argument("--port", type=int, default=8731)
    ap.add_argument("--timeout", type=float, default=90.0)
    ap.add_argument("--user-data-dir", type=Path,
                    default=HARNESS / "chrome-profile-detect")
    ap.add_argument("--keep-open", action="store_true",
                    help="leave Chrome running after the report arrives")
    ap.add_argument("--fresh", action="store_true", default=True,
                    help="wipe the user-data-dir before launching (default)")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--chrome-arg", action="append", default=[], metavar="FLAG",
                    help="extra flag forwarded to chrome (repeatable)")
    ap.add_argument("--sandboxed", action="store_true",
                    help="keep the Chromium sandbox; renderer-side hooks will NOT install")
    args = ap.parse_args()

    for label, path in (("chrome", args.chrome), ("launcher", args.launcher),
                        ("profile", args.profile)):
        if not path.exists():
            print(f"missing {label}: {path}")
            return 2

    profile = json.loads(args.profile.read_text(encoding="utf-8"))

    # Chrome holds an exclusive lock on the shim DLL and on the profile dir.
    killed = kill_chrome_for(args.user_data_dir)
    if killed:
        print(f"stopped {killed} stale chrome process(es) for this profile")
        time.sleep(2.0)
    if args.fresh and args.user_data_dir.exists():
        shutil.rmtree(args.user_data_dir, ignore_errors=True)
    args.user_data_dir.mkdir(parents=True, exist_ok=True)

    server = ThreadingHTTPServer(("127.0.0.1", args.port), Collector)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    url = f"http://127.0.0.1:{args.port}/"
    print(f"collector listening on {url}")

    chrome_args = [
        str(args.chrome),
        f"--user-data-dir={args.user_data_dir}",
        "--no-first-run",
        "--no-default-browser-check",
        "--disable-session-crashed-bubble",
        f"--window-size={profile['window_width']},{profile['window_height']}",
        *args.chrome_arg,
        url,
    ]
    # Chromium's renderer sandbox runs each renderer under an Untrusted-integrity,
    # restricted token whose restricted SID list is [S-1-0-0] (the NULL SID, which no
    # ordinary ACE can grant). LoadLibraryW of our shim inside such a process fails with
    # STATUS_ACCESS_DENIED (0xC0000022), so renderer-side hooks — hardwareConcurrency and
    # deviceMemory — cannot install while the sandbox is on. Verified both ways: with the
    # sandbox off the shim loads in the browser and in every renderer. Track B
    # (source-level patches) removes the need for renderer injection altogether.
    if not args.sandboxed:
        chrome_args.insert(-1, "--no-sandbox")
    cmd = [str(args.launcher), "--profile", str(args.profile)]
    if profile.get("timezone_id"):
        cmd += ["--tz", profile["timezone_id"]]
    # --lang is what actually sets Chromium's application locale (and hence Intl's default
    # locale); the shim's GetUserDefaultLocaleName hook alone does not, because the engine
    # resolves and caches that locale before the shim's hooks are live.
    if profile.get("locale"):
        cmd += ["--lang", profile["locale"]]
    if args.verbose:
        cmd.append("--verbose")
    cmd += ["--no-wait", "--", *chrome_args]

    print("launching: " + " ".join(cmd))
    proc = subprocess.Popen(cmd, cwd=str(ROOT), stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)

    deadline = time.time() + args.timeout
    while time.time() < deadline and not Collector.report_event.is_set():
        if proc.poll() is not None and proc.returncode not in (0, None):
            print(f"launcher exited early with code {proc.returncode}")
            break
        time.sleep(0.4)

    print()
    if not Collector.report_event.is_set() and not Collector.reports:
        print("NO REPORT RECEIVED within the timeout.")
        print("collector saw: " + (", ".join(Collector.requests_seen) or "(nothing)"))
        print("\n--- shim log tail ---")
        print(shim_log_tail())
        server.shutdown()
        return 1

    if not Collector.report_event.is_set():
        print("WARNING: the page never sent its phase-2 report "
              f"({len(Collector.reports)} phase-1 payload(s) seen); "
              "falling back to the last one received.")
        print()

    report = Collector.report or {}
    if args.verbose:
        print(json.dumps(report, indent=2, ensure_ascii=False))

    out_path = HARNESS / "detect_report.json"
    out_path.write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
    print(f"report written to {out_path}\n")

    fails = build_checks(report, profile, args.sandboxed).report()

    if not args.keep_open:
        kill_chrome_for(args.user_data_dir)
    server.shutdown()
    print("\n--- shim log tail ---")
    print(shim_log_tail())
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
