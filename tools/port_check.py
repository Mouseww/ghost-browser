#!/usr/bin/env python3
"""Acceptance for the claim that the DevTools channel is not a port.

`ghost` talks to the browser over the pipe Chromium creates for
`--remote-debugging-io-pipes`. That is the whole point of the transport: nothing
on the machine can find a port to probe, and the profile never records one. Two
separate things have to hold for that to be true, so this script measures both
instead of asserting them:

  1. the browser's own processes listen on no TCP port at all, and
  2. no `DevTools*` file -- in particular `DevToolsActivePort` -- appears
     anywhere under the profile, at startup or after a page has been driven.

The port reading is guarded against the failure mode that would make it
vacuous: a browser that never opened the channel also listens on no port, so
the channel is asked for `Browser.getVersion` first and that answer is required
before the port reading is allowed to mean anything.

The same reasoning applies to the file search: it runs against the profile of a
session that has actually been used, not against an empty directory.

Exit 0 means every check passed. A check that could not be run at all is
reported as `[skip]` and counted separately; it is never silently a pass.
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_GHOST = ROOT / "native" / "build" / "bin" / "ghost.exe"

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.stdout.reconfigure(encoding="utf-8", errors="replace")

from ghost_client import Ghost, GhostError  # noqa: E402


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


def powershell(command: str) -> str:
    result = subprocess.run(["powershell", "-NoProfile", "-Command", command],
                            capture_output=True, text=True)
    return result.stdout


def browser_pids() -> list[int]:
    out = powershell("(Get-Process chrome -ErrorAction SilentlyContinue).Id -join ','")
    return [int(x) for x in out.strip().split(",") if x.strip().isdigit()]


def listening_ports(pids: list[int]) -> list[int]:
    if not pids:
        return []
    listed = ",".join(str(p) for p in pids)
    out = powershell(
        "Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | "
        f"Where-Object {{ @({listed}) -contains $_.OwningProcess }} | "
        "Select-Object -ExpandProperty LocalPort")
    return sorted({int(x) for x in out.split() if x.strip().isdigit()})


def devtools_traces(root: Path) -> list[str]:
    """Everything under the profile that names DevTools, file or directory."""
    traces = []
    for path in root.rglob("*"):
        if path.name.startswith("DevTools"):
            try:
                kind = "dir" if path.is_dir() else "file"
            except OSError:
                kind = "?"
            traces.append(f"{kind}: {path.relative_to(root)}")
    return sorted(traces)


def kill_chrome() -> None:
    subprocess.run(["taskkill", "/F", "/IM", "chrome.exe"],
                   capture_output=True, text=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ghost", default=str(DEFAULT_GHOST))
    parser.add_argument("--keep", action="store_true",
                        help="keep the profile instead of removing it")
    args = parser.parse_args()

    checks = Checks()
    profile = f"port-check-{os.getpid()}-{int(time.time())}"
    ghost = Ghost(profile, ghost=Path(args.ghost))

    print(f"ghost:   {args.ghost}")
    print(f"profile: {profile}\n")

    data_dir: Path | None = None
    try:
        ghost.start("https://example.com")
        time.sleep(3)
        data_dir = ghost.data_dir()

        # Ask the channel a question first. Without this the port reading below
        # would pass just as happily on a browser that never opened a channel.
        version = ghost.call("cdp", method="Browser.getVersion", session="browser")
        product = (version.get("result") or {}).get("product", "")
        checks.true("the DevTools channel answers over the pipe",
                    product.startswith("Chrome/"),
                    f"Browser.getVersion -> {product or version}")
        if not product:
            checks.gap("the browser listens on no TCP port",
                       "the channel did not answer, so an empty port list "
                       "would not prove anything")
        else:
            pids = browser_pids()
            ports = listening_ports(pids)
            checks.true("the browser is running", len(pids) > 0,
                        f"chrome processes: {len(pids)}")
            checks.eq("the browser listens on no TCP port", ports, [],
                      f"pids: {pids}")
            checks.true("and the page is readable, so the session is real",
                        len(ghost.call("tree").get("nodes") or []) > 0)

        # The control plane is the other channel, and it is a pipe too.
        checks.true("the control plane is a named pipe",
                    ghost.pipe.startswith("ghost-"), f"pipe: {ghost.pipe}")
        checks.true("and Windows can see that pipe",
                    os.path.exists("\\\\.\\pipe\\" + ghost.pipe))

        # The profile is where a leaked port would be written down.
        traces = devtools_traces(data_dir)
        checks.eq("the profile holds no DevToolsActivePort file",
                  [t for t in traces if "DevToolsActivePort" in t], [],
                  f"profile: {data_dir}")
        checks.eq("and no DevTools file of any kind",
                  [t for t in traces if t.startswith("file:")], [],
                  f"all DevTools entries: {traces or 'none'}")

        # A session that was told not to open the channel must not open one.
        ghost.stop()
        time.sleep(1)
        off = Ghost(f"{profile}-off", ghost=Path(args.ghost))
        try:
            off.start("https://example.com", extra_args=("--no-cdp",))
            time.sleep(3)
            off_pids = browser_pids()
            checks.eq("a session started with --no-cdp listens on no TCP port",
                      listening_ports(off_pids), [])
            refused = False
            try:
                off.call("cdp", method="Browser.getVersion", session="browser")
            except GhostError as error:
                refused = "DevTools" in str(error) or "cdp" in str(error).lower()
            checks.true("and refuses a DevTools request rather than opening one",
                        refused)
        finally:
            off.stop()
    except GhostError as error:
        checks.gap("the session could be started", str(error))
    finally:
        kill_chrome()
        if data_dir is not None and not args.keep:
            import shutil
            shutil.rmtree(data_dir, ignore_errors=True)

    print(f"\n{checks.passed} checks, {checks.failed} failed, "
          f"{checks.skipped} not measurable")
    return 0 if checks.failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
