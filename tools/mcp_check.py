#!/usr/bin/env python3
"""Acceptance test for the ghost MCP server.

Spawns `mcp/ghost_mcp.py` the way an MCP client does — as a child process talking
newline-delimited JSON-RPC on stdin/stdout — and drives a real browser through it.
This is the test that proves the agent path works end to end, not just that the
file imports.

  python tools/mcp_check.py [--timeout 120] [--keep-open] [--verbose]

Exit code 0 means every check passed.
"""

from __future__ import annotations

import argparse
import json
import os
import queue
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MCP_SERVER = ROOT / "mcp" / "ghost_mcp.py"

sys.stdout.reconfigure(encoding="utf-8", errors="replace")


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

    def contains(self, name: str, haystack: str, needle: str) -> bool:
        if needle in haystack:
            self.passed += 1
            print(f"[ ok ] {name}")
            return True
        self.failed += 1
        print(f"[FAIL] {name}")
        print(f"       {needle!r} not found in:")
        print("       " + (haystack[:400] or "(empty)"))
        return False

    def gap(self, name: str, why: str) -> None:
        self.skipped += 1
        print(f"[skip] {name}: {why}")


class McpClient:
    """A minimal MCP client: write a JSON-RPC message, read the reply with the
    matching id. Responses arrive on a reader thread so a missing reply times out
    instead of hanging the whole test."""

    def __init__(self, env: dict, verbose: bool = False):
        self.verbose = verbose
        self.inbox: queue.Queue = queue.Queue()
        self.stderr_lines: list[str] = []
        self.process = subprocess.Popen(
            [sys.executable, str(MCP_SERVER)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            env=env, cwd=str(ROOT), text=True, encoding="utf-8", errors="replace",
            bufsize=1,
        )
        threading.Thread(target=self._pump_stdout, daemon=True).start()
        threading.Thread(target=self._pump_stderr, daemon=True).start()

    def _pump_stdout(self) -> None:
        for line in self.process.stdout:
            line = line.strip()
            if not line:
                continue
            try:
                self.inbox.put(json.loads(line))
            except ValueError:
                if self.verbose:
                    print(f"       (non-JSON on stdout: {line[:120]})")

    def _pump_stderr(self) -> None:
        for line in self.process.stderr:
            self.stderr_lines.append(line.rstrip())

    def send(self, message: dict) -> None:
        self.process.stdin.write(json.dumps(message) + "\n")
        self.process.stdin.flush()

    def request(self, method: str, params: dict | None = None, timeout: float = 60.0):
        request_id = self._next_id()
        payload = {"jsonrpc": "2.0", "id": request_id, "method": method}
        if params is not None:
            payload["params"] = params
        self.send(payload)
        deadline = time.time() + timeout
        deferred = []
        try:
            while time.time() < deadline:
                try:
                    message = self.inbox.get(timeout=max(0.1, deadline - time.time()))
                except queue.Empty:
                    break
                if message.get("id") == request_id:
                    for item in deferred:
                        self.inbox.put(item)
                    return message
                deferred.append(message)
        finally:
            for item in deferred:
                self.inbox.put(item)
        raise TimeoutError(f"no reply to {method} within {timeout:.0f}s")

    _counter = 0

    def _next_id(self) -> int:
        McpClient._counter += 1
        return McpClient._counter

    def notify(self, method: str, params: dict | None = None) -> None:
        payload = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            payload["params"] = params
        self.send(payload)

    def call_tool(self, name: str, arguments: dict | None = None, timeout: float = 180.0) -> dict:
        message = self.request("tools/call",
                               {"name": name, "arguments": arguments or {}},
                               timeout=timeout)
        if "error" in message:
            raise RuntimeError(f"{name}: {message['error']}")
        return message["result"]

    def close(self) -> None:
        try:
            self.process.stdin.close()
        except Exception:
            pass
        try:
            self.process.wait(timeout=20)
        except Exception:
            self.process.kill()


def text_of(result: dict) -> str:
    return "\n".join(block.get("text", "") for block in result.get("content", [])
                     if block.get("type") == "text")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout", type=float, default=120.0,
                        help="seconds to wait for the browser to come up")
    parser.add_argument("--keep-open", action="store_true")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    checks = Checks()

    exe = ROOT / "native" / "build" / "bin" / "ghost.exe"
    if not exe.is_file():
        print(f"ghost.exe not found at {exe}; build it first")
        return 2

    env = dict(os.environ)
    env["GHOST_EXE"] = str(exe)
    env["GHOST_ID"] = "mcp-check"
    env["GHOST_URL"] = "https://example.com"
    env["GHOST_TIMEOUT"] = str(args.timeout)
    env["PYTHONIOENCODING"] = "utf-8"

    client = McpClient(env, args.verbose)
    started = time.time()
    try:
        # --- handshake ----------------------------------------------------
        message = client.request("initialize", {
            "protocolVersion": "2025-06-18",
            "capabilities": {},
            "clientInfo": {"name": "mcp_check", "version": "1.0"},
        }, timeout=30)
        result = message.get("result") or {}
        checks.eq("initialize returns a protocol version",
                  result.get("protocolVersion"), "2025-06-18")
        checks.eq("initialize advertises the ghost server",
                  (result.get("serverInfo") or {}).get("name"), "ghost")
        checks.true("initialize advertises tools",
                    (result.get("capabilities") or {}).get("tools") is not None)
        client.notify("notifications/initialized")

        # --- tool discovery ------------------------------------------------
        tools = (client.request("tools/list", {}, timeout=30).get("result") or {}).get("tools") or []
        names = sorted(tool["name"] for tool in tools)
        print(f"       tools: {', '.join(names)}")
        checks.true("tools/list returns tools", len(tools) >= 10)
        for required in ("ghost_open", "ghost_status", "ghost_navigate", "ghost_page",
                         "ghost_find", "ghost_click", "ghost_type", "ghost_key",
                         "ghost_scroll", "ghost_screenshot", "ghost_captcha",
                         "ghost_close"):
            checks.true(f"tool {required} is offered", required in names)
        checks.true("every tool has an input schema",
                    all("inputSchema" in tool for tool in tools))

        # --- a real browser, driven through MCP ---------------------------
        # Starting *at* the URL costs no synthesized input, so every check below that
        # only reads the page works even in a session with no foreground window. The
        # address-bar navigation is the one step that needs one, and it is graded as
        # unmeasurable there instead of taking the read-only checks down with it.
        result = client.call_tool("ghost_open", {"url": "https://example.com"},
                                  timeout=args.timeout)
        checks.true("ghost_open starts the browser", not result.get("isError"),
                    text_of(result))
        print("       " + text_of(result).replace("\n", "\n       "))

        result = client.call_tool("ghost_status")
        status_text = text_of(result)
        checks.contains("ghost_status reports a pid", status_text, "pid:")
        checks.true("ghost_status is not an error", not result.get("isError"), status_text)

        result = client.call_tool("ghost_page", {"max_nodes": 400})
        page_text = text_of(result)
        checks.true("ghost_page returns elements", "elements" in page_text, page_text[:200])
        # The document node carries id=RootWebArea, which no browser-chrome element
        # ever has. An earlier version of this check matched the string "Example
        # Domain" and passed on the *window title* while the page itself was invisible,
        # which is exactly the failure it was supposed to catch.
        checks.contains("ghost_page reaches the document", page_text, "id=RootWebArea")

        result = client.call_tool("ghost_find", {"role": "link"})
        find_text = text_of(result)
        head = (find_text.splitlines() or [""])[0].split()
        link_count = int(head[0]) if head and head[0].isdigit() else -1
        checks.true("ghost_find sees page links", link_count > 0, find_text[:300])

        # example.com carries no human-verification widget, so the honest answer is
        # "none" — not an error, and not a provider invented out of the page's own URL,
        # which is the bug ARCHITECTURE.md §14.3 records.
        result = client.call_tool("ghost_captcha", {"action": "detect"})
        captcha_text = text_of(result)
        checks.true("ghost_captcha runs", not result.get("isError"), captcha_text)
        checks.contains("ghost_captcha finds no challenge on a plain page",
                        captcha_text, "no human-verification challenge")

        # The audio route has to refuse for the same reason, and refuse before it
        # touches the audio device: a page with no challenge must not open a recording
        # just because it was asked to.
        result = client.call_tool("ghost_captcha", {"action": "solve-audio", "seconds": 2})
        audio_text = text_of(result)
        checks.true("ghost_captcha solve-audio runs", not result.get("isError"), audio_text)
        checks.contains("ghost_captcha solve-audio refuses a plain page",
                        audio_text, "no human-verification challenge")

        # Navigation drives the real address bar, so it needs a foreground window.
        result = client.call_tool("ghost_navigate", {"url": "https://example.com"},
                                  timeout=args.timeout)
        navigate_text = text_of(result)
        if result.get("isError") and "foreground" in navigate_text:
            checks.gap("ghost_navigate reaches the page",
                       "this session has no foreground window")
        else:
            checks.contains("ghost_navigate reports the page title",
                            navigate_text, "Example Domain")

        # Screenshots work in any session, foreground or not.
        result = client.call_tool("ghost_screenshot")
        blocks = result.get("content") or []
        images = [b for b in blocks if b.get("type") == "image"]
        if not images:
            checks.true("ghost_screenshot returns an image", False, text_of(result))
        else:
            import base64
            raw = base64.b64decode(images[0]["data"])
            checks.eq("ghost_screenshot declares PNG", images[0]["mimeType"], "image/png")
            checks.eq("ghost_screenshot is a real PNG", raw[:8],
                      b"\x89PNG\r\n\x1a\n")
            checks.true("ghost_screenshot has a plausible size", len(raw) > 5000,
                        f"{len(raw)} bytes")

        # --- error handling ------------------------------------------------
        result = client.call_tool("ghost_find", {})
        checks.true("a tool with no arguments reports an error", result.get("isError"))
        checks.contains("the error explains what is missing", text_of(result), "role")

        # --- shutdown ------------------------------------------------------
        if not args.keep_open:
            result = client.call_tool("ghost_close")
            checks.contains("ghost_close confirms", text_of(result), "closed")
    finally:
        if args.keep_open:
            print("       leaving the browser running (--keep-open)")
        client.close()

    total = checks.passed + checks.failed
    print()
    print(f"{total} checks, {checks.failed} failed, {checks.skipped} not measurable "
          f"in this session")
    if checks.failed:
        print("--- server stderr (tail) ---")
        for line in client.stderr_lines[-25:]:
            print("   " + line)
    return 1 if checks.failed else 0


if __name__ == "__main__":
    sys.exit(main())
