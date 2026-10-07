# Changelog

All notable changes to this project are recorded here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.9.0] - 2026-10-07

### Added

- **`captcha action=wait`: watch a challenge arrive, and watch it leave, without
  touching it.** A challenge that is only being verified needs time rather than input,
  and clicking into that window is guessing. `wait` clicks nothing, reports `appeared`,
  `appeared_ms`, `cleared` and `waited_ms`, and stops early once the challenge settles
  into an image or audio one, because those are waiting for a person and more waiting
  cannot change that. Measured on Cloudflare's interstitial: `appeared: true` at
  **953 ms**, `cleared: false` after 9 s — that demo never passes on its own, and the
  command says so instead of pretending patience is a solution.

### Fixed

- **The first look at a challenge is no longer a single sample.** The widget animates
  in, and the gap is not subtle: measured on this machine, Cloudflare's interstitial
  exposes no challenge at all for its first **~1.2 s**, and hCaptcha's checkbox for
  **~1.2 s**. During that window a page that is about to challenge you and a page that
  never will are the same picture, so `captcha` used to answer `provider: none` — and an
  agent reading that concludes there is no challenge and walks into a blocked page,
  which is the one failure the first tier exists to prevent. Detection now keeps looking
  for up to `wait_ms` (default 3000; `0` restores the single-sample behaviour) before it
  will say `none`, and reports `appeared_ms` so a caller can tell a fast page from a slow
  one. Measured after the fix: Cloudflare named at **953 ms**, hCaptcha at **891 ms**,
  reCAPTCHA at **171 ms** — where all three previously said `none`. Acceptance grew from
  21 to **33 checks, 0 failed, 0 not measurable** (`tools/captcha_check.py`), because the
  tier that was missing its most important property was also the one nobody was checking.

### Investigated

- **hCaptcha's audio route is a measured dead end, not an oversight.** hCaptcha is the
  one provider whose audio challenge is still not driven, so it was worth finding out
  why. Its challenge frame does expose a button named `About hCaptcha & Accessibility
  Options` — with no automation id, so it can only be found by name — and hCaptcha's own
  image alt text points at that menu for "Get Cookie" and "Text Challenge". Activating it
  through UI Automation, however, changes nothing in the tree for ten seconds, sampled
  once a second. The same invoke pattern *does* work on hCaptcha's checkbox (the state
  goes `checkbox` → `visual`), so the obstacle is the control rather than the channel: the
  checkbox is a real form control, the menu button is a custom element that Chromium gives
  a button role without wiring up its handler. A real mouse event might work, and that
  needs a foreground-capable session this machine does not have. Recorded as a limitation
  with its evidence rather than left as an unexplained gap.

## [0.8.0] - 2026-10-07

### Added

- **The browser now works in sessions that cannot deliver input.** A disconnected
  RDP session, a service, or anything headless has no foreground window, and Windows
  silently discards synthesized input there — `ghost` used to fail outright. `click`,
  `type` and `captcha` now fall back to the UI Automation invoke and value patterns,
  which reach the page through the accessibility tree instead of the input queue, so
  they work with no foreground window at all. **The fallback is reported, not hidden**:
  every click and type returns `input`, which is `synthesized` when a real `SendInput`
  carried it and `accessibility` when it went through UI Automation. The two are not
  equivalent — a page sees trusted events from the first and `event.isTrusted: false`
  from the second — so a caller can tell which one answered its challenge. Only a
  plain named left-click falls back; right-clicks and double-clicks have no
  accessibility equivalent and are refused rather than silently downgraded.
  Measured: in a session where `GetForegroundWindow()` returns 0 and a full
  `AttachThreadInput`/`SetForegroundWindow` sequence still fails, clicking a link by
  index navigates the page and reports `input: accessibility`.
- **Third tier: when the machine has no recogniser for the challenge's language, the
  recording goes to a solving service instead.** The audio route records what the
  machine actually played; the local Windows recogniser is tried first, and only if
  it returns nothing *and* a key is configured does the same recording go to a
  2captcha-compatible endpoint. There is no built-in key and there never will be:
  with none configured the third tier is off, not broken. `GHOST_CAPTCHA_KEY` or
  `captcha_api_key` in the profile supplies the key, `GHOST_CAPTCHA_URL` overrides
  the service. The upload is down-mixed and resampled to 16 kHz mono 16-bit — about
  157 KB for a five-second challenge — and the response says `solved_by: api` rather
  than leaving you to guess where the digits came from. Acceptance
  (`tools/solve_api_check.py`) points `GHOST_CAPTCHA_URL` at a stand-in service on
  localhost, so the protocol and the pipeline are verified without spending money or
  depending on anyone's answer quality: **12 checks, 0 failed** when reCAPTCHA serves
  the audio challenge. It is rate-limited too — after enough attempts from one address
  reCAPTCHA stops offering the audio route, and then stops serving the widget at all —
  so the script reports that as *not measurable* rather than as a failure, because
  blaming this tier for the challenge refusing would be as dishonest as claiming a run
  that never happened.
- **`solve-audio` presses the challenge's own play control**, and reports it as
  `play`. **The audio challenge does not start itself**, and until its play button is
  pressed the page holds an open stream that renders nothing — which looks exactly
  like a page that played nothing. That was the whole of the previously unexplained
  `peak 0`: not a broken capture, but a challenge waiting for a person. The control
  is found structurally rather than by its label, because the label is a localized
  sentence; after the fix the same capture went from `peak 0` to `peak 0.1767`, and a
  kept recording measured `44100 Hz 2ch 16-bit 7.98 s` with 7 seconds of sound.
- `solve-audio` takes `keep`, which returns the recording's path as `wav` instead of
  deleting it, because a failed transcription cannot be diagnosed from `peak`, `rms`
  and `confidence` alone.
- `ghost __audio sessions` lists the OS audio sessions — process id, state, whether
  it is the system-sounds session, peak level and identifier. This is the same data
  the Windows volume mixer draws, and it is what distinguishes "the page never played
  anything" from "nothing ever opened a stream", which samples alone cannot.

### Fixed

- **`find` and `tree` disagreed about what an element's index was.** `find` numbered
  elements after filtering and `tree` numbered them before, so
  `find(role="link")[0]["index"]` returned 0 and `click(index=0)` clicked the first
  element of the whole tree. Indices are now assigned to every element the walk
  stores, and filtering only decides what is reported, not where anything is.
- **A success branch that only updated the fields it wrote left the rest of the
  verdict stale.** When the solving service returned digits, the code overwrote
  `digits` but left `ok` false, so the later early-return fired and the service's
  answer was never typed into the page — visible as `heard` populated while `typed`
  was `None`. A branch that succeeds has to update the whole verdict, not just its
  own fields.

### Changed

- `captcha` reports `input` for the click that carried it, so a solved challenge is
  no longer treated as evidence that a trusted click answered it.
- The captcha acceptance scripts retry by **restarting the browser** rather than
  re-clicking, because a half-clicked widget keeps its state and clicking a checkbox
  that is already answered is itself a failure. `tools/captcha_check.py` runs
  **21 checks, 0 failed, 0 not measurable** — including with no foreground window.

## [0.7.0] - 2026-10-07

### Added

- **The audio challenge route: record what the machine actually played, then
  transcribe it locally.** `captcha` on the control plane takes
  `action=solve-audio`, and `ghost_captcha` in the MCP server exposes it. It opens
  the audio challenge, records the default output device in WASAPI loopback mode,
  asks for a replay so the clip starts inside the recording window, transcribes the
  digits with the Windows speech engine and types them back. Nothing is downloaded
  and no URL is parsed: the samples came through the OS audio stack, so they are
  what the browser really played. Verified end to end that the capture path works —
  a plain `<audio autoplay loop>` page records at `peak 0.610340`, `rms 0.427081`,
  `silent 0 frames`, with no input synthesis at all.
- **A digit-only grammar, which is the difference between unusable and usable.**
  Free dictation spends its probability mass on words; a captcha answer needs
  digits. Over 12 random five-digit strings synthesised locally, dictation scored
  **2/12 exact, 17/60 digits (28.3%)** at confidence ≈0.02, while the SRGS digit
  grammar scored **10/12 exact, 58/60 digits (96.7%)** at confidence 0.73–0.99.
  Padding the clip with 0/250/500 ms of silence changed nothing, so the misses are
  acoustic-model errors rather than a clipped onset.
- `ghost __audio list|sessions|<seconds> [out.wav]` and
  `ghost __speech list|<wav> [lang] [--digits]`, hidden diagnostics that report the
  level, who is playing and how sure the recogniser was. A capture that came back
  silent is ambiguous — the page may have played nothing, or nothing may have opened
  a stream at all — and the session list is the same data the Windows volume mixer
  draws, so the two can be told apart.
- `solve-audio` reports `peak`, `rms`, `streams` and `confidence` alongside the
  transcript, because a wrong answer costs an attempt and "it heard nothing" needs
  to be diagnosable from the response alone.

### Known limitations

- **The reCAPTCHA audio challenge measured `peak 0` in every run that reached it**,
  including with autoplay permitted, while a plain page on the same machine records
  at `peak 0.61`. The capture path is not the problem; what the challenge itself did
  is still being determined, and `streams` was added to answer it. Not solved, and
  not claimed to be.
- **Only a Chinese speech recogniser is installed here**
  (`MS-2052-80-DESK`), and adding an English one needs administrator rights, so the
  English audio challenge cannot be transcribed locally on this machine.
  `ghost __speech` says so plainly rather than guessing.

## [0.6.0] - 2026-10-07

### Added

- **Human-verification challenges can be read, and Cloudflare Turnstile can be
  cleared.** `ghost captcha` reports the provider, the state and the sitekey, and with
  `action=solve` clicks the checkbox and reports where that left it. It reads the
  accessibility tree rather than the DOM, which works because that tree already reaches
  inside the challenge's cross-origin iframe, and because a challenge frame's document
  node carries its own URL as its `value` — the sitekey is parsed out of that URL. No
  CDP, no injected script. Measured against the three real challenges:
  [`tools/captcha_check.py`](tools/captcha_check.py) reports **18 checks, 0 failed**.
  Cloudflare Turnstile is answered outright by one trusted click. hCaptcha and
  reCAPTCHA are identified and clicked open; their audio challenges are reachable and
  reCAPTCHA's challenge token is read, but nothing solves them yet.
- `ghost_captcha` in the MCP server, so an agent does not have to hunt for the checkbox
  itself.

### Fixed

- **The accessibility tree is primed at session start.** Chromium builds its
  accessibility tree lazily, and the first query is what asks for it — so the first
  `tree` or `find` after a fresh browser returned a chrome-only tree with no document
  in it, which is indistinguishable from an empty page. A client that read once and
  concluded the page was blank was told the truth about the tree and a lie about the
  page. The server now queries once at startup, and again whenever the window handle
  changes.
- **`navigator.deviceMemory` is capped at 32 GiB**, not at the machine's size. Measured
  across 4/8/16/32/48/64 GiB: 4→4, 8→8, 16→16, 32→32, 48→32, 64→32. The harness expected
  the raw capacity, so a profile with 64 GiB failed a spoof that was behaving correctly.
- **`{"cmd":"call"}` deadlocked the server.** It re-sent the request to the pipe it was
  itself serving, nesting a connection into a single-threaded server until the client
  timed out.
- Three protocol details the language clients found: `--pipe` is the literal pipe name
  rather than a suffix on `ghost-<id>`; `find` with no matches is a successful empty
  answer rather than an error; and `status.pipe` returns the bare name.

## [0.5.0] - 2026-10-07

### Added

- **The control plane is specified.** [`docs/PROTOCOL.md`](docs/PROTOCOL.md) documents
  the transport, the line-delimited JSON framing, all twelve commands with their
  arguments and replies, the error each can return, and a fifteen-line client. The
  protocol had existed since 0.3.0 without a specification, which is an odd place to
  leave the interface this browser exists to expose.
- **A client for every common language.** [`examples/`](examples) holds C#, Go, Rust
  and Node clients, each compiled and run against a live browser;
  [`tools/ghost_client.py`](tools/ghost_client.py) remains the Python one. None of
  them needs a dependency — each opens a file and exchanges lines of JSON.
  `ghost call '<json>'` covers the case where a program would rather link nothing.
- **An MCP server**, [`mcp/ghost_mcp.py`](mcp/ghost_mcp.py): twelve tools over stdio,
  standard library only. It starts and stops the browser itself, renders the
  accessibility tree as text an agent can act on (`[14] button "Sign in" @144,256`),
  and converts the BMP captures to PNG because MCP image content carries only PNG and
  JPEG. [`tools/mcp_check.py`](tools/mcp_check.py) spawns it the way a client does and
  drives a real page through it.
- **A skill**, [`skills/ghost/SKILL.md`](skills/ghost/SKILL.md): when this browser is
  the right tool, the navigate → read → act → re-read loop, and the failure modes that
  look like bugs. An MCP server supplies capability; it cannot supply judgement.

### Fixed

- **The first `tree` or `find` after a browser started returned only browser
  chrome, as if the page were empty.** Chromium builds its accessibility tree
  lazily: the first UI Automation query is what switches it on, and the document
  appears in a *later* query. Measured on a fresh profile, querying once a second:
  the query at +0.22 s saw 57 nodes and no document, while the query at +1.37 s saw
  65 nodes with the document and its seven text nodes. An agent that called
  `ghost_page` once and concluded the page had no content was being misled by us,
  not by the page. `ghost serve` now primes accessibility as soon as the window
  appears, and again whenever a navigation replaces the window handle, so the first
  query a client makes is already correct. The fix needs no extra flag;
  `--force-renderer-accessibility` also works but puts a visible automation tell on
  the command line, which is exactly what this browser exists to avoid.

- **`navigator.deviceMemory` is capped at 32 GiB by Chrome, and the acceptance suite
  expected the profile's raw size.** The profile generator offers a 64 GiB machine,
  and a run using it failed with `actual=32 expected=64`. Measured against profiles
  of 4, 8, 16, 32, 48 and 64 GiB, Chrome 154 reported 4, 8, 16, 32, 32 and 32 — so a
  real 64 GiB machine reports 32 as well, the spoof was behaving correctly, and the
  expectation was what needed fixing. `harness/run_detect.py` now derives what Chrome
  derives: the physical memory rounded down to a power of two, capped at 32 GiB.

- **`ghost serve` answered `{"cmd":"call"}` by forwarding the request to its own
  pipe**, re-entering the same single-threaded server and deadlocking until the 60 s
  timeout. `ghost call` is the client, and that name belongs to it; the command now
  returns `unknown command: call` immediately.

- **Three protocol details the clients disagreed with the documentation about.**
  `--pipe` is the literal pipe name rather than a suffix, so `serve --id foo --pipe bar`
  listens on `\\.\pipe\bar`; `find` with no matches is a success carrying
  `count: 0`, not a failure; and `status.pipe` returns the bare name while `status.pid`
  is the server's pid rather than the launcher's. All three are now written down.

## [0.4.0] - 2026-10-06

### Added

- **Font enumeration is spoofed.** `document.fonts`, canvas text measurement and
  font-metric probing all resolve through DirectWrite on Windows, so the shim now
  hooks `dwrite.dll!DWriteCreateFactory`, replaces the `IDWriteFactory`
  `GetSystemFontCollection` slot, and filters the `IDWriteFontCollection` it
  returns down to the profile's `fonts` allow-list. The machine measured here had
  **266** font families — Office, a Chinese IME pack and a developer's toolchain
  at once, an unusually loud fingerprint — and a profile now presents the ~89 that
  a stock Windows install ships. Verified in the renderer: `Arial`, `Segoe UI`,
  `Times New Roman` and `Bahnschrift` still resolve, while `Cascadia Code`,
  `Noto Sans SC`, `Ubuntu Mono` and `Agency FB` are gone. A/B controlled: with
  `GHOST_HOOK_MASK=BF` (every group except fonts) those four come back and the
  check fails, which is what proves the font hook is the thing removing them.
  - It is a filter, not a fiction: the visible set is the profile's list
    intersected with what the OS actually has, so a profile can hide a font but
    cannot invent one that is not installed.
  - `FindFamilyName` consults the real collection first and then asks "is that
    family one we admit?", so a family's localized aliases (`ＭＳ ゴシック` for
    `MS Gothic`) resolve correctly without matching on the requested string.
  - If none of the profile's families exist on the host, the hook declines to
    patch rather than tell the browser the machine has no fonts at all — an empty
    collection would wreck text rendering far more visibly than an unfiltered one.
  - Canvas and audio are **not** spoofed and cannot be by this technique: their
    pixels and samples are produced by Skia and Blink inside the renderer and
    never cross an OS API for a hook to intercept.

### Fixed

- **`GHOST_HOOK_MASK` silently disabled every hook when written with a `0x`
  prefix.** The value is hexadecimal, and `wcstoul(text, nullptr, 16)` parses
  `"0x3F"` as `0` rather than failing, so the documented spelling of the mask
  turned all hook groups off. The parser now skips an optional `0x`/`0X` prefix.

- **Window activation was flaky.** Windows refuses `SetForegroundWindow` to a
  process the user is not interacting with, and a single `AttachThreadInput`
  plus an ALT press was not always enough — a release acceptance run died with
  "the window could not be brought to the foreground". Activation now also
  attaches to the thread that currently owns the foreground, retries the whole
  sequence, uses the topmost raise/lower toggle that the foreground lock does
  not block, and falls back to `SwitchToThisWindow`.
- **A disconnected session failed silently.** Windows discards synthesized input
  while a session is disconnected — there is no foreground window for `SendInput`
  to deliver to, and the call reports success anyway. Every command after the
  first therefore timed out with no explanation. The control plane now detects
  the condition and says so: "this session has no foreground window, so Windows
  discards synthesized input (a disconnected or headless session); connect the
  session and retry". This was found the hard way: the same suite that passed
  17/17 failed completely after the RDP session dropped, with no code change
  between the two runs.
- **`serve_check.py` reported environment blocks as failures.** A check that
  cannot run because the session cannot deliver input is now reported as `GAP`
  with the reason, and the summary counts it separately
  (`17 checks, 0 failed, 7 not measurable in this session`) instead of claiming
  a pass it did not earn.

## [0.3.0] - 2026-10-06

The browser can be driven now, and it wears the project's name. `ghost serve`
replaces the debugging protocol entirely, and the shim rebrands the window it
lives in.

### Added

- **`ghost serve` — a zero-CDP control plane.** Newline-delimited JSON over a
  **named pipe** (`\\.\pipe\ghost-<id>`), never a TCP port, because a page can
  scan ports and cannot scan pipes. Commands: `status`, `windows`, `focus`,
  `navigate`, `tree`, `find`, `click`, `type`, `key`, `scroll`, `screenshot`,
  `call`, `shutdown`.
  - **Input is synthesized with `SendInput`.** Mouse movement is interpolated
    (smoothstep easing with a slight vertical bow, never a teleport), clicks hold
    for 35–90 ms, and typing sends `KEYEVENTF_UNICODE` per UTF-16 code unit so it
    bypasses the keyboard layout completely. Every event the page sees is
    `isTrusted`, with a real hardware timestamp.
  - **Reading the page uses the UI Automation tree**, not an injected binding, so
    there is no `Runtime.enable`, no utility script, and nothing for the page to
    observe. `navigate` types into the address bar with `Ctrl+L` rather than
    touching the engine.
  - **Screenshots come from `PrintWindow` with `PW_RENDERFULLCONTENT`**, falling
    back to `BitBlt`.
- **`tools/ghost_client.py`** — the Python client. The control plane is a pipe,
  so the transport is `open(pipe, "r+b")`, a JSON line each way.
- **`tools/serve_check.py`** — end-to-end acceptance for the control plane: a
  local page reports its own state through `aria-label`, which the accessibility
  tree exposes, so the assertions measure what the page actually saw.
- **Browser branding.** The shim is already inside the browser process, so it
  hooks `SetWindowTextW`/`SetWindowTextA` to replace the engine's product name,
  sets the process AppUserModelID to `unknowbrowser.Ghost Browser`, and applies
  the project icon via `WM_SETICON` plus `SetClassLongPtrW(GCLP_HICON)` (so later
  windows inherit it). Title bars and the taskbar read `Ghost Browser`.
- **`--test-type` by default.** Chromium's "unsupported command-line flag"
  infobar is both a visible automation tell and 56 px of lost viewport height. It
  is suppressed unless `--sandbox` is requested.
- **`tools/make_icon.py`** and `native/assets/ghost.ico` — the icon is generated
  from geometry at 8x supersampling, and embedded in both `ghost.exe` and
  `ghost_shim.dll` so either can show it.

### Fixed

- **UI Automation bounding rectangles were decoded wrongly.** The
  `UIA_BoundingRectanglePropertyId` quadruple is `[left, top, width, height]`,
  not `[left, top, right, bottom]`; every element had a negative width.
- **`activate_window` could not take the foreground.** `AttachThreadInput` alone
  loses to the Windows foreground lock; a synthetic ALT press makes the calling
  thread the last-input thread, after which `SetForegroundWindow` succeeds.
- **The client raced the server's pipe.** Between `DisconnectNamedPipe` and the
  next `ConnectNamedPipe` the name briefly does not resolve, and a concurrent
  request returns `ERROR_PIPE_BUSY`. Both are normal states; the client retries
  instead of failing.
- **The brand sweep leaked icon handles.** `LoadImageW` without `LR_SHARED`
  creates a new `HICON` on every pass; the handles are now loaded once and shared,
  and the sweep is bounded by wall clock rather than by iteration count.
- **`shutdown` left the browser running.** Chromium locks its `--user-data-dir`
  exclusively, so a browser left behind made the *next* run's browser exit
  immediately and present no window — which surfaced as "the browser has no
  visible window yet", several layers from the cause. `shutdown` now closes the
  browser it started (`WM_CLOSE`, then termination if it will not go). An
  `--attach` session owns nothing and is left alone.
- **A dead control plane took five minutes to report.** The client's connection
  retry shared the request timeout, so a request against a stopped server hung
  for the full 300 s. Connecting is now retried for 5 s; a successful connect
  still leaves `readline` blocking for as long as a slow command needs.
- **`cookie_db()` needed the control plane.** It asked the server for the profile
  path, which fails by definition once the browser has been stopped for the
  cookie read. The path can now be passed in, and `serve_check` resolves it
  before stopping.
- **Cookies could not be read at all.** Three separate causes, all now handled:
  the database is locked exclusively while the browser runs; session cookies are
  never persisted; and Chrome 154 leaves the `value` column empty, storing
  `b"v10"` + AES-256-GCM instead. The client decrypts via `crypt32`/`bcrypt` with
  no third-party dependency. See Findings.

### Findings

- **Chrome holds `Default/Network/Cookies` with no sharing at all.**
  `CreateFileW` fails with `ERROR_SHARING_VIOLATION` (32) under every share mode,
  while other files in the same directory tree open normally, so this is neither
  an ACL nor a sandbox effect. Reading cookies while the browser runs is
  impossible by design; harvesting has to happen after shutdown, which is what
  `ghost_client.cookies()` does.
- **Chrome 154 does not store cookie values in the `value` column.** It is always
  empty; the real bytes live in `encrypted_value` as `b"v10"` followed by
  AES-256-GCM over `nonce(12) || ciphertext || tag(16)`. The key is in
  `<data_dir>/Local State` at `os_crypt.encrypted_key` — base64 of `b"DPAPI"`
  plus a DPAPI-wrapped 32-byte AES key, with no `app_bound_encrypted_key`, so
  this is the pre-App-Bound scheme. The decrypted plaintext is **32 bytes of
  domain binding followed by the value**: every `.google.com` row shares one
  32-byte prefix, and `ghost_check=ok` decrypts to 32 opaque bytes then `ok`.
  A cookie with no `max-age` or `expires` never reaches the database at all.
- **`navigator.deviceMemory` is not clamped to 8 GiB on Chrome 154.** A profile
  claiming 32 GiB reports 32, so the assertion, not the spoof, was wrong.

### Known gaps

- `chrome://version`, the on-disk `chrome.exe` name and the window class
  `Chrome_WidgetWin_1` still say Chrome. They are compiled into the engine and
  cannot be changed at runtime.
- Canvas hashing, audio fingerprinting and font metrics are still measured from
  the real machine.
- Windows x64 only.

## [0.2.0] - 2026-10-06

The slice becomes something you can hand to a person. `ghost.exe` is a single
self-contained executable: it locates the browser, invents a coherent machine
and locale profile, and injects its own native layer, with no profile file, no
DLL and no install step to explain.

### Added

- **`ghost.exe`** — one file, no dependencies. It carries `ghost_shim.dll` as an
  `RCDATA` resource and extracts it on first use into
  `%LOCALAPPDATA%\GhostBrowser\engine\<fnv1a64-of-contents>\`. The directory is
  named after the resource hash, so a rebuilt shim can never be shadowed by a
  stale extracted copy. Subcommands: `browse`, `run`, `profile new|show|list`,
  `selftest`, `doctor`, `install`/`uninstall`, `version`, `help`.
- **`ghost.exe` is its own probe target.** `selftest` injects the shim into a
  copy of `ghost.exe` running a hidden `__probe` subcommand, which prints the
  same report as the standalone `probe.exe` — the report lives in
  `native/common/probe_report.cpp` so there is exactly one definition of what
  counts as spoofed. This keeps the payload at a single extracted file: writing a
  second unsigned executable into `%LOCALAPPDATA%` and launching it is precisely
  the behaviour Defender's heuristics quarantine (see below).
- **Coherent profile generation** — `ghost profile new` picks one of twelve
  machine presets (RTX 3060/4060/4070/4080, RX 6600/6700 XT, UHD 630, Iris Xe,
  …) and derives the rest from it: `screen_width x device_pixel_ratio` is a real
  panel resolution, the WebGL renderer carries the matching PCI device id, the
  timezone key resolves through `EnumDynamicTimeZoneInformation` so DST rules
  come from Windows rather than from the generator, and the user agent's Chrome
  version is re-aligned to the installed browser on every launch.
- **`tools/product_check.py`** — grades the shipped executable end to end: it
  drives `ghost browse` against the detection page and reuses the harness's
  assertions, so the product and the development harness cannot drift apart.
- **`tools/verify_ghost.ps1`** — proves the single-file claim by copying
  `ghost.exe` alone into an empty directory, wiping the extraction cache and
  running `selftest` there. Wired into CI and into the release workflow.
- **`.github/workflows/release.yml`** — tag pushes build, verify and publish
  `ghost.exe` plus `SHA256SUMS.txt`, with notes generated from this changelog by
  `tools/release_notes.py`.
- `ghost install` puts the executable on `PATH` for the current user.

### Fixed

- **The launcher hung its caller.** `CreateProcess` was called with
  `bInheritHandles = TRUE` unconditionally, so Chrome inherited the caller's
  stdout pipe and kept it open for its whole lifetime; `subprocess.communicate()`
  and `Start-Process -Wait` never returned. The comment justifying it was also
  wrong — the child's environment comes from `lpEnvironment`, which is
  independent of handle inheritance. Handle inheritance is now opt-in
  (`LaunchOptions::forward_stdio`), off for `browse`.
- **`ghost browse <url>` launched the browser with no URL.** The argument parser
  files the first positional argument after the subcommand into the subcommand
  slot, and `browse` only forwarded the remaining ones.
- **The harness hardcoded Europe/London.** Every timezone and locale assertion
  expected GMT/BST and `1,234,567.891`, so a correct spoof of any other zone was
  reported as a failure. Expectations are now derived from the profile through
  `zoneinfo`, including the sign flip between the tz database's UTC−local and
  JavaScript's local−UTC, and both `CST` and `GMT+8` spellings are accepted.
- **`rc.exe` mangled the embedded resource paths.** It processes backslash
  escapes inside string literals, so `\native` became a newline and the build
  failed with `error RC2135: file not found`. Paths are now written with forward
  slashes.

### Findings

- **`navigator.deviceMemory` is not clamped to 8 GiB.** The Device Memory API is
  specified to clamp to `[0.25, 8]` and the harness assumed it. Measured against
  profiles of 4 GiB and 32 GiB, Chrome 154 returned 4 and 32 — the profile value
  verbatim. The check now compares against the profile directly, which also makes
  it a real test rather than one that any host satisfies.
- **Extracting a second executable is what antivirus objects to.** Defender
  quarantined `%LOCALAPPDATA%\GhostBrowser\engine\<hash>\probe.exe` as
  `Behavior:Win32/DefenseEvasion.A!ml` (threat id `2147738096`), after which
  `CreateProcess` failed with `225` (`ERROR_VIRUS_INFECTED`) and `selftest`
  reported `1/9`. Extracting and running an unsigned executable is the pattern
  the heuristic targets; loading a DLL into an already-running process is not.
  That is why the probe was folded into `ghost.exe` itself.

### Known gaps

- Same as 0.1.0: sandboxed renderers are unreachable (so `browse` passes
  `--no-sandbox` by default, which is documented in `ghost help`), canvas,
  audio and font metrics are unspoofed, and Linux/macOS are unimplemented.
- `ghost.exe` is unsigned, and Defender flags it heuristically as
  `Behavior:Win32/DefenseEvasion.A!ml` because creating a suspended process,
  writing a DLL path into it and starting a remote thread is what an injector
  does. `ghost.exe` now detects the resulting `225` and says so instead of
  failing silently. This is documented in `docs/USAGE.md` and in the release
  notes.

## [0.1.0] - 2026-10-06

First working vertical slice, Windows/x64. A profile file drives a launcher
that injects a native shim into Chromium **before the engine caches any
value**, so nothing is patched in JavaScript, there is no script layer for a
page to find, and there is no CDP endpoint at all.

### Added

- **`ghost_shim.dll`** — a native shim injected into the target process at
  `CREATE_SUSPENDED`, before Chromium initialises its sandbox or caches any
  system fact. Installs 24 hooks and propagates itself into every child
  process. Covers:
  - processor count (`GetNativeSystemInfo`, `GetActiveProcessorCount`,
    `GetSystemInfo`) and physical memory (`GlobalMemoryStatusEx`)
  - screen geometry and DPI (`EnumDisplayMonitors`, `GetMonitorInfoW`,
    `GetSystemMetrics`, `GetDeviceCaps`, `GetDpiForMonitor`, `GetDpiForSystem`)
  - time zone (`GetTimeZoneInformation`, `GetDynamicTimeZoneInformation`)
    and locale (`GetUserDefaultLocaleName`)
  - WebGL vendor/renderer via the DXGI adapter (`CreateDXGIFactory{,1,2}` plus
    the adapter vtable's `GetDesc`/`GetDesc1`)
  - child-process creation (`CreateProcessW`, `CreateProcessAsUserW`,
    `CreateProcessInternalW`) for shim propagation
- **`ghost_launch.exe`** — profile-driven launcher. Injects the shim and
  refuses to run an unspoofed browser unless `--allow-unspoofed` is passed.
- **`probe.exe`** — prints the real return values of every hooked API, so the
  spoofing can be asserted from outside the browser and used as a CI smoke test.
- **`harness/detect.html` + `harness/run_detect.py`** — ground-truth fingerprint
  measurement taken *inside the renderer* and POSTed to a local collector
  (the only honest way to measure without CDP). 34 checks, 0 failed.
- **`native/tests/probe`**, `tools/pe_exports.py`, `tools/token_sids.ps1`,
  `tools/mitigations.ps1` — the diagnostic tooling the findings below came from.
- CI: MSVC build plus a probe smoke test with a negative control, and a lint job.
- BSD-3-Clause licensing and third-party notices (MinHook is vendored, BSD-2).

### Findings

Three results that changed the architecture, all recorded in
`docs/ARCHITECTURE.md` §10–§11:

- **`libGLESv2.dll` no longer exists.** In Chrome 154 ANGLE is statically
  linked into `chrome.dll` (302 MB), and Google ships no PDB, so the planned
  "hook the GL exports" channel was dead on arrival. WebGL is instead spoofed
  by patching the **DXGI adapter vtable**, which needs no `chrome.dll` reverse
  engineering. ANGLE then composes the renderer string itself, so it stays
  internally consistent: `ANGLE (NVIDIA, NVIDIA GeForce RTX 3060
  (0x00002504) Direct3D11 vs_5_0 ps_5_0, D3D11)`.
- **Sandboxed renderers cannot be injected.** Their restricted token's only
  restricted SID is `S-1-0-0` and `LoadLibraryW` returns `STATUS_ACCESS_DENIED`
  (`0xC0000022`). The GPU process is created through `CreateProcessW` rather
  than `CreateProcessAsUserW`, so `--disable-gpu-sandbox` makes it injectable
  while the renderer sandbox stays on.
- **`navigator.hardwareConcurrency` comes from `GetNativeSystemInfo`**, not
  `GetActiveProcessorCount` — a long-standing misdiagnosis, confirmed by
  reading `base/win/windows_version.cc`.

### Known gaps

- Sandboxed renderer processes are structurally unreachable, so
  `hardwareConcurrency` and `deviceMemory` still report host values there.
  The harness reports these as `GAP`, not `PASS` — they are not verified.
- Canvas `getImageData`/`toDataURL`, `OfflineAudioContext` and font metrics go
  through pure Blink/Skia/DSP paths with no exported symbols. They need a
  source-level patch set (Track B); Track A cannot reach them.
- Linux and macOS are designed (`docs/ARCHITECTURE.md` §8) but not implemented.
- `harness/run_detect.py` is flaky (roughly a 50% chance of `NO REPORT
  RECEIVED` from a Chrome startup race) and is deliberately not wired into CI.

[0.3.0]: https://github.com/Mouseww/ghost-browser/releases/tag/v0.3.0
[0.2.0]: https://github.com/Mouseww/ghost-browser/releases/tag/v0.2.0
[0.1.0]: https://github.com/Mouseww/ghost-browser/releases/tag/v0.1.0
