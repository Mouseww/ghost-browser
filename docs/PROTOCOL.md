# The control protocol

`ghost serve` runs the browser and listens on a Windows named pipe. This document
is the whole interface: if you can open a named pipe and write a line of JSON, you
can drive the browser from any language with no library and no dependency on us.

## Why a pipe and not CDP

Chrome DevTools Protocol is the obvious choice and it is the wrong one for this
product. `--remote-debugging-port` opens a TCP listener that any page can find by
scanning ports, `Runtime.enable` has side effects a page can observe, and the
protocol's own existence is one of the strongest automation signals there is.

A named pipe is not reachable from a page. There is no port to scan, no socket to
probe, no injected script, and nothing for `navigator.webdriver` or any other
detector to notice. Input goes in through `SendInput` and the page is read through
the UI Automation tree, so from inside the renderer everything that happens looks
like a person using a keyboard and mouse.

The cost is real and worth stating: this is not a fast protocol for scripted bulk
work. Each command is a round trip through the OS input stack. It is designed to
be indistinguishable, not to be quick.

## Transport

A byte-mode duplex named pipe. The name depends on how the server was started:

| Started with | Listens on |
|---|---|
| `ghost serve --id demo` | `\\.\pipe\ghost-demo` |
| `ghost serve --id demo --pipe custom` | `\\.\pipe\custom` |

**`--pipe` is the literal pipe name, not a suffix.** It is used exactly as given, and
the `ghost-<id>` convention applies only when `--pipe` is omitted. A client that
assumes the prefix will fail to connect to any server started with an explicit
`--pipe` — which is the trap worth reading twice, because passing the same string to
both flags looks like it should agree with the convention and does not.
`--id` defaults to `default`, so a bare `ghost serve` listens on
`\\.\pipe\ghost-default`.

The default name is deterministic, so a client that passes only `--id` has nothing to
discover — it computes the name from the same id it gave the server.

## Framing

One JSON object per line, UTF-8, `\n`-terminated, in both directions.

- A request is one object followed by `\n`.
- A response is one object followed by `\n`.
- A trailing `\r` on a request is ignored, so a client may write `\r\n`.
- Blank lines are skipped.
- **One connection may carry any number of sequential requests.** The server reads
  a line, answers it, and reads the next one until the client disconnects. There is
  no request id and no pipelining: request *n*, then response *n*, then request
  *n+1*. A client that writes two requests before reading a response will still
  work, but only because the server answers them in order.

## Envelope

Request:

```json
{"cmd": "navigate", "url": "https://example.com"}
```

`cmd` is required. A request without it gets `{"ok": false, "error": "every request needs a \"cmd\""}`.

Success:

```json
{"ok": true, ...command-specific fields}
```

Failure:

```json
{"ok": false, "error": "human-readable reason"}
```

**Check `ok`, not the presence of a field.** A failed command returns `ok: false`
and an `error` string; it never throws, never closes the pipe, and never leaves the
connection in a state where the next request is misaligned.

## Commands

| `cmd` | Purpose |
|---|---|
| `status` | What is attached, where the window is |
| `windows` | Every top-level window the browser process owns |
| `focus` | Bring the browser window to the foreground |
| `navigate` | Go to a URL, as if typed into the address bar |
| `tree` | Dump the accessibility tree |
| `find` | Search the accessibility tree by role and/or name |
| `click` | Click an element or a point |
| `type` | Type text into the focused element |
| `key` | Press a key combination |
| `scroll` | Scroll at a point |
| `screenshot` | Capture the window to a BMP file |
| `shutdown` | Stop the server (and the browser it launched) |

### `status`

No parameters. Returns:

```json
{
  "ok": true,
  "pid": 13284,
  "pipe": "ghost-default",
  "profile": "default",
  "data_dir": "C:\\Users\\you\\AppData\\Local\\GhostBrowser\\profiles\\default.data",
  "attached": true,
  "window": {
    "title": "Example Domain",
    "class": "Chrome_WidgetWin_1",
    "width": 1280, "height": 720,
    "scale": 1.0,
    "focused": true
  },
  "tree_nodes": 0
}
```

`window` is `null` and a `window_error` string is present when there is no window
yet. `tree_nodes` counts the elements cached by the last `tree` or `find`, which is
what `index` refers to.

`pipe` is the bare pipe name, not the full `\\.\pipe\...` path. `pid` is the **server**
process, not the browser and not the `ghost.exe` you launched — the launcher re-spawns,
so those are three different numbers and only this one is useful for correlating with
the server's own log line.

`status` is the one command that succeeds before the browser is up, so it is the
right way to poll for readiness. It waits up to 30 seconds for a window to appear
before reporting `window_error`.

### `windows`

Every top-level window owned by the browser process:

```json
{"ok": true, "windows": [
  {"handle": 461234, "title": "Example Domain", "class": "Chrome_WidgetWin_1",
   "visible": true, "minimized": false, "width": 1280, "height": 720}
]}
```

### `focus`

No parameters. Brings the window forward. Fails with a specific message on a
disconnected or headless session, because Windows silently discards synthesized
input there — see *Sessions* below.

### `navigate`

| field | required | meaning |
|---|---|---|
| `url` | yes | absolute URL |

There is no navigation protocol available without CDP, so this does exactly what a
person does: focus the window, press Ctrl+L, type the URL, press Enter. Completion
is detected by the window title changing, with a 20 second ceiling. Returns:

```json
{"ok": true, "navigated": true, "title": "Example Domain"}
```

`navigated` is `false` when the title never changed within 20 seconds. That is not
necessarily a failure — a page whose title matches the previous one, or a URL that
redirects to the same document, produces `navigated: false` too. Check `title`, or
`find` for something you expect on the page.

### `tree`

| field | default | meaning |
|---|---|---|
| `max_depth` | 12 | how deep to walk |
| `max_nodes` | 600 | stop after this many elements |
| `anonymous` | false | include elements with no name and no useful role |

Returns `{"ok": true, "nodes": [...], "count": n}`. Each node:

```json
{
  "index": 14, "depth": 3,
  "role": "button", "name": "Submit",
  "value": "optional, only when non-empty",
  "id": "optional automation id, only when non-empty",
  "enabled": true, "offscreen": false, "focused": false,
  "bounds": {"x": 100, "y": 240, "width": 88, "height": 32},
  "center_x": 144, "center_y": 256
}
```

`bounds` and `center_*` are in **absolute screen pixels**. `center_*` is omitted
when the element has no visible area. `index` is only valid until the next `tree`
or `find`, which replace the cached list.

The page itself is the node with `role: "document"` and `id: "RootWebArea"`, and it
is worth checking for by name, because it is the one thing browser chrome can never
imitate — `id` and `value` are the fields to read, not just `name`.

`ghost serve` primes the accessibility tree as soon as the window appears, so a
client's first query already sees the document. That priming exists because
Chromium builds the tree lazily: the first query is what switches accessibility on,
and the document only appears in a later one. A client talking to a browser that has
*not* been primed would see a tree of pure chrome on its first `tree` or `find`, and
the obvious reading of that — "the page is empty" — would be wrong.

### `find`

| field | default | meaning |
|---|---|---|
| `role` | `""` | exact role match |
| `name` | `""` | **substring**, case-insensitive |
| `max_depth` | 12 | how deep to walk |
| `max_nodes` | 2000 | stop after this many elements |

At least one of `role` or `name` is required. Returns the same node shape and the
same `nodes`/`count` envelope as `tree`, and like `tree` it replaces the cache that
`index` resolves against.

### `click`

Where to click is given in one of three ways:

| form | fields | meaning |
|---|---|---|
| absolute | `x`, `y` | screen pixels |
| page | `css_x`, `css_y` | CSS pixels within the page viewport |
| element | `index` | an index from the last `tree`/`find` |
| element | `role`, `name` | resolved the same way as `find`, first match |

Plus optional `button` (default `left`) and `count` (default 1).

The window is focused first. Returns `{"ok": true, "x": 144, "y": 256}` — the
actual point clicked, which is useful for confirming what was resolved.

### `type`

| field | required | meaning |
|---|---|---|
| `text` | yes | literal text to type |

Types into whatever currently has focus. Returns `{"ok": true, "typed": 11}`.
To type into a specific field, `click` it first — there is no "type into element"
form, because that would mean synthesizing focus events instead of clicking.

### `key`

Either a single key, or a combination that is pressed together:

```json
{"cmd": "key", "key": "enter"}
{"cmd": "key", "keys": ["ctrl", "shift", "t"]}
```

`keys` takes precedence over `key`. Returns `{"ok": true}`.

### `scroll`

| field | default | meaning |
|---|---|---|
| `delta` | -360 | positive scrolls down, negative scrolls up |
| `x`, `y` / `css_x`, `css_y` / `index` / `role`,`name` | centre of the viewport | where to scroll |

Returns `{"ok": true, "delta": 600}`.

### `screenshot`

| field | default | meaning |
|---|---|---|
| `path` | `%LOCALAPPDATA%\GhostBrowser\screenshot.bmp` | where to write |

Returns `{"ok": true, "path": "...", "width": 1280, "height": 720}`. The file is a
**BMP**, written by capturing the window through the OS, so it shows exactly what
was on screen including anything drawn outside the page.

### `captcha`

Reads the human-verification challenge on the page and, when asked, clicks it.

| field | default | meaning |
|---|---|---|
| `action` | `solve` | `detect` reads only; `solve` also clicks the checkbox and watches |
| `timeout` | `30000` | milliseconds to keep watching after the click |

Returns `provider`, `state`, `detail`, and when they are known `site_key`, `page_url`,
`frame_url` and `challenge_token`; `solve` adds `clicked` and `elapsed_ms`.

`provider` is one of `none`, `hcaptcha`, `recaptcha`, `turnstile`. `state` is one of
`absent`, `checkbox`, `visual`, `audio`, `solved`.

Everything is read from the accessibility tree — the same tree `find` walks, which
sees into the challenge's cross-origin iframe. A challenge frame's document node
carries the frame's URL as its `value`, and the sitekey lives in that URL, so
`site_key` is parsed out of it rather than guessed. `page_url` is the top-level
document's URL. `challenge_token` is reCAPTCHA's `bft` parameter, which is what a
solving service needs for an image challenge.

`site_key` is empty for Cloudflare Turnstile: its widget document has no `value`, so
the widget is recognized by name and automation id instead, and there is no URL to
parse.

What `solve` actually does: it clicks the checkbox once — twice if the first click
changes nothing, because the widget animates in and a click aimed at a stale
rectangle lands nowhere — then watches until the state settles:

- `solved` — the widget is gone, so the challenge was answered. Turnstile usually
  reaches this from the click alone.
- `visual` — an image challenge is open. A person still has to solve it; this is
  where the browser's job ends and yours begins.
- `audio` — an audio challenge is open. This is the state the local speech-to-text
  path is meant to act on.

`absent` right after a click is not "there is no challenge": it is what Cloudflare
looks like while it verifies, because the checkbox is gone while the widget is still
there. The command keeps watching through it.

### `shutdown`

Stops the server. If the server launched the browser (the normal case), it also
closes it: `WM_CLOSE` first, then a forced terminate after 8 seconds. Chromium
takes an exclusive lock on `--user-data-dir`, so leaving it running would make the
next `serve` on the same profile fail.

A server started with `--attach <pid>` does **not** close the browser it attached
to; it only detaches.

## Errors

`ok: false` with an `error` string. The messages are written to be actionable, and
a few are worth recognizing:

| message contains | means |
|---|---|
| `no browser is attached` | the browser died or never started |
| `the browser has no visible window yet` | no window after 30 s of waiting |
| `this session has no foreground window` | disconnected or headless session; see below |
| `nothing matched role=... name contains "..."` | a `click` located by role/name found no target |

Note that **`find` with no matches is not an error**: it returns
`{"ok": true, "count": 0, "nodes": []}`. Only `click` turns an empty match into a
failure, because a click that resolves to nothing cannot proceed. Client code must
treat `count == 0` as a successful "nothing there" answer, not as a failed request.
| `no element with index N in the last tree` | the index is stale; call `tree` again |
| `give x/y (screen), css_x/css_y (page), index (from tree), or role/name` | a click with no location |
| `unknown command: X` | typo, or a command from a newer version |

## Sessions

Input synthesis needs a **connected, foreground-capable session**. On a
disconnected RDP session or a headless one, `GetForegroundWindow()` returns null
and Windows discards synthesized input *while reporting success* — so `click` and
`type` would silently do nothing. Rather than let that look like a bug in your
script, `focus`, `click`, `type`, `key`, `navigate` and `scroll` fail up front with
`this session has no foreground window, so Windows discards synthesized input (a
disconnected or headless session); connect the session and retry`.

`status`, `windows`, `tree`, `find` and `screenshot` work in any session.

## Security

The pipe is created with the default DACL, which means any process running as the
same user can connect to it and drive the browser. That is the same trust boundary
as the profile directory and the browser itself. There is no token and no
authentication; do not expose the pipe across a trust boundary you would not
already share, and do not run `ghost serve` as a different user than the one who
owns the profile.

## A minimal client

This is the entire protocol in Python. `Open` a named pipe as a file, write a line,
read a line.

```python
import json, os

def ghost(cmd, pipe="default", **params):
    with open(rf"\\.\pipe\ghost-{pipe}", "r+b", buffering=0) as p:
        p.write((json.dumps({"cmd": cmd, **params}) + "\n").encode())
        return json.loads(p.readline())

print(ghost("status"))
print(ghost("navigate", url="https://example.com"))
```

Every other language is the same three lines: open the path, write a line of JSON,
read a line of JSON. Working clients for C#, Go, Rust and Node are in
[`examples/`](../examples), and there is an MCP server for agents in
[`mcp/`](../mcp).

## One-shot use without a client

`ghost call` is a built-in client, so a shell or a language with no pipe support
can still use the protocol:

```
ghost call '{"cmd":"status"}'
ghost call --pipe myprofile '{"cmd":"navigate","url":"https://example.com"}'
```
