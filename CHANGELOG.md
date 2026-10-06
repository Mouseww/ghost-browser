# Changelog

All notable changes to this project are recorded here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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
