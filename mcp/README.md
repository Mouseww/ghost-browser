# The ghost MCP server

[`ghost_mcp.py`](ghost_mcp.py) exposes the browser to any
[Model Context Protocol](https://modelcontextprotocol.io) client — Claude Desktop,
Claude Code, an IDE, or your own agent loop. It is a single standard-library Python
file that speaks JSON-RPC over stdio and forwards every call to the control plane
documented in [`docs/PROTOCOL.md`](../docs/PROTOCOL.md).

## What it adds over the raw pipe

An agent *could* open the named pipe itself. This server exists because four things
are worth doing once, correctly, rather than in every agent:

- **It owns the browser lifecycle.** The first tool call starts `ghost serve` and
  waits for the window; `ghost_close` shuts it down. No agent has to know that
  Chromium takes an exclusive lock on the profile directory.
- **It renders the accessibility tree as text.** `ghost_page` returns lines like
  `[14] button "Submit" @144,256` — an index the model can pass straight back to
  `ghost_click`. Raw JSON of the same tree is mostly punctuation.
- **It returns screenshots as PNG.** MCP image content only carries PNG and JPEG,
  and ghost captures BMP because that is what the OS hands over cheaply. The
  conversion is 40 lines of `zlib` and `struct` here, so the C++ side stays simple
  and nothing gains an image dependency.
- **It translates the failure that matters.** Input commands need a connected,
  foreground-capable desktop session. When there isn't one, the OS silently drops
  synthesized input, so the server surfaces that as an explanation instead of
  letting an agent conclude the page did not change.

## Requirements

- **Python 3.10 or newer** (it uses `X | Y` type syntax).
- **`ghost.exe`** — from [releases](https://github.com/Mouseww/ghost-browser/releases/latest),
  or `native/build/bin/ghost.exe` in a checkout.
- Windows. The whole control plane is Windows-only today.

The server finds `ghost.exe` in this order: `GHOST_EXE`, then `PATH`, then
`%LOCALAPPDATA%\GhostBrowser\bin\ghost.exe` (where `ghost install` puts it), then
the build tree relative to this repository.

## Registering it

### Claude Code

```bash
claude mcp add ghost -- python /absolute/path/to/mcp/ghost_mcp.py
```

### Claude Desktop, and anything else that reads `mcpServers`

```json
{
  "mcpServers": {
    "ghost": {
      "command": "python",
      "args": ["E:\\projects\\unknowbrowser\\mcp\\ghost_mcp.py"],
      "env": {
        "GHOST_EXE": "E:\\projects\\unknowbrowser\\native\\build\\bin\\ghost.exe",
        "GHOST_ID": "agent"
      }
    }
  }
}
```

On macOS or Linux the same JSON works with forward slashes; the server will start,
but `ghost serve` does not exist there yet.

## Tools

| Tool | Arguments | What it does |
|---|---|---|
| `ghost_open` | `url?` | Start the browser, optionally open a URL |
| `ghost_status` | — | pid, profile, data dir, window title and size |
| `ghost_navigate` | `url` | Go to a URL via the real address bar |
| `ghost_page` | `max_depth?`, `max_nodes?`, `anonymous?` | Read the page as text |
| `ghost_find` | `role?`, `name?`, `max_nodes?` | Find elements; `name` is a substring |
| `ghost_wait_for` | `role?`, `name?`, `timeout?` | Poll until an element appears |
| `ghost_click` | `index?` \| `role`+`name` \| `css_x`+`css_y` \| `x`+`y`, `button?`, `count?` | Click |
| `ghost_type` | `text` | Type into the focused field |
| `ghost_key` | `key` \| `keys[]` | Press a key or a combination |
| `ghost_scroll` | `delta?`, `css_x?`, `css_y?` | Scroll |
| `ghost_captcha` | `action?` (`detect`/`wait`/`solve`/`solve-audio`), `timeout?`, `wait_ms?`, `seconds?`, `language?`, `keep?` | Read, watch, click, or record-and-answer a human-verification challenge |
| `ghost_screenshot` | — | Capture the window as PNG |
| `ghost_close` | — | Close the browser and stop the server |

The intended loop is: `ghost_navigate` → `ghost_page` → `ghost_click` with an index
from that page → `ghost_page` again to see what changed. `ghost_wait_for` covers the
gap between an action and the content it loads.

When a page is behind a human-verification challenge, call `ghost_captcha` instead of
hunting for the checkbox yourself. It reads the challenge from the accessibility tree
and returns the provider, the state and the sitekey; with `action="solve"` it clicks
the checkbox and reports where that got you. Cloudflare Turnstile usually passes from
that click alone. hCaptcha and reCAPTCHA escalate to an image or audio challenge,
which the tool reports honestly rather than pretending to have solved.

**One look is not a detection.** The widget animates in, so for roughly the first
second a page that is about to challenge you looks exactly like a page that never will.
`ghost_captcha` keeps looking for up to `wait_ms` (default 3000; `0` restores a single
sample) before it will say `provider: none`, and reports `appeared_ms`. `action="wait"`
clicks nothing and reports `appeared`, `appeared_ms`, `cleared` and `waited_ms` — use it
when a challenge is being verified and needs time rather than input.

The audio challenge is the automatable one: `action="solve-audio"` records what the
machine actually played, transcribes the digits and types them back, and says whether
the answer came from this machine (`solved_by: local`) or from a solving service
(`api`). Pass `language` — speech recognisers are installed per language and the
machine may not have one for the challenge's. A solving service is used only when the
local recogniser comes back empty **and** `GHOST_CAPTCHA_KEY` (or `captcha_api_key` in
the profile) is set; with no key the service tier is simply off. `keep=true` returns
the recording's path, which is what you want when the transcription fails.

Every click reports `input`: `synthesized` when a real `SendInput` carried it,
`accessibility` when it went through UI Automation instead. The second happens when
the session has no foreground window — a disconnected RDP session, a service, anything
headless — and it is why these tools keep working there at all.

## Environment

| Variable | Default | Meaning |
|---|---|---|
| `GHOST_EXE` | auto-detected | Path to `ghost.exe` |
| `GHOST_ID` | `mcp` | Profile and pipe id; a separate id is a separate browser profile |
| `GHOST_URL` | none | URL opened when the browser first starts |
| `GHOST_TIMEOUT` | `60` | Seconds to wait for the browser to come up |

## What it will not do

- **No JavaScript evaluation.** There is deliberately no `evaluate` tool. Running
  script in the page is the thing this browser exists to avoid, and it is trivially
  detectable. If you need data out of a page, read the accessibility tree or take a
  screenshot.
- **No headless mode.** Headless Chrome is the most obvious automation signal there
  is, and input synthesis does not work without a real window.
- **No parallel sessions in one server.** Each server instance owns one browser and
  one profile. Run several servers with different `GHOST_ID`s if you need several.

## Testing

```bash
python tools/mcp_check.py
```

This spawns the server exactly as a client would, performs the MCP handshake,
asserts the tool list, drives a real browser to `https://example.com`, checks that
the page text and the PNG screenshot come back, and verifies that a bad tool call
returns an error instead of killing the session. Checks that need a foreground
desktop are reported as *not measurable in this session* rather than failed.
