---
name: ghost
description: Drive a real Chromium browser that presents no automation fingerprint — read pages, click, type, scroll, and screenshot. Use when a task needs a page that blocks headless or scripted browsers, when a Cloudflare-style interstitial or bot check is in the way, when you need a screenshot of a live authenticated page, or when you must interact with a site where Playwright/Selenium would be detected. Not for scraping HTML you can fetch directly, and not a replacement for an HTTP client.
---

# Driving the ghost browser

`ghost` is a real Chromium with OS-level hooks that make it look like an ordinary
desktop browser: no `navigator.webdriver`, no CDP port, and input that goes through
the same OS path a human's mouse and keyboard use. Sites that reject Playwright and
Selenium generally treat it as a normal visitor. (It does keep a DevTools channel
open internally, over an anonymous pipe rather than a port, because two things are
impossible without one: writing a hidden form field, and reading an `<audio>`
element's own URL. Nothing is injected into the page that runs before its own code.)

## Choosing how to drive it

Prefer the **MCP tools** if they are available (see the tool list below). They own
the browser lifecycle, render the page as text, and return PNG screenshots.

If MCP is not wired up in your environment, shell out to the CLI instead:

```bash
ghost serve --id agent --pipe agent https://example.com   # start it (background)
ghost call '{"cmd":"status"}' --id agent                  # one-shot request
ghost call '{"cmd":"navigate","url":"https://example.com"}' --id agent
ghost call '{"cmd":"tree","max_nodes":400}' --id agent
ghost call '{"cmd":"shutdown"}' --id agent
```

`ghost call` opens the pipe, sends one JSON request, prints one JSON reply, and
exits — so any language that can run a process can use it. The wire protocol is in
`docs/PROTOCOL.md`, and `examples/` has native clients for Python, .NET, Go, Rust,
and Node.

## The loop

1. **Open** — `ghost_open` (or `ghost serve`) starts the browser. It is a full
   Chromium window on the desktop, not headless. Passing `url` to `ghost_open` while
   nothing is running starts the browser *at* that page, which needs no synthesized
   input at all — do that rather than opening empty and navigating, and the read-only
   tools keep working even in a session with no foreground window.
2. **Go** — `ghost_navigate` with a URL. It types into the real address bar, so it
   needs a foreground-capable session.
3. **Read** — `ghost_page` returns the accessibility tree as text lines:
   `[14] button "Sign in" @144,256`. The number in brackets is the index.
4. **Act** — pass that index straight to `ghost_click`, or use `ghost_type` /
   `ghost_key` / `ghost_scroll`.
5. **Re-read** — `ghost_page` again to see what changed. Use `ghost_wait_for`
   when the change is asynchronous.

`ghost_click` also accepts `role` + `name`, `css_x`/`css_y` for a point in page
coordinates, or raw `x`/`y` screen pixels. Prefer an index from a page read you
just did: coordinates go stale as soon as the layout shifts.

## Human-verification challenges

Call `ghost_captcha` rather than hunting for the checkbox yourself. It reads the
challenge out of the accessibility tree — the same tree `ghost_page` walks, which
sees inside the challenge's cross-origin iframe — and reports the provider, the
state, and the sitekey. With `action="solve"` it also clicks the checkbox and says
where that got you.

**One look is not a detection.** The widget animates in, so for roughly the first
second a page that is about to challenge you looks exactly like a page that never
will. `ghost_captcha` already accounts for this — it keeps looking for up to `wait_ms`
(default 3000) and reports `appeared_ms` — but do not conclude "no challenge" from a
tree you read yourself too early. If you want to watch without acting, use
`action="wait"`: it clicks nothing and reports `appeared`, `appeared_ms`, `cleared` and
`waited_ms`. A challenge that is only being verified needs time, not input.

- `state: solved` — the widget is gone; the challenge is answered.
- `state: visual` — an image challenge is open. **You cannot solve this, and neither
  can the browser yet.** Report it to the user instead of burning attempts.
- `state: audio` — an audio challenge is open. Call `ghost_captcha` again with
  `action="solve-audio"`: it records what the machine actually played, transcribes the
  digits and types them back. Give it `language` (e.g. `en-US`) — the machine's speech
  recognisers are installed per language, and it may not have one for the challenge's.

Cloudflare Turnstile usually reaches `solved` from the click alone, so
`ghost_captcha` can clear a Cloudflare interstitial on its own. hCaptcha and
reCAPTCHA escalate; for reCAPTCHA the audio route finishes the job, and for hCaptcha
the honest answer is that it stops there.

When `solve-audio` comes back empty, read the response before retrying:

- `play` — whether the challenge's play control was pressed. The audio challenge does
  not start itself, and an unpressed control looks exactly like a silent page.
- `streams` — `nothing held a stream` means the page never played anything, so a retry
  will not help. A browser process that is `active` while `peak` is `0` means the
  recording, not the page, is at fault.
- `solved_by` — `local` or `api`, so you know whether the answer came from this machine
  or from a solving service. With no key configured the service tier is off, and the
  command says so rather than failing obscurely.

Do not retry a challenge in a loop. Repeated attempts are themselves a bot signal,
and a challenge that fails often comes back harder. If you must retry, restart the
browser rather than clicking again — a half-clicked widget keeps its state, and
clicking a checkbox that is already answered is itself a failure.

## What this browser deliberately cannot do

- **There is no JavaScript evaluation**, over MCP or the pipe. Running script in
  the page is exactly the signal this browser exists to avoid. If you need data
  out of a page, read the tree, or screenshot and read the image.
- **There is no headless mode.** A headless window cannot receive synthesized
  input, and headless is itself the loudest automation tell.
- **There is no network interception or request mocking.** Use a real HTTP client
  for anything you do not need a browser for.

## Pitfalls

- **Input needs a foreground-capable desktop session.** On a disconnected RDP
  session, Windows silently discards synthesized input while reporting success, so
  clicks and typing appear to do nothing. `ghost_status` will report no window, or
  the input command will say the session has no foreground window. Reading the tree
  and taking screenshots still work. This is the most common cause of "the click
  did nothing".
- **One browser per pipe id.** A second `serve` on the same id fails because
  Chromium takes an exclusive lock on the profile directory. Use a different
  `GHOST_ID` for a second session, or close the first.
- **The first navigation is slow.** Chromium startup is 5–15 seconds; the MCP
  server waits for the window before returning from `ghost_open`.
- **`ghost_type` types into whatever has focus.** There is no "type into this
  element" call — click the field first.
- **Screenshots from the CLI are BMP.** The MCP tool converts to PNG for you.
- **The window title carries the product name.** That is intentional branding, not
  a spoofing failure; a page cannot read the window title.

## Verifying it is working

```bash
ghost selftest     # checks the hooks in-process, no browser needed
ghost doctor       # reports the exe, cache, profiles, browser, and host state
```

`ghost selftest` passing means the OS-level hooks are live. If a fingerprint check
fails on a real page, that is a different problem from the hooks being absent.
