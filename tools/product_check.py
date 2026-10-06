"""End-to-end acceptance check of the shipped ghost.exe.

This is deliberately not the developer harness. `harness/run_detect.py` drives
`ghost_launch` with an explicit profile file, which proves the shim works but
says nothing about the thing users actually download. Here the only inputs are
the built `ghost.exe` and a URL: the product has to find the browser, invent a
coherent profile, extract its embedded shim, inject it, and survive the
detection page on its own. The grading assertions are reused from the harness so
the two cannot drift apart.

    python tools/product_check.py
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import subprocess
import sys
import threading
import time
from http.server import ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HARNESS = ROOT / "harness"
GHOST = ROOT / "native" / "build" / "bin" / "ghost.exe"

# The detection page reports whatever the browser's locale is, so the console's
# default code page (GBK on this host) cannot encode the output.
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

# The harness is a script, not a package; load it by path so its collector and
# its assertions can be reused verbatim.
_spec = importlib.util.spec_from_file_location("run_detect", HARNESS / "run_detect.py")
rd = importlib.util.module_from_spec(_spec)
assert _spec.loader is not None
_spec.loader.exec_module(rd)


def ghost(exe: Path, args: list[str], timeout: float = 120.0) -> subprocess.CompletedProcess:
    return subprocess.run([str(exe), *args], cwd=str(ROOT), capture_output=True,
                          text=True, timeout=timeout)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--ghost", type=Path, default=GHOST,
                    help="the ghost.exe to grade; point this at a release asset to "
                         "check exactly what users download")
    ap.add_argument("--id", default="product-check", help="profile name to use")
    ap.add_argument("--seed", default="", help="profile seed; random when omitted")
    # A profile that inherits the host's zone and locale would make every
    # timezone and locale assertion vacuous, because the browser would be
    # reporting the host's own values either way. The defaults are deliberately
    # somewhere else.
    ap.add_argument("--tz", default="Europe/London")
    ap.add_argument("--locale", default="en-GB")
    ap.add_argument("--port", type=int, default=8732)
    ap.add_argument("--timeout", type=float, default=90.0)
    ap.add_argument("--keep-open", action="store_true")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    exe = args.ghost.resolve()
    if not exe.exists():
        print(f"missing {exe}; build first:\n"
              "  python -m cmake --build native/build --config Release")
        return 2

    # A fresh profile every run, so the check cannot pass on a stale file.
    create = ["profile", "new", "--id", args.id, "--force",
              "--tz", args.tz, "--locale", args.locale]
    if args.seed:
        create += ["--seed", args.seed]
    made = ghost(exe, create)
    print(f"$ {exe.name} " + " ".join(create))
    print(made.stdout.strip() or made.stderr.strip())
    if made.returncode != 0:
        return made.returncode

    shown = ghost(exe, ["profile", "show", "--id", args.id])
    if shown.returncode != 0:
        print(shown.stderr.strip())
        return shown.returncode
    profile = json.loads(shown.stdout)
    print(f"profile: {profile['cpu_hardware_concurrency']} cores, "
          f"{profile['memory_total_bytes'] // (1 << 30)} GiB, "
          f"{profile['screen_width']}x{profile['screen_height']} @ "
          f"{profile['device_pixel_ratio']}, {profile['timezone_id']}, "
          f"{profile['locale']}, {profile['gpu_adapter_description']}")

    data_dir = ROOT / "harness" / f"chrome-profile-{args.id}"
    killed = rd.kill_chrome_for(data_dir)
    if killed:
        print(f"stopped {killed} stale chrome process(es)")
        time.sleep(2.0)

    server = ThreadingHTTPServer(("127.0.0.1", args.port), rd.Collector)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    url = f"http://127.0.0.1:{args.port}/"
    print(f"collector listening on {url}")

    cmd = [
        "browse", "--id", args.id, "--no-wait",
        f"--user-data-dir={data_dir}",
        f"--chrome-arg=--window-size={profile['window_width']},{profile['window_height']}",
    ]
    if args.verbose:
        cmd.append("--verbose")
    cmd.append(url)

    print(f"$ {exe.name} " + " ".join(cmd))
    proc = subprocess.Popen([str(exe), *cmd], cwd=str(ROOT), stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)

    deadline = time.time() + args.timeout
    while time.time() < deadline and not rd.Collector.report_event.is_set():
        if proc.poll() is not None and proc.returncode not in (0, None):
            print(f"ghost exited early with code {proc.returncode}")
            break
        time.sleep(0.4)

    print()
    if not rd.Collector.report_event.is_set() and not rd.Collector.reports:
        print("NO REPORT RECEIVED within the timeout.")
        print("collector saw: " + (", ".join(rd.Collector.requests_seen) or "(nothing)"))
        print("\n--- shim log tail ---")
        print(rd.shim_log_tail())
        server.shutdown()
        return 1

    report = rd.Collector.report or {}
    if args.verbose:
        print(json.dumps(report, indent=2, ensure_ascii=False))

    (HARNESS / "product_check_report.json").write_text(
        json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")

    # The product runs the browser without the sandbox, so no check is
    # structurally unreachable and every one of them is graded.
    fails = rd.build_checks(report, profile, False).report()

    if not args.keep_open:
        rd.kill_chrome_for(data_dir)
    server.shutdown()
    print("\n--- shim log tail ---")
    print(rd.shim_log_tail())
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
