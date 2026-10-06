"""A client for the ghost control plane.

Nothing here uses a debugging protocol. Requests travel over a Windows named
pipe, and the browser is driven the way a person would drive it: synthesized
OS input for actions, the accessibility tree for reading, the OS window for
screenshots, and the profile's own SQLite file for cookies. A page has no
JavaScript-visible surface to detect any of it.

    from ghost_client import Ghost

    with Ghost("demo").start(url="https://example.com") as browser:
        browser.wait_for(name_contains="Sign in")
        browser.click(role="button", name="Sign in")
        browser.type("hello", into={"role": "edit", "name": "Search"})
        browser.key("Enter")
        print(browser.screenshot())
"""
from __future__ import annotations

import base64
import ctypes
import ctypes.wintypes
import json
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_GHOST = ROOT / "native" / "build" / "bin" / "ghost.exe"


class GhostError(RuntimeError):
    """The control plane replied, and the reply was an error."""


# --- reading cookie values ----------------------------------------------------
#
# Chrome never stores a cookie value in plaintext. The `value` column is always
# empty; the real bytes live in `encrypted_value`, which on Windows is
# `b"v10"` followed by AES-256-GCM(nonce || ciphertext || tag). The AES key is
# random per profile and kept in `Local State` as `os_crypt.encrypted_key`, a
# DPAPI-wrapped blob. Both steps are reversible for the same Windows user, and
# neither needs the browser to be running -- which is what makes harvesting a
# `cf_clearance` token possible at all.
#
# Everything here goes through Windows' own crypto (crypt32 for DPAPI, bcrypt for
# AES-GCM) so the client keeps its no-third-party-dependency property.


class _DataBlob(ctypes.Structure):
    _fields_ = [("cbData", ctypes.wintypes.DWORD),
                ("pbData", ctypes.POINTER(ctypes.c_char))]


def _dpapi_unprotect(blob: bytes) -> bytes | None:
    """Reverse CryptProtectData for the current user."""
    crypt32 = ctypes.WinDLL("crypt32", use_last_error=True)
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    crypt32.CryptUnprotectData.argtypes = [
        ctypes.POINTER(_DataBlob), ctypes.POINTER(ctypes.wintypes.LPWSTR),
        ctypes.POINTER(_DataBlob), ctypes.c_void_p, ctypes.c_void_p,
        ctypes.wintypes.DWORD, ctypes.POINTER(_DataBlob)]
    crypt32.CryptUnprotectData.restype = ctypes.wintypes.BOOL

    buf = ctypes.create_string_buffer(blob, len(blob))
    blob_in = _DataBlob(len(blob), ctypes.cast(buf, ctypes.POINTER(ctypes.c_char)))
    blob_out = _DataBlob()
    if not crypt32.CryptUnprotectData(ctypes.byref(blob_in), None, None, None, None, 0,
                                      ctypes.byref(blob_out)):
        return None
    plain = ctypes.string_at(blob_out.pbData, blob_out.cbData)
    kernel32.LocalFree(blob_out.pbData)
    return plain


class _AuthCipherModeInfo(ctypes.Structure):
    _fields_ = [
        ("cbSize", ctypes.wintypes.ULONG),
        ("dwInfoVersion", ctypes.wintypes.ULONG),
        ("pbNonce", ctypes.POINTER(ctypes.c_ubyte)),
        ("cbNonce", ctypes.wintypes.ULONG),
        ("pbAuthData", ctypes.POINTER(ctypes.c_ubyte)),
        ("cbAuthData", ctypes.wintypes.ULONG),
        ("pbTag", ctypes.POINTER(ctypes.c_ubyte)),
        ("cbTag", ctypes.wintypes.ULONG),
        ("pbMacContext", ctypes.POINTER(ctypes.c_ubyte)),
        ("cbMacContext", ctypes.wintypes.ULONG),
        ("cbAAD", ctypes.wintypes.ULONG),
        ("cbData", ctypes.c_ulonglong),
        ("dwFlags", ctypes.wintypes.ULONG),
    ]


def _aes_gcm_decrypt(key: bytes, payload: bytes) -> bytes | None:
    """Decrypt `nonce || ciphertext || tag` with AES-256-GCM through bcrypt.

    Every pointer argument is declared and every buffer is a real ctypes buffer.
    Leaving the prototypes implicit makes ctypes pass a Python bytes object where
    a `void*` is expected, and bcrypt answers with STATUS_ACCESS_VIOLATION.
    """
    if len(payload) < 12 + 16:
        return None
    nonce, body, tag = payload[:12], payload[12:-16], payload[-16:]

    bcrypt = ctypes.WinDLL("bcrypt", use_last_error=True)
    bcrypt.BCryptOpenAlgorithmProvider.argtypes = [
        ctypes.POINTER(ctypes.wintypes.HANDLE), ctypes.wintypes.LPCWSTR,
        ctypes.wintypes.LPCWSTR, ctypes.wintypes.ULONG]
    bcrypt.BCryptSetProperty.argtypes = [
        ctypes.wintypes.HANDLE, ctypes.wintypes.LPCWSTR, ctypes.c_void_p,
        ctypes.wintypes.ULONG, ctypes.wintypes.ULONG]
    bcrypt.BCryptGenerateSymmetricKey.argtypes = [
        ctypes.wintypes.HANDLE, ctypes.POINTER(ctypes.wintypes.HANDLE), ctypes.c_void_p,
        ctypes.wintypes.ULONG, ctypes.c_void_p, ctypes.wintypes.ULONG, ctypes.wintypes.ULONG]
    bcrypt.BCryptDecrypt.argtypes = [
        ctypes.wintypes.HANDLE, ctypes.c_void_p, ctypes.wintypes.ULONG, ctypes.c_void_p,
        ctypes.c_void_p, ctypes.wintypes.ULONG, ctypes.c_void_p, ctypes.wintypes.ULONG,
        ctypes.POINTER(ctypes.wintypes.ULONG), ctypes.wintypes.ULONG]

    handle = ctypes.wintypes.HANDLE()
    if bcrypt.BCryptOpenAlgorithmProvider(ctypes.byref(handle), "AES", None, 0) != 0:
        return None
    try:
        mode = "ChainingModeGCM".encode("utf-16-le")
        if bcrypt.BCryptSetProperty(handle, "ChainingMode", mode, len(mode), 0) != 0:
            return None

        key_buf = ctypes.create_string_buffer(key, len(key))
        key_handle = ctypes.wintypes.HANDLE()
        if bcrypt.BCryptGenerateSymmetricKey(handle, ctypes.byref(key_handle), None, 0,
                                             key_buf, len(key), 0) != 0:
            return None
        try:
            nonce_buf = (ctypes.c_ubyte * len(nonce)).from_buffer_copy(nonce)
            tag_buf = (ctypes.c_ubyte * len(tag)).from_buffer_copy(tag)
            info = _AuthCipherModeInfo()
            info.cbSize = ctypes.sizeof(info)
            info.dwInfoVersion = 1
            info.pbNonce = ctypes.cast(nonce_buf, ctypes.POINTER(ctypes.c_ubyte))
            info.cbNonce = len(nonce)
            info.pbTag = ctypes.cast(tag_buf, ctypes.POINTER(ctypes.c_ubyte))
            info.cbTag = len(tag)

            in_buf = ctypes.create_string_buffer(body, len(body))
            out_buf = ctypes.create_string_buffer(len(body))
            written = ctypes.wintypes.ULONG()
            # For an authenticated mode the cipher-mode struct goes in the `pbIV`
            # slot *and* pPaddingInfo must point at it too -- passing NULL for
            # pPaddingInfo faults with STATUS_ACCESS_VIOLATION instead of
            # failing. The trailing dwFlags is not optional either: omitting it
            # leaves the last argument unmapped. All three were found the hard
            # way, so they are written out rather than tucked into a helper.
            status = bcrypt.BCryptDecrypt(key_handle, in_buf, len(body),
                                          ctypes.byref(info), ctypes.byref(info),
                                          ctypes.sizeof(info), out_buf, len(body),
                                          ctypes.byref(written), 0)
            if status != 0:
                return None
            return out_buf.raw[:written.value]
        finally:
            bcrypt.BCryptDestroyKey(key_handle)
    finally:
        bcrypt.BCryptCloseAlgorithmProvider(handle, 0)


def _cookie_key(data_dir: Path) -> bytes | None:
    """The profile's AES key, unwrapped from `Local State`."""
    state = Path(data_dir) / "Local State"
    if not state.exists():
        return None
    try:
        key_b64 = json.loads(state.read_text(encoding="utf-8"))["os_crypt"]["encrypted_key"]
    except (OSError, KeyError, ValueError):
        return None
    blob = base64.b64decode(key_b64)
    if not blob.startswith(b"DPAPI"):
        return None
    return _dpapi_unprotect(blob[5:])


class Ghost:
    """One connection to a `ghost serve` process and the browser it launched."""

    # How long a request keeps retrying a *connection* that is refused. The real
    # window is milliseconds wide (the server serves one client at a time), so a
    # few seconds is generous; anything longer means the server is gone.
    CONNECT_RETRY_SECONDS = 5.0

    def __init__(self, profile: str = "default", ghost: Path | None = None,
                 pipe: str | None = None, timeout: float = 300.0):
        self.ghost = Path(ghost) if ghost else DEFAULT_GHOST
        self.profile = profile
        self.pipe = pipe or f"ghost-{profile}"
        self.timeout = timeout
        self._process: subprocess.Popen | None = None
        self._log = None

    # --- lifecycle -----------------------------------------------------------

    @property
    def pipe_path(self) -> str:
        return rf"\\.\pipe\{self.pipe}"

    def start(self, url: str | None = None, extra_args=(), wait: float = 90.0,
              log: Path | None = None) -> "Ghost":
        """Launch `ghost serve`, which starts the browser and opens the pipe."""
        args = [str(self.ghost), "serve", "--id", self.profile, "--pipe", self.pipe]
        args += list(extra_args)
        if url:
            args.append(url)
        if log is not None:
            self._log = open(log, "wb")
            stdout = self._log
        else:
            stdout = subprocess.DEVNULL
        self._process = subprocess.Popen(args, stdout=stdout, stderr=subprocess.STDOUT)
        self.wait_ready(wait)
        return self

    def wait_ready(self, timeout: float = 90.0) -> bool:
        """Block until the pipe answers, which means the browser is up too."""
        deadline = time.time() + timeout
        last: Exception | None = None
        while time.time() < deadline:
            try:
                self.status()
                return True
            except (OSError, GhostError, ValueError) as exc:
                last = exc
                if self._process is not None and self._process.poll() is not None:
                    raise GhostError(
                        f"ghost serve exited with {self._process.returncode} "
                        f"before the pipe opened: {last}")
                time.sleep(0.5)
        raise GhostError(f"the control pipe never came up: {last}")

    def stop(self, kill: bool = False) -> None:
        try:
            self.call("shutdown")
        except Exception:
            pass
        if self._process is not None:
            try:
                self._process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self._process.kill()
                self._process.wait(timeout=10)
        if self._log is not None:
            self._log.close()
            self._log = None

    def __enter__(self) -> "Ghost":
        return self

    def __exit__(self, *exc) -> None:
        self.stop()

    # --- transport -----------------------------------------------------------

    def call(self, cmd: str, **kwargs) -> dict:
        """Send one request and return the response object."""
        request = {"cmd": cmd, **kwargs}
        response = self._round_trip(request)
        if not response.get("ok"):
            raise GhostError(response.get("error", "unknown error"))
        return response

    def _round_trip(self, request: dict, timeout: float | None = None) -> dict:
        payload = (json.dumps(request, ensure_ascii=False) + "\n").encode("utf-8")
        # A named pipe is a file on Windows, and the server answers exactly one
        # line per request, so a plain open/readline is the whole protocol.
        #
        # The server serves one client at a time, so between a disconnect and the
        # next ConnectNamedPipe the name briefly resolves to nothing, and a
        # request that arrives while another is being served gets ERROR_PIPE_BUSY.
        # Both are normal, so connection errors are retried rather than surfaced.
        #
        # Only *connecting* is retried. A successful connect leaves the loop and
        # readline then blocks for as long as the command takes, so a slow command
        # is unaffected by the short window here -- which matters, because a dead
        # control plane must fail in seconds rather than hanging for the full
        # request timeout.
        deadline = time.time() + self.CONNECT_RETRY_SECONDS
        last: OSError | None = None
        while time.time() < deadline:
            try:
                with open(self.pipe_path, "r+b") as handle:
                    handle.write(payload)
                    handle.flush()
                    line = handle.readline()
            except OSError as exc:
                last = exc
                time.sleep(0.05)
                continue
            if not line:
                raise GhostError("the control plane closed the connection")
            return json.loads(line.decode("utf-8"))
        raise GhostError(f"could not reach the control plane on {self.pipe_path}: {last}")

    # --- commands ------------------------------------------------------------

    def status(self) -> dict:
        return self.call("status")

    def windows(self) -> list[dict]:
        return self.call("windows").get("windows", [])

    def focus(self) -> dict:
        return self.call("focus")

    def navigate(self, url: str, timeout: float = 30.0) -> dict:
        return self.call("navigate", url=url, timeout=timeout)

    def tree(self, max_nodes: int = 400, max_depth: int = 20,
             keep_anonymous: bool = False) -> list[dict]:
        return self.call("tree", max_nodes=max_nodes, max_depth=max_depth,
                         keep_anonymous=keep_anonymous).get("nodes", [])

    def find(self, role: str = "", name: str = "", max_nodes: int = 400,
             max_depth: int = 20) -> list[dict]:
        return self.call("find", role=role, name=name, max_nodes=max_nodes,
                         max_depth=max_depth).get("nodes", [])

    def first(self, role: str = "", name: str = "") -> dict | None:
        found = self.find(role=role, name=name)
        return found[0] if found else None

    def click(self, role: str = "", name: str = "", index: int | None = None,
              x: int | None = None, y: int | None = None, button: str = "left",
              count: int = 1) -> dict:
        request: dict = {"button": button, "count": count}
        if index is not None:
            request["index"] = index
        elif x is not None and y is not None:
            request["x"], request["y"] = x, y
        elif role or name:
            target = self.first(role=role, name=name)
            if target is None:
                raise GhostError(f"no element matches role={role!r} name={name!r}")
            request["index"] = target["index"]
        else:
            raise GhostError("click needs an element, coordinates, or a role/name")
        return self.call("click", **request)

    def type(self, text: str, into: dict | None = None) -> dict:
        """Type text. With `into`, click that element first."""
        if into is not None:
            self.click(role=into.get("role", ""), name=into.get("name", ""),
                       index=into.get("index"))
        return self.call("type", text=text)

    def key(self, *keys: str) -> dict:
        return self.call("key", keys=list(keys))

    def scroll(self, delta: int, x: int | None = None, y: int | None = None) -> dict:
        request: dict = {"delta": delta}
        if x is not None and y is not None:
            request["x"], request["y"] = x, y
        return self.call("scroll", **request)

    def screenshot(self, path: str | None = None) -> str:
        request = {"path": path} if path else {}
        return self.call("screenshot", **request)["path"]

    # --- convenience ---------------------------------------------------------

    def wait_for(self, role: str = "", name: str = "", contains: str = "",
                 timeout: float = 30.0, poll: float = 0.4) -> dict:
        """Poll the accessibility tree until an element appears."""
        needle = (contains or name).lower()
        deadline = time.time() + timeout
        while time.time() < deadline:
            # `find` needs at least one filter, so a bare substring search has to
            # walk the whole tree instead.
            nodes = self.tree() if (contains and not name) else self.find(role=role, name=name)
            for node in nodes:
                if role and node.get("role") != role:
                    continue
                if needle and needle not in (node.get("name") or "").lower():
                    continue
                return node
            time.sleep(poll)
        raise GhostError(f"timed out waiting for role={role!r} containing {needle!r}")

    def label_value(self, label: str, timeout: float = 15.0) -> str | None:
        """Read `label=value` out of any element whose accessible name carries it.

        Pages under test can publish state by setting an aria-label, which UIA
        then reports as the element's name. It is a crude channel, but it needs
        no debugging protocol and the page cannot tell it is being watched.
        """
        deadline = time.time() + timeout
        while time.time() < deadline:
            for node in self.find(name=label + "="):
                name = node.get("name", "")
                if name.startswith(label + "="):
                    return name.split("=", 1)[1]
            time.sleep(0.3)
        return None

    # --- cookies, straight out of the profile ---------------------------------

    def data_dir(self) -> Path:
        """Where this profile's browser keeps its files.

        Asking the server is one pipe round-trip, so callers that intend to stop
        the browser first should capture this while it is still running.
        """
        return Path(self.status()["data_dir"])

    def cookie_db(self, data_dir: Path | None = None) -> Path | None:
        """Locate the profile's cookie database.

        Pass `data_dir` to avoid a round-trip to the control plane -- necessary
        once the browser has been stopped, since the pipe is gone by then.
        """
        root = Path(data_dir) if data_dir is not None else self.data_dir()
        candidates = [
            root / "Default" / "Network" / "Cookies",
            root / "Default" / "Cookies",
            root / "Cookies",
        ]
        for candidate in candidates:
            if candidate.exists():
                return candidate
        return None

    def cookies(self, host: str | None = None, db: Path | None = None) -> list[dict]:
        """Read cookies from the profile's SQLite file.

        The browser holds the database open with no sharing at all while it runs,
        so this only works once the browser has exited. SQLite's own locking makes
        the copy safe. This is also the channel a `cf_clearance` token would be
        collected from, which is why it matters that it works at all.
        """
        path = Path(db) if db is not None else self.cookie_db()
        if path is None:
            raise GhostError("no cookie database in the profile yet")

        # The AES key sits in `Local State` at the profile root, a few levels up.
        key = None
        for candidate in path.parents:
            if (candidate / "Local State").exists():
                key = _cookie_key(candidate)
                break

        with tempfile.TemporaryDirectory() as tmp:
            copy = Path(tmp) / "Cookies"
            shutil.copy2(path, copy)
            connection = sqlite3.connect(f"file:{copy}?mode=ro", uri=True)
            try:
                query = ("SELECT host_key, name, value, path, is_secure, is_httponly, "
                         "encrypted_value FROM cookies")
                params: tuple = ()
                if host:
                    query += " WHERE host_key LIKE ?"
                    params = (f"%{host}%",)
                rows = connection.execute(query, params).fetchall()
            finally:
                connection.close()

        out = []
        for host_key, name, value, cookie_path, secure, http_only, encrypted in rows:
            if not value and encrypted and key is not None:
                # b"v10" is a version marker, not part of the ciphertext.
                payload = encrypted[3:] if encrypted[:3] == b"v10" else encrypted
                decrypted = _aes_gcm_decrypt(key, payload)
                if decrypted is not None:
                    # Chrome prepends a 32-byte binding to the domain, so the
                    # plaintext is that prefix followed by the value itself.
                    value = decrypted[32:].decode("utf-8", "replace")
            out.append({"host": host_key, "name": name, "value": value, "path": cookie_path,
                        "secure": bool(secure), "http_only": bool(http_only)})
        return out


def main() -> int:
    """A one-liner interface, so the plane can be poked from a shell."""
    if len(sys.argv) < 3:
        print("usage: ghost_client.py <profile> <command> [json-args]")
        print("       ghost_client.py demo status")
        print('       ghost_client.py demo navigate \'{"url":"https://example.com"}\'')
        return 64
    profile, command = sys.argv[1], sys.argv[2]
    kwargs = json.loads(sys.argv[3]) if len(sys.argv) > 3 else {}
    ghost = Ghost(profile)
    print(json.dumps(ghost.call(command, **kwargs), indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
