#!/usr/bin/env python3
"""ghost MCP server — drive the ghost browser from any MCP client.

This is a Model Context Protocol server over stdio. It exposes the ghost control
plane (see docs/PROTOCOL.md) as tools an agent can call, and it owns the browser
lifecycle: the first tool call starts `ghost serve` if it is not already running,
and `ghost_close` shuts it down.

Nothing here is ghost-specific machinery — it is a thin adapter. The value it adds
over handing an agent a pipe is:

  * the browser is started, waited for, and torn down automatically;
  * the accessibility tree is rendered as compact text instead of raw JSON, which
    is what a model can actually read;
  * screenshots come back as PNG (MCP only carries PNG/JPEG) and as an image
    content block the model can look at;
  * input commands that cannot work in a headless or disconnected session fail
    with a clear explanation instead of silently doing nothing.

Standard library only. Run it with `python mcp/ghost_mcp.py`.

Environment:
  GHOST_EXE      path to ghost.exe (otherwise searched: PATH, then
                 %LOCALAPPDATA%\\GhostBrowser\\bin\\ghost.exe, then the build tree)
  GHOST_ID       profile / pipe id (default: mcp)
  GHOST_URL      URL to open when the browser starts (optional)
  GHOST_TIMEOUT  seconds to wait for the browser to come up (default: 60)
"""

from __future__ import annotations

import base64
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from pathlib import Path

PROTOCOL_VERSION = "2025-06-18"
SUPPORTED_PROTOCOL_VERSIONS = ("2025-06-18", "2025-03-26", "2024-11-05")

SERVER_NAME = "ghost"
SERVER_VERSION = "0.9.0"

DEFAULT_ID = os.environ.get("GHOST_ID") or "mcp"
READY_TIMEOUT = float(os.environ.get("GHOST_TIMEOUT") or 60)


def log(message: str) -> None:
    """Diagnostics go to stderr: stdout carries the JSON-RPC stream and a stray
    print there corrupts the session."""
    print(f"ghost-mcp: {message}", file=sys.stderr, flush=True)


# --------------------------------------------------------------------------
# locating ghost.exe
# --------------------------------------------------------------------------

def find_ghost_exe() -> str | None:
    override = os.environ.get("GHOST_EXE")
    if override:
        return override if Path(override).is_file() else None

    on_path = shutil.which("ghost") or shutil.which("ghost.exe")
    if on_path:
        return on_path

    local = os.environ.get("LOCALAPPDATA")
    if local:
        installed = Path(local) / "GhostBrowser" / "bin" / "ghost.exe"
        if installed.is_file():
            return str(installed)

    # Running from a checkout: the build tree next to this file.
    here = Path(__file__).resolve().parent.parent
    for candidate in (here / "native" / "build" / "bin" / "ghost.exe",
                      here / "dist" / "ghost.exe"):
        if candidate.is_file():
            return str(candidate)
    return None


# --------------------------------------------------------------------------
# the pipe client
# --------------------------------------------------------------------------

class PipeError(RuntimeError):
    pass


class Ghost:
    """One connection to a running `ghost serve`.

    The protocol is line-delimited JSON over a named pipe (docs/PROTOCOL.md). A
    fresh connection per request is the simplest correct thing: the server handles
    one connection at a time and closing after each request avoids any chance of a
    stale half-read line desynchronising the next call.
    """

    def __init__(self, pipe: str):
        self.pipe = pipe
        self.path = rf"\\.\pipe\{pipe}"

    def call(self, cmd: str, timeout: float = 120.0, **params) -> dict:
        request = json.dumps({"cmd": cmd, **params}, ensure_ascii=False)
        deadline = time.time() + timeout
        last: Exception | None = None
        while time.time() < deadline:
            try:
                with open(self.path, "r+b", buffering=0) as handle:
                    handle.write((request + "\n").encode("utf-8"))
                    line = _read_line(handle, deadline - time.time())
                if not line:
                    last = PipeError("the server closed the connection without answering")
                else:
                    return json.loads(line.decode("utf-8"))
            except FileNotFoundError as exc:
                # The pipe does not exist yet: the server is still starting, or it
                # has exited. Retrying is right for the first case and harmless for
                # the second, because the caller has a deadline.
                last = exc
                time.sleep(0.25)
            except (OSError, ValueError) as exc:
                last = exc
                time.sleep(0.25)
        raise PipeError(f"{cmd} failed after {timeout:.0f}s: {last}")


def _read_line(handle, budget: float) -> bytes:
    """Read up to and including a newline. Reading a pipe is not seekable, so this
    accumulates one byte at a time — the responses are small and correctness beats
    cleverness here."""
    end = time.time() + max(budget, 1.0)
    chunks = bytearray()
    while time.time() < end:
        try:
            piece = handle.read(1)
        except OSError:
            break
        if not piece:
            break
        if piece == b"\n":
            return bytes(chunks)
        chunks += piece
    return bytes(chunks)


# --------------------------------------------------------------------------
# the session: owns the ghost serve process
# --------------------------------------------------------------------------

class Session:
    def __init__(self, exe: str, profile_id: str, url: str | None):
        self.exe = exe
        self.id = profile_id
        self.url = url
        self.process: subprocess.Popen | None = None
        self.log_path: Path | None = None
        self.ghost = Ghost(profile_id)
        self.started_at = 0.0

    # -- lifecycle ---------------------------------------------------------

    def ensure(self) -> None:
        if self._alive():
            return
        self.start()

    def _alive(self) -> bool:
        if self.process is None or self.process.poll() is not None:
            return False
        try:
            response = self.ghost.call("status", timeout=5.0)
        except PipeError:
            return False
        return bool(response.get("ok"))

    def start(self) -> None:
        self.log_path = Path(tempfile.gettempdir()) / f"ghost-mcp-{self.id}.log"
        args = [self.exe, "serve", "--id", self.id, "--pipe", self.id]
        if self.url:
            args.append(self.url)

        log(f"starting: {' '.join(args)}")
        creationflags = 0
        if hasattr(subprocess, "CREATE_NO_WINDOW"):
            creationflags = subprocess.CREATE_NO_WINDOW
        # Redirect to a file, never to a pipe: ghost serve launches a browser that
        # outlives it, and a browser holding the write end of a pipe keeps the
        # parent's reader from ever seeing EOF.
        self.log_file = open(self.log_path, "wb")
        self.process = subprocess.Popen(
            args, stdout=self.log_file, stderr=subprocess.STDOUT,
            stdin=subprocess.DEVNULL, creationflags=creationflags,
            cwd=str(Path(self.exe).parent),
        )
        self.started_at = time.time()

        deadline = time.time() + READY_TIMEOUT
        while time.time() < deadline:
            if self.process.poll() is not None:
                raise PipeError(
                    f"ghost serve exited with code {self.process.returncode}; "
                    f"log: {self.log_path}")
            try:
                response = self.ghost.call("status", timeout=3.0)
                if response.get("ok"):
                    log(f"ready after {time.time() - self.started_at:.1f}s")
                    return
            except PipeError:
                pass
            time.sleep(0.5)
        raise PipeError(f"the browser did not come up within {READY_TIMEOUT:.0f}s; "
                        f"log: {self.log_path}")

    def stop(self) -> None:
        if self.process is None:
            return
        try:
            self.ghost.call("shutdown", timeout=10.0)
        except Exception:
            pass
        try:
            self.process.wait(timeout=15)
        except Exception:
            self.process.kill()
        self.process = None
        if self.log_file:
            try:
                self.log_file.close()
            except Exception:
                pass
            self.log_file = None
        log("stopped")

    # -- convenience -------------------------------------------------------

    def call(self, cmd: str, **params) -> dict:
        self.ensure()
        response = self.ghost.call(cmd, **params)
        if not response.get("ok"):
            raise PipeError(response.get("error") or f"{cmd} failed")
        return response


# --------------------------------------------------------------------------
# rendering
# --------------------------------------------------------------------------

def render_nodes(nodes: list[dict]) -> str:
    """The tree as text. A model reads this far better than nested JSON, and the
    `index` it prints is exactly the handle `ghost_click` takes."""
    if not nodes:
        return "(no elements)"
    lines = []
    for node in nodes:
        depth = max(0, min(int(node.get("depth", 0)), 24))
        parts = [f"[{node.get('index')}]", "  " * depth, str(node.get("role") or "?")]
        name = (node.get("name") or "").strip()
        if name:
            parts.append(f'"{name[:120]}"')
        value = (node.get("value") or "").strip()
        if value:
            parts.append(f"value={value[:60]!r}")
        if node.get("id"):
            parts.append(f"id={node['id']}")
        flags = []
        if node.get("focused"):
            flags.append("focused")
        if not node.get("enabled", True):
            flags.append("disabled")
        if node.get("offscreen"):
            flags.append("offscreen")
        if flags:
            parts.append("<" + ",".join(flags) + ">")
        bounds = node.get("bounds") or {}
        if "center_x" in node:
            parts.append(f"@{node['center_x']},{node['center_y']}")
        elif bounds:
            parts.append(f"@{bounds.get('x')},{bounds.get('y')} {bounds.get('width')}x{bounds.get('height')}")
        lines.append(" ".join(p for p in parts if p))
    return "\n".join(lines)


def bmp_to_png(data: bytes) -> bytes:
    """MCP image content must be PNG or JPEG, and ghost captures BMP because that is
    what the OS gives us cheaply. Converting here keeps the C++ side simple and
    avoids a dependency on an image library in either place."""
    if data[:2] != b"BM":
        raise ValueError("not a BMP")
    pixel_offset = struct.unpack_from("<I", data, 10)[0]
    header_size = struct.unpack_from("<I", data, 14)[0]
    if header_size < 40:
        raise ValueError(f"unsupported BMP header size {header_size}")
    width = struct.unpack_from("<i", data, 18)[0]
    height = struct.unpack_from("<i", data, 22)[0]
    planes, bit_count = struct.unpack_from("<HH", data, 26)
    compression = struct.unpack_from("<I", data, 30)[0]
    if compression != 0:
        raise ValueError(f"compressed BMP (type {compression}) is not supported")
    if bit_count not in (24, 32):
        raise ValueError(f"{bit_count}-bit BMP is not supported")

    top_down = height < 0
    height = abs(height)
    if width <= 0 or height <= 0:
        raise ValueError("BMP has no pixels")

    stride = ((width * bit_count + 31) // 32) * 4
    need = pixel_offset + stride * height
    if len(data) < need:
        raise ValueError("BMP is truncated")

    # BMP stores BGRA; PNG wants RGB, and rows are bottom-up unless height < 0.
    raw = bytearray()
    for row in range(height):
        source = height - 1 - row if not top_down else row
        base = pixel_offset + source * stride
        raw.append(0)  # filter type 0 (None) for every scanline
        line = data[base:base + width * (bit_count // 8)]
        if bit_count == 32:
            for i in range(0, len(line), 4):
                raw += bytes((line[i + 2], line[i + 1], line[i]))
        else:
            for i in range(0, len(line), 3):
                raw += bytes((line[i + 2], line[i + 1], line[i]))

    def chunk(tag: bytes, payload: bytes) -> bytes:
        return (struct.pack(">I", len(payload)) + tag + payload
                + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 6))
            + chunk(b"IEND", b""))


# --------------------------------------------------------------------------
# tools
# --------------------------------------------------------------------------

TOOLS = [
    {
        "name": "ghost_open",
        "description": (
            "Start the fingerprint browser (or reuse the running one) and optionally "
            "open a URL. Every other ghost_* tool starts it automatically, so you only "
            "need this to open a page first."),
        "inputSchema": {
            "type": "object",
            "properties": {
                "url": {"type": "string", "description": "URL to open"},
            },
            "additionalProperties": False,
        },
    },
    {
        "name": "ghost_status",
        "description": "Report the browser process id, profile, data directory, window title and size.",
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
    },
    {
        "name": "ghost_navigate",
        "description": (
            "Go to a URL. This drives the real address bar with synthesized keystrokes, "
            "so it is indistinguishable from a person typing. Waits for the page title "
            "to change (up to 20 s)."),
        "inputSchema": {
            "type": "object",
            "properties": {"url": {"type": "string", "description": "Absolute URL"}},
            "required": ["url"],
            "additionalProperties": False,
        },
    },
    {
        "name": "ghost_page",
        "description": (
            "Read the page's accessibility tree as text. This is the main way to see "
            "what is on the page: each line is `[index] role \"name\" value=... @x,y`. "
            "The index can be passed to ghost_click."),
        "inputSchema": {
            "type": "object",
            "properties": {
                "max_depth": {"type": "integer", "default": 12},
                "max_nodes": {"type": "integer", "default": 400},
                "anonymous": {"type": "boolean", "default": False,
                              "description": "include elements with no name"},
            },
            "additionalProperties": False,
        },
    },
    {
        "name": "ghost_find",
        "description": (
            "Search the page for elements by role and/or name (name is a "
            "case-insensitive substring). Returns matching lines in the same format "
            "as ghost_page, with indexes usable by ghost_click."),
        "inputSchema": {
            "type": "object",
            "properties": {
                "role": {"type": "string", "description": "e.g. button, edit, link, text"},
                "name": {"type": "string", "description": "substring of the accessible name"},
                "max_nodes": {"type": "integer", "default": 200},
            },
            "additionalProperties": False,
        },
    },
    {
        "name": "ghost_wait_for",
        "description": (
            "Poll the page until an element matching role/name appears, or until the "
            "timeout expires. Use this after a navigation or a click that loads content."),
        "inputSchema": {
            "type": "object",
            "properties": {
                "role": {"type": "string"},
                "name": {"type": "string"},
                "timeout": {"type": "number", "default": 15.0},
            },
            "additionalProperties": False,
        },
    },
    {
        "name": "ghost_click",
        "description": (
            "Click an element or a point with a real synthesized mouse event. Give "
            "either index (from ghost_page/ghost_find), or role+name, or css_x+css_y, "
            "or x+y (screen pixels)."),
        "inputSchema": {
            "type": "object",
            "properties": {
                "index": {"type": "integer", "description": "index from the last page read"},
                "role": {"type": "string"},
                "name": {"type": "string"},
                "css_x": {"type": "number", "description": "CSS pixel x within the viewport"},
                "css_y": {"type": "number"},
                "x": {"type": "number", "description": "absolute screen pixel x"},
                "y": {"type": "number"},
                "button": {"type": "string", "enum": ["left", "right", "middle"], "default": "left"},
                "count": {"type": "integer", "default": 1},
            },
            "additionalProperties": False,
        },
    },
    {
        "name": "ghost_type",
        "description": (
            "Type text with synthesized keystrokes, character by character. Focus the "
            "field first with ghost_click."),
        "inputSchema": {
            "type": "object",
            "properties": {"text": {"type": "string"}},
            "required": ["text"],
            "additionalProperties": False,
        },
    },
    {
        "name": "ghost_key",
        "description": 'Press a key or a combination, e.g. key="enter" or keys=["ctrl","l"].',
        "inputSchema": {
            "type": "object",
            "properties": {
                "key": {"type": "string"},
                "keys": {"type": "array", "items": {"type": "string"}},
            },
            "additionalProperties": False,
        },
    },
    {
        "name": "ghost_scroll",
        "description": "Scroll the page. Positive delta scrolls down, negative scrolls up.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "delta": {"type": "integer", "default": -360},
                "css_x": {"type": "number"},
                "css_y": {"type": "number"},
            },
            "additionalProperties": False,
        },
    },
    {
        "name": "ghost_captcha",
        "description": (
            "Handle a human-verification challenge (hCaptcha, reCAPTCHA, Cloudflare "
            "Turnstile). With action='wait' it touches nothing and just watches: it "
            "waits for a challenge to appear and then for it to pass on its own, "
            "reporting appeared/appeared_ms/cleared. Use it when a challenge may be "
            "transient or may only be verifying, and use it before concluding there "
            "is no challenge at all — the widget animates in, so a page about to "
            "challenge you looks like a page with none for about a second. With "
            "action='detect' it only reports what is there, including the sitekey. "
            "With action='solve' it clicks the checkbox and "
            "waits: Cloudflare Turnstile is usually answered outright, while "
            "hCaptcha and reCAPTCHA escalate to an image or audio challenge that a "
            "person still has to solve. With action='solve-audio' it opens the audio "
            "challenge, records what the machine actually plays through the OS audio "
            "stack, transcribes the digits and types them back. It reports "
            "solved_by='local' or 'api': the local Windows recogniser is used first, "
            "and a solving service only when the machine has no recogniser for the "
            "challenge's language and GHOST_CAPTCHA_KEY (or the profile's "
            "captcha_api_key) is set. Reads 'peak' and 'play' to tell a silent page "
            "from a recording that failed, and never retries in a loop."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "action": {"type": "string",
                           "enum": ["detect", "solve", "solve-audio", "wait"],
                           "default": "solve"},
                "timeout": {"type": "integer", "default": 30000,
                            "description": "milliseconds to keep watching"},
                "wait_ms": {"type": "integer", "default": 3000,
                            "description": "how long the first look may keep looking "
                                           "for a challenge to appear; 0 means a "
                                           "single sample"},
                "seconds": {"type": "integer", "default": 10,
                            "description": "seconds of audio to record for solve-audio"},
                "language": {"type": "string",
                             "description": "speech recogniser culture, e.g. en-US"},
                "keep": {"type": "boolean", "default": False,
                         "description": "keep the solve-audio recording and return its "
                                        "path as 'wav', instead of deleting it"},
            },
            "additionalProperties": False,
        },
    },
    {
        "name": "ghost_screenshot",
        "description": "Capture the browser window and return it as a PNG image.",
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
    },
    {
        "name": "ghost_close",
        "description": "Close the browser and stop the ghost server.",
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
    },
]


def tool_result(text: str, is_error: bool = False) -> dict:
    return {"content": [{"type": "text", "text": text}], "isError": is_error}


def run_tool(session: Session, name: str, args: dict) -> dict:
    if name == "ghost_open":
        url = args.get("url")
        if url and not session._alive():
            # Starting *at* the URL costs no synthesized input, which is what makes
            # this work in a session with no foreground window. Navigating afterwards
            # drives the address bar and does need one, so it is only the fallback.
            session.url = url
            session.ensure()
            title = ""
            deadline = time.time() + 20.0
            first = None
            while time.time() < deadline:
                status = session.call("status")
                title = (status.get("window") or {}).get("title") or ""
                if first is None and title:
                    first = title
                if title and first is not None and title != first:
                    break
                time.sleep(0.5)
            return tool_result(f"opened {url}\ntitle: {title}")

        session.ensure()
        if url:
            response = session.call("navigate", url=url)
            return tool_result(f"opened {url}\ntitle: {response.get('title')}")
        status = session.call("status")
        window = status.get("window") or {}
        return tool_result(f"running, pid {status.get('pid')}, "
                           f"window: {window.get('title')!r}")

    if name == "ghost_status":
        status = session.call("status")
        window = status.get("window") or {}
        lines = [
            f"pid:        {status.get('pid')}",
            f"profile:    {status.get('profile')}",
            f"data dir:   {status.get('data_dir')}",
            f"pipe:       {status.get('pipe')}",
            f"title:      {window.get('title')}",
            f"window:     {window.get('width')}x{window.get('height')} "
            f"scale {window.get('scale')}",
            f"focused:    {window.get('focused')}",
        ]
        if status.get("window_error"):
            lines.append(f"window error: {status['window_error']}")
        return tool_result("\n".join(lines))

    if name == "ghost_navigate":
        response = session.call("navigate", url=args["url"])
        note = "" if response.get("navigated") else " (the title did not change)"
        return tool_result(f"title: {response.get('title')}{note}")

    if name == "ghost_page":
        response = session.call("tree",
                                max_depth=int(args.get("max_depth", 12)),
                                max_nodes=int(args.get("max_nodes", 400)),
                                anonymous=bool(args.get("anonymous", False)))
        return tool_result(f"{response.get('count')} elements\n"
                           + render_nodes(response.get("nodes") or []))

    if name == "ghost_find":
        params = {"max_nodes": int(args.get("max_nodes", 200))}
        if args.get("role"):
            params["role"] = args["role"]
        if args.get("name"):
            params["name"] = args["name"]
        if len(params) == 1:
            return tool_result("give a role, a name, or both", is_error=True)
        response = session.call("find", **params)
        return tool_result(f"{response.get('count')} matches\n"
                           + render_nodes(response.get("nodes") or []))

    if name == "ghost_wait_for":
        role = args.get("role") or ""
        needle = args.get("name") or ""
        if not role and not needle:
            return tool_result("give a role, a name, or both", is_error=True)
        timeout = float(args.get("timeout", 15.0))
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            params = {"max_nodes": 200}
            if role:
                params["role"] = role
            if needle:
                params["name"] = needle
            last = session.call("find", **params)
            if last.get("count"):
                return tool_result(f"appeared after {timeout - (deadline - time.time()):.1f}s\n"
                                   + render_nodes(last.get("nodes") or []))
            time.sleep(0.5)
        return tool_result(f"nothing matched role={role!r} name={needle!r} "
                           f"within {timeout:.0f}s", is_error=True)

    if name == "ghost_click":
        params = {}
        for key in ("index", "role", "name", "css_x", "css_y", "x", "y", "button", "count"):
            if args.get(key) is not None:
                params[key] = args[key]
        if not params:
            return tool_result("give index, role+name, css_x+css_y, or x+y", is_error=True)
        response = session.call("click", **params)
        return tool_result(f"clicked at {response.get('x')},{response.get('y')}")

    if name == "ghost_type":
        response = session.call("type", text=args["text"])
        return tool_result(f"typed {response.get('typed')} characters")

    if name == "ghost_key":
        if args.get("keys"):
            session.call("key", keys=list(args["keys"]))
            return tool_result("pressed " + "+".join(args["keys"]))
        if args.get("key"):
            session.call("key", key=args["key"])
            return tool_result(f"pressed {args['key']}")
        return tool_result("give key or keys", is_error=True)

    if name == "ghost_scroll":
        params = {"delta": int(args.get("delta", -360))}
        if args.get("css_x") is not None and args.get("css_y") is not None:
            params["css_x"] = args["css_x"]
            params["css_y"] = args["css_y"]
        response = session.call("scroll", **params)
        return tool_result(f"scrolled by {response.get('delta')}")

    if name == "ghost_captcha":
        action = args.get("action") or "solve"
        params = {"action": action, "timeout": int(args.get("timeout", 30000)),
                  "wait_ms": int(args.get("wait_ms", 3000))}
        if action == "solve-audio":
            params["seconds"] = int(args.get("seconds", 10))
            if args.get("language"):
                params["language"] = args["language"]
            if args.get("keep"):
                params["keep"] = True
        response = session.call("captcha", **params)
        provider = response.get("provider", "none")
        state = response.get("state", "?")
        if provider == "none":
            if action == "wait":
                return tool_result(
                    "no human-verification challenge appeared within "
                    f"{response.get('waited_ms', 0)} ms of watching")
            return tool_result("no human-verification challenge on this page")

        lines = [f"provider: {provider}", f"state:    {state}"]
        if response.get("site_key"):
            lines.append(f"sitekey:  {response['site_key']}")
        if response.get("page_url"):
            lines.append(f"page:     {response['page_url']}")
        if response.get("detail"):
            lines.append(f"detail:   {response['detail']}")
        if action == "wait":
            lines.append(f"appeared: {bool(response.get('appeared'))}")
            if response.get("appeared_ms") is not None:
                lines.append(f"in:       {response['appeared_ms']} ms")
            lines.append(f"cleared:  {bool(response.get('cleared'))}")
            lines.append(f"waited:   {response.get('waited_ms')} ms")
        if action == "solve":
            lines.append(f"clicked:  {bool(response.get('clicked'))}")
        if action == "solve-audio":
            # `play` and `peak` together are what separate "the challenge never
            # played anything" from "nothing ever opened a stream" -- and the audio
            # challenge does not start itself, so an unpressed play control is the
            # first thing worth knowing when the answer comes back empty.
            if response.get("play"):
                lines.append(f"play:     {response['play']}")
            if response.get("device"):
                lines.append(f"device:   {response['device']}")
            if response.get("captured_seconds") is not None:
                lines.append(f"captured: {response['captured_seconds']}s")
            if response.get("peak") is not None:
                lines.append(f"peak:     {response['peak']}")
            if response.get("streams"):
                lines.append(f"streams:  {response['streams']}")
            if response.get("solved_by"):
                lines.append(f"by:       {response['solved_by']}")
            if response.get("heard"):
                lines.append(f"heard:    {response['heard']}")
            if response.get("confidence") is not None:
                lines.append(f"conf:     {response['confidence']}")
            if response.get("typed"):
                lines.append(f"typed:    {response['typed']}")
            if response.get("input"):
                lines.append(f"input:    {response['input']}")
            if response.get("wav"):
                lines.append(f"wav:      {response['wav']}")

        if action == "wait":
            if not response.get("appeared"):
                lines.append("nothing challenged this page while we watched")
            elif response.get("cleared"):
                lines.append("the challenge appeared and then passed on its own")
            else:
                lines.append("the challenge appeared and is still there -- it did not "
                             "pass by itself, so it needs action='solve'")
        elif state == "solved":
            lines.append("the challenge is answered")
        elif state == "visual":
            lines.append("an image challenge is open; a person still has to solve it")
        elif state == "audio":
            if action == "solve-audio":
                if response.get("heard"):
                    lines.append("the audio challenge was recorded and transcribed")
                elif response.get("streams") == "nothing held a stream":
                    lines.append("an audio challenge is open, but the page never played "
                                 "anything -- retrying will not help")
                else:
                    lines.append("an audio challenge is open, but nothing usable was "
                                 "heard; check 'play' and 'peak' before retrying")
            else:
                lines.append("an audio challenge is open — use action='solve-audio' to "
                             "record it and answer it automatically")
        return tool_result("\n".join(lines))

    if name == "ghost_screenshot":
        # Write to a temp path rather than the default cache location, so repeated
        # calls cannot collide with a file another tool is reading.
        target = Path(tempfile.gettempdir()) / f"ghost-mcp-{session.id}.bmp"
        response = session.call("screenshot", path=str(target))
        data = target.read_bytes()
        try:
            png = bmp_to_png(data)
        except ValueError as exc:
            return tool_result(f"captured {response.get('path')} "
                               f"({response.get('width')}x{response.get('height')}) "
                               f"but could not convert it: {exc}")
        return {
            "content": [
                {"type": "image", "data": base64.b64encode(png).decode("ascii"),
                 "mimeType": "image/png"},
                {"type": "text",
                 "text": f"{response.get('width')}x{response.get('height')} PNG"},
            ],
            "isError": False,
        }

    if name == "ghost_close":
        session.stop()
        return tool_result("closed")

    return tool_result(f"unknown tool {name}", is_error=True)


# --------------------------------------------------------------------------
# JSON-RPC over stdio
# --------------------------------------------------------------------------

def send(message: dict) -> None:
    sys.stdout.write(json.dumps(message, ensure_ascii=False) + "\n")
    sys.stdout.flush()


def reply(request_id, result: dict) -> None:
    send({"jsonrpc": "2.0", "id": request_id, "result": result})


def reply_error(request_id, code: int, message: str) -> None:
    send({"jsonrpc": "2.0", "id": request_id, "error": {"code": code, "message": message}})


def main() -> int:
    exe = find_ghost_exe()
    if exe is None:
        log("could not find ghost.exe; set GHOST_EXE to its path")

    profile_id = DEFAULT_ID
    url = os.environ.get("GHOST_URL") or None
    session = Session(exe or "ghost.exe", profile_id, url)
    if exe is None:
        log("tools will fail until GHOST_EXE points at a real ghost.exe")

    log(f"ghost.exe = {exe}")
    log(f"profile    = {profile_id}")

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            message = json.loads(line)
        except ValueError as exc:
            log(f"ignoring malformed input: {exc}")
            continue

        method = message.get("method")
        request_id = message.get("id")
        is_notification = request_id is None

        if method == "initialize":
            requested = (message.get("params") or {}).get("protocolVersion")
            version = requested if requested in SUPPORTED_PROTOCOL_VERSIONS else PROTOCOL_VERSION
            reply(request_id, {
                "protocolVersion": version,
                "capabilities": {"tools": {"listChanged": False}},
                "serverInfo": {"name": SERVER_NAME, "version": SERVER_VERSION},
                "instructions": (
                    "Drives a fingerprint browser that is built to be indistinguishable "
                    "from a person using it. Read a page with ghost_page, then act on the "
                    "indexes it prints with ghost_click. Navigation and input are real "
                    "synthesized OS events, so they are slower than a scripted browser "
                    "and they need a connected, unlocked desktop session."),
            })
        elif method in ("notifications/initialized", "initialized"):
            pass
        elif method == "ping":
            reply(request_id, {})
        elif method == "tools/list":
            reply(request_id, {"tools": TOOLS})
        elif method == "tools/call":
            params = message.get("params") or {}
            name = params.get("name") or ""
            args = params.get("arguments") or {}
            try:
                reply(request_id, run_tool(session, name, args))
            except PipeError as exc:
                reply(request_id, tool_result(str(exc), is_error=True))
            except KeyError as exc:
                reply(request_id, tool_result(f"missing required argument {exc}", is_error=True))
            except Exception as exc:  # never kill the session over one bad call
                log(f"tool {name} raised {type(exc).__name__}: {exc}")
                reply(request_id, tool_result(f"{type(exc).__name__}: {exc}", is_error=True))
        elif method in ("resources/list", "prompts/list"):
            key = method.split("/")[0]
            reply(request_id, {key: []})
        elif is_notification:
            pass
        else:
            reply_error(request_id, -32601, f"method not found: {method}")

    session.stop()
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
