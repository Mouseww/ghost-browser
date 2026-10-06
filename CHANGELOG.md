# Changelog

All notable changes to this project are recorded here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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

[0.2.0]: https://github.com/Mouseww/ghost-browser/releases/tag/v0.2.0
[0.1.0]: https://github.com/Mouseww/ghost-browser/releases/tag/v0.1.0
