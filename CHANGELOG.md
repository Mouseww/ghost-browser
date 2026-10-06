# Changelog

All notable changes to this project are recorded here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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

[0.1.0]: https://github.com/Mouseww/ghost-browser/releases/tag/v0.1.0
