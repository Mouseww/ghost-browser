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

An `index` is a **position in the accessibility walk**, counted before any
`role`/`name` filter is applied. So `find` and `tree` number the same element the
same way, and an index from one can be handed to `click` after the other. A client
that filters `find` results in its own code does not need to translate the indices.

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

The window is focused first. Returns `{"ok": true, "x": 144, "y": 256, "input":
"synthesized"}` — the actual point clicked, which is useful for confirming what was
resolved.

`input` says which channel carried the click, and the two are **not equivalent**:

| `input` | what happened |
|---|---|
| `synthesized` | a real `SendInput` click. The page sees a trusted event, exactly as from a person. |
| `accessibility` | the control was activated through UI Automation. It changes the page, but `event.isTrusted` is false. |

Synthesized input is always preferred. The accessibility channel is the fallback
for a session that cannot deliver input at all (see [Sessions](#sessions)), and it
is used only for a **plain left click on a named element** — a right click, a
double click, or a bare coordinate has no accessibility equivalent, and those are
refused rather than silently downgraded. A caller that cares whether the page saw a
trusted event should read `input`.

### `type`

| field | required | meaning |
|---|---|---|
| `text` | yes | literal text to type |
| `index` | no | an element to write the text into |

Without `index`, text goes to whatever currently has focus. Returns
`{"ok": true, "typed": 11, "input": "synthesized"}`.

With `index` the same channel rule applies: real keystrokes when the session can
deliver them, otherwise the value is written through UI Automation's value
interface and `input` is `accessibility`. Writing a value is not typing — no
keystroke events are generated — which is why the channel is reported rather than
hidden.

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
| `action` | `solve` | `detect` reads only; `wait` reads and watches without clicking; `solve` also clicks the checkbox and watches; `solve-audio` records the audio challenge, transcribes it and answers it |
| `timeout` | `30000` | milliseconds to keep watching after the click, or for `wait` to keep watching at all |
| `wait_ms` | `3000` | how long the *first* look may keep looking for a challenge to appear; `0` means a single sample |
| `seconds` | `10` | `solve-audio` only: how long to record, clamped to 2–30 |
| `language` | `en` | `solve-audio` only: the recogniser language and the solving service's hint |
| `keep` | `false` | `solve-audio` only: keep the recording and return its path as `wav`, instead of deleting it |
| `rounds` | `1` | `solve-audio` only: how many audio clips the command is willing to answer in one call, clamped to 1–5. reCAPTCHA often rejects a correct answer once and plays the next clip; a second round answers that clip too instead of reporting a single-round failure |

Returns `provider`, `state`, `detail`, and when they are known `site_key`, `page_url`,
`frame_url` and `challenge_token`; `solve` adds `clicked`, `input` and `elapsed_ms`.
`input` is `synthesized` or `accessibility` and means the same thing it does for
`click` — read it before treating a solved challenge as evidence that a trusted click
answered it.

**One look is not a detection.** The widget animates in: measured on this machine,
Cloudflare's interstitial exposes no challenge at all for its first ~1.2 s and
hCaptcha's checkbox for ~1.2 s. During that window a page that is about to challenge
you and a page that never will are the same picture, so the first look keeps looking
for up to `wait_ms` before it is willing to say `provider: none`. When a challenge is
found, `appeared_ms` says how long that took; it is absent when nothing appeared.

`action=wait` clicks nothing. It watches for a challenge to arrive (using the whole
`timeout` for that), then watches for it to leave on its own, and reports:

- `appeared` — whether a challenge named itself at all.
- `appeared_ms` — how long that took.
- `cleared` — whether it then went away without being touched. A challenge that is
  only being verified clears; one that wants a person does not.
- `waited_ms` — the total time spent watching.
- `clicked` — always `false`, so a caller can tell this action from `solve`.

`wait` stops early when the challenge settles into `visual` or `audio`, because those
are waiting for a person and more waiting cannot change that. Use it when a challenge
may be transient, and before concluding a page has no challenge at all.

`solve-audio` additionally returns `play`, `device`, `captured_seconds`, `peak`, `rms`,
`streams`, `solved_by`, `heard`, `confidence` and `typed`. It works with no foreground
window, because every click it makes falls back to the accessibility channel.

- `play` — whether the challenge's own play control was pressed. **The audio challenge
  does not start itself**: until that button is pressed the page holds an open stream
  that renders nothing, so `peak 0` and "the page played nothing" look identical.
- `streams` — who held a stream on the output device during the recording, and how
  loud. The same list the Windows volume mixer draws.
- `solved_by` — `local` when the digits came from the machine's speech recogniser,
  `api` when they came from a solving service.

With `rounds` above 1 the command answers up to that many clips in one call: after a
typed answer fails to clear the challenge it clicks the reload control, re-reads the
tree (redraws invalidate indexes), and starts the next clip's recording. The reply
then also carries `rounds_attempted` — how many clips were answered — and `solved`
reflects the state after the last one. A single-round call keeps the reply shape it
always had and adds no round fields.

The third tier is off unless a key is configured. There is no built-in key. The key is
taken from `GHOST_CAPTCHA_KEY`, or from `captcha_api_key` in the profile the server was
started with; `GHOST_CAPTCHA_URL` overrides the service and defaults to
`https://2captcha.com`. With no key, `provider` is empty and `solve-audio` stops at the
local recogniser's honest refusal.

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
- `audio` — an audio challenge is open. This is the state `solve-audio` acts on.

`absent` right after a click is not "there is no challenge": it is what Cloudflare
looks like while it verifies, because the checkbox is gone while the widget is still
there. The command keeps watching through it.

`index` values in `find` results are only meaningful against the tree they came from.
A challenge that redraws between your `find` and your `click` invalidates them, which
is why the command reads the tree again after every click it makes rather than reusing
an index from before.

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
| `no element with index N in the last tree` | the index is stale; call `tree` again |
| `give x/y (screen), css_x/css_y (page), index (from tree), or role/name` | a click with no location |
| `the control exposes no way to be activated` | the element offers no invoke, toggle, select or default action |
| `unknown command: X` | typo, or a command from a newer version |

Note that **`find` with no matches is not an error**: it returns
`{"ok": true, "count": 0, "nodes": []}`. Only `click` turns an empty match into a
failure, because a click that resolves to nothing cannot proceed. Client code must
treat `count == 0` as a successful "nothing there" answer, not as a failed request.

## Sessions

Input synthesis needs a **connected, foreground-capable session**. On a
disconnected RDP session or a headless one, `GetForegroundWindow()` returns null
and Windows discards synthesized input *while reporting success*. Rather than let
that look like a bug in your script, the control plane checks before synthesizing
anything.

What happens next depends on whether the command can be carried out some other way:

| command | in a session with no foreground window |
|---|---|
| `click` by `index` or `role`/`name` | falls back to UI Automation; reports `input: "accessibility"` |
| `click` by `x`/`y`, with `button` other than left, or `count` > 1 | fails with `this session has no foreground window...` |
| `type` with `index` | falls back to the accessibility value interface |
| `type` without `index` | fails with the same message |
| `focus`, `key`, `navigate`, `scroll` | fails with the same message |

The failure text is `this session has no foreground window, so Windows discards
synthesized input (a disconnected or headless session); connect the session and
retry`.

`status`, `windows`, `tree`, `find`, `screenshot` and `captcha` work in any
session. `captcha action=wait` and `captcha action=detect` only read, so they never
need input at all; `captcha action=solve-audio` also works without a foreground
window: the audio comes off the render endpoint and the answer is written through
accessibility, so the whole tier runs headless. Only the checkbox click in
`action=solve` needs the accessibility fallback.

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
