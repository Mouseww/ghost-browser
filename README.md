# Ghost Browser

An agent-native browser: instead of driving a stock browser and injecting JavaScript to
hide the automation, this project changes what the **engine itself** reports. The browser
is a real Chromium; the fingerprint it presents is synthesized before the engine ever
caches a value, so there is no script layer for a detector to find.

This repository currently contains **Track A — the native shim vertical slice**: a working
end-to-end path from a profile file to a launched browser that passes a 34-assertion
fingerprint coherence harness, including WebGL vendor/renderer spoofing.

**Just want to run it?** The [v0.1.0 release](https://github.com/Mouseww/ghost-browser/releases/tag/v0.1.0)
ships prebuilt binaries and a sample profile, and **[docs/USAGE.md](docs/USAGE.md)** walks
through using them — the 30-second probe check, launching a real browser through the shim,
what each profile field means, and what is deliberately not spoofed. Cloning and building
are only needed for the fingerprint test page under `harness/`.

---

## Status

| Area | State |
|---|---|
| Profile → engine flag/API spoofing | **working** |
| Native shim (26 hooks, Win32/x64) | **working** |
| Injection into the browser process | **working** |
| Injection into the GPU process | **working** (needs `--disable-gpu-sandbox`) |
| Injection into sandboxed renderers | **blocked by the restricted token** |
| WebGL vendor / renderer (`UNMASKED_*`) | **working** — via DXGI adapter identity |
| Zero-CDP control plane (`ghost serve`) | **working** — 15 checks |
| Window branding (title, icon, taskbar) | **working** |
| Reading cookies while the browser runs | **impossible** — Chrome holds the file unshared |
| Fingerprint coherence harness | **34 checks, 0 failed** |
| Canvas / Audio / font *metrics* | **not spoofed** (Track A gap) |
| Captcha pipeline (CF, hCaptcha) | **not implemented** |
| Linux / macOS | **not implemented** |

`python harness/run_detect.py --sandboxed --chrome-arg=--disable-gpu-sandbox` currently prints:

```
34 checks, 0 failed
```

Two checks are reported as `GAP` rather than `PASS` or `FAIL`: with the renderer sandbox on,
`hardwareConcurrency` and `deviceMemory` are structurally unreachable (see limitation 1), so
the harness refuses to score them either way. A `GAP` is not a pass — it is an explicit
"cannot be measured here", which is why it does not appear in the pass count.

---

## Quick start (Windows, x64)

**The product is one file.** Download `ghost.exe` from the
[latest release](https://github.com/Mouseww/ghost-browser/releases/latest) — no
clone, no build, no DLLs, no profile to author:

```powershell
.\ghost.exe selftest        # prove injection works on this machine (9/9)
.\ghost.exe browse https://example.com
```

`ghost.exe` finds your Chrome (or Edge), generates a coherent machine profile
from a seed, extracts its embedded shim into a content-addressed cache, and
starts the browser suspended-then-injected. See [docs/USAGE.md](docs/USAGE.md)
for the full command surface, the profile fields, and troubleshooting.

### From source

Prerequisites: Visual Studio Build Tools (MSVC x64), Python 3.11, CMake.

```powershell
python -m pip install cmake
python -m cmake -S native -B native/build -G "Visual Studio 17 2022" -A x64
python -m cmake --build native/build --config Release

# grade the built single file exactly as a user would run it
python tools/product_check.py

# grade the shim through the developer launcher, renderer sandbox on
Get-Process chrome -ErrorAction SilentlyContinue | Stop-Process -Force
python harness/run_detect.py --sandboxed --chrome-arg=--disable-gpu-sandbox
```

Artifacts land in `native/build/bin/`: `ghost.exe` (the product),
`ghost_shim.dll`, `ghost_launch.exe` (the developer launcher) and `probe.exe`.

> **Always stop Chrome before rebuilding.** A running Chrome holds `ghost_shim.dll` open
> and the link step fails with `LNK1104: cannot open file 'ghost_shim.dll'`.

`run_detect.py` defaults to `--no-sandbox`, which is the only way to reach the renderer-side
hooks; pass `--sandboxed` to keep the renderer sandboxed (those two checks then report `GAP`).

> **The harness is currently flaky.** Roughly half of runs end in `NO REPORT RECEIVED` with
> no crash event in the Windows event log, which points at a Chrome startup race rather than
> a defect in the shim. Re-running it passes. This is why it is deliberately not wired into
> CI — see [docs/CI.md](docs/CI.md). `tools/product_check.py` grades the same assertions
> through the product and is the one CI runs.

### Driving the shim directly (development)

```powershell
native\build\bin\ghost_launch.exe `
  --profile harness\profiles\slice-test-001.json `
  --tz Europe/London --lang en-GB --no-wait -- `
  "C:\Program Files\Google\Chrome\Application\chrome.exe" `
  --user-data-dir=E:\tmp\ghost --disable-gpu-sandbox --lang=en-GB about:blank
```

Use `--no-sandbox` instead of `--disable-gpu-sandbox` if you also need the renderer-side
values (`hardwareConcurrency`, `deviceMemory`); that gives up the renderer sandbox entirely.
`ghost.exe browse` makes that trade-off for you and documents it in `ghost help`.

### Driving the browser without CDP

`ghost serve` puts the browser behind a JSON control plane carried over a **named pipe** —
never a TCP port, because a page can scan ports and cannot scan pipes:

```powershell
.\ghost.exe serve --id demo --pipe demo --tz Europe/London --locale en-GB https://example.com
```

```python
from tools.ghost_client import Ghost

with Ghost("demo").start(url="https://example.com") as browser:
    browser.wait_for(role="button", name="Accept")
    browser.click(role="button", name="Accept")   # synthesized, so isTrusted is true
    print(browser.tree(max_nodes=40))             # read through the accessibility tree
    browser.screenshot("shot.bmp")                # PrintWindow, not a debugging protocol
```

There is no `Runtime.enable`, no injected utility script, and no port for a page to find. Input
is `SendInput` — interpolated mouse paths rather than teleports, 35–90 ms press durations, and
`KEYEVENTF_UNICODE` typing that ignores the keyboard layout. The page is read through UI
Automation, and screenshots come from `PrintWindow`. `python tools/serve_check.py` grades all of
it against a local page that reports its own state through `aria-label` — **17 checks, 0 failed**,
including that every event the page received was `isTrusted`.

Cookies are the one thing that cannot be read live: Chrome holds `Default/Network/Cookies` with
**no sharing at all** (`ERROR_SHARING_VIOLATION` under every share mode), so
`ghost_client.cookies()` harvests them after the browser exits. Reading them then takes two more
steps — session cookies never reach the database, and the `value` column is always empty because
Chrome 154 stores `b"v10"` + AES-256-GCM in `encrypted_value`, with the key DPAPI-wrapped in
`Local State`. The client undoes all of it with `crypt32` and `bcrypt` through `ctypes`, so there
is still no third-party dependency.

### The window says "Ghost Browser"

The shim already lives inside the browser process, so it rebrands the window from within: it
hooks `SetWindowTextW`/`SetWindowTextA` to replace the engine's product name, sets the process
AppUserModelID to `unknowbrowser.Ghost Browser` for taskbar grouping, and applies the project
icon with `WM_SETICON` plus `SetClassLongPtrW(GCLP_HICON)` so later windows inherit it.

`--test-type` is passed by default: Chromium's "unsupported command-line flag" infobar is both a
visible automation tell and 56 px of stolen viewport.

Three things still say Chrome, and cannot be changed at runtime because they are compiled into
the engine: `chrome://version`, the on-disk file name `chrome.exe`, and the window class
`Chrome_WidgetWin_1`.

The launcher starts the target `CREATE_SUSPENDED`, injects the shim, waits for a real
in-process readiness handshake, and only then resumes it. **If injection fails it aborts
rather than continuing with an unspoofed browser** — pass `--allow-unspoofed` to override.

---

## How it works

```
profile.json ──► ghost_launch ──► CreateProcessW(CREATE_SUSPENDED)
                     │                    │
                     │                    ├─ inject ghost_shim.dll
                     │                    ├─ wait for in-process ready handshake
                     │                    └─ ResumeThread
                     ▼
            GHOST_PROFILE_JSON (inherited by every child)
                     │
   ┌─────────────────┴──────────────────┐
   ▼                                    ▼
browser process                     child processes
  hook OS APIs                        CreateProcessW hook injects the
  before the engine                   shim into each child before it runs
  caches any value
```

Four deliberate design choices:

1. **Zero CDP.** No `--remote-debugging-port`, no `Runtime.enable`, no injected utility
   script. Those are the artifacts that actually get Playwright/Puppeteer caught — not
   `navigator.webdriver`. Control will come from OS input synthesis and the accessibility
   tree instead.
2. **Spoof at the OS API, not in JavaScript.** `navigator.hardwareConcurrency` is read
   from `GetNativeSystemInfo().dwNumberOfProcessors`; we hook that call. A page can call
   `Function.prototype.toString` on anything it likes and still see `[native code]`,
   because nothing was patched in JavaScript.
3. **Configuration travels by environment variable.** `GHOST_PROFILE_JSON` is inherited by
   every child automatically, so no IPC channel is needed just to distribute the profile.
4. **For WebGL, patch the identity source, not the graphics API.** See below.

The readiness handshake deserves a note: it is done with a remote thread that calls an
export inside the already-injected shim. It deliberately does **not** use a named event or
a remote stub — the shim runs inside sandboxed children whose restricted token cannot open
a named kernel object, and a remote stub's `LoadLibraryW` is subject to the same restriction.

### The WebGL channel: ANGLE is statically linked

The original plan was to hook the GL entry points in `libGLESv2.dll`. **That DLL no longer
exists.** In Chrome 154 (`154.0.8037.98`) it is not in the install directory at all; searching
`chrome.dll` (302 MB) for ASCII markers gives `libGLESv2` = 0 hits while `EGL_` = 546 and
`GL_ANGLE` = 202. ANGLE is linked straight into `chrome.dll`, so there is no export table to
hook and no PDB to recover symbols from.

The channel that does work is one level down. ANGLE builds its `GL_RENDERER` string itself
from the DXGI adapter it selects:

```
ANGLE (NVIDIA, NVIDIA GeForce RTX 3060 (0x00002504) Direct3D11 vs_5_0 ps_5_0, D3D11)
                └── IDXGIAdapter::GetDesc().Description      └── DeviceId
```

The adapter is a COM object living in `dxgi.dll`, whose vtable can be patched without any
`chrome.dll` reverse engineering. The shim hooks `CreateDXGIFactory{,1,2}`, patches
`EnumAdapters1` in the returned factory's vtable, and rewrites `Description`, `VendorId`,
`DeviceId` and `DedicatedVideoMemory` in `GetDesc`/`GetDesc1`. ANGLE then composes a string
that is internally consistent — correct vendor prefix, correct `(0x…)` suffix, correct
feature level — because the *real* ANGLE code produced it.

The hooks are installed lazily: `dxgi.dll` is not loaded when the shim attaches, so the
shim hooks `ntdll!LdrLoadDll` and installs the DXGI hooks at the moment the module appears.

---

## Verified behaviour

Profile (`harness/profiles/slice-test-001.json`) vs. what the page actually observed, from a
fresh Chrome launched by `ghost_launch`:

| Probe | Profile | Observed | |
|---|---|---|---|
| `navigator.hardwareConcurrency` | 4 | 4 | pass |
| `navigator.deviceMemory` | 8 | 8 | pass |
| `screen.width × height` | 2560×1440 | 2560×1440 | pass |
| `screen.availHeight` | 1400 | 1400 | pass |
| `window.devicePixelRatio` | 1.25 | 1.25 | pass |
| `screen.width × DPR` == physical | 3200 | 3200.0 | pass |
| `Intl.DateTimeFormat().resolvedOptions().timeZone` | Europe/London | Europe/London | pass |
| January UTC offset / July UTC offset | 0 / −60 | 0 / −60 | pass |
| Default-locale month abbreviation | `GMT` / `BST` | `GMT` / `BST` | pass |
| `navigator.language` | en-GB | en-GB | pass |
| `navigator.webdriver` | false | false | pass |
| Automation globals (`cdc_*`, `__playwright__`, …) | none | none | pass |
| CDP ports 9222/9223/9229/9515 | closed | all closed | pass |
| Hooked getters still report `[native code]` | yes | yes | pass |
| `UNMASKED_VENDOR_WEBGL` | NVIDIA | NVIDIA | pass |
| `UNMASKED_RENDERER_WEBGL` contains adapter description | RTX 3060 | RTX 3060 | pass |
| …and the matching device id | `(0x00002504)` | `(0x00002504)` | pass |

The harness asserts the adapter description and the `(0x…)` device id **separately** rather
than matching one hardcoded literal, so a change in ANGLE's string format cannot silently
turn the check into a false pass.

---

## Known limitations

These are measured, not speculative.

**1. Chromium's renderer sandbox defeats injection — `--no-sandbox` is currently required
for renderer-side hooks.**
A sandboxed renderer runs under an Untrusted-integrity restricted token whose restricted SID
list is `[S-1-0-0]` (the NULL SID, which no ordinary ACE can grant). `LoadLibraryW` of the
shim inside such a process returns `STATUS_ACCESS_DENIED` (`0xC0000022`). This was confirmed
against every alternative explanation: signature policy (`any` on all children), AppContainer
ACLs, mandatory integrity labels (set to both Untrusted and Low), NULL-SID grants, path and
traverse rights, and all 16 process mitigation policies (none enabled). With the sandbox off,
the shim loads in the browser **and** in every renderer.

Consequence: `hardwareConcurrency` and `deviceMemory` need `--no-sandbox`. Screen, DPR,
timezone and locale do **not** — those are computed in the browser process and inherited by
the renderer, so they work with the sandbox on.

The GPU process is different and is the useful discovery here: Chromium creates it with
`CreateProcessW`, **not** `CreateProcessAsUserW`, so `--disable-gpu-sandbox` alone makes it
injectable while renderers stay sandboxed. That is what makes the WebGL channel usable
without giving up the renderer sandbox.

The correct fix for the renderer side is Track B: patch the engine at build time so nothing
needs injecting. Track A is the verifier and the transition form.

**2. Canvas, Audio and font *metrics* are still the host's.**
These live entirely inside Blink/Skia/DSP code with no exported symbol to hook, and the
official Chrome build ships no PDBs. They are reported as `INFO` by the harness, not `PASS`.
This is now the largest remaining detection surface — WebGL used to be on this list and no
longer is.

**3. Windows Defender flags the tooling.** `ghost_launch.exe` was quarantined as
`Behavior:Win32/DefenseEvasion.A!ml` after an `icacls /setintegritylevel` experiment (which
turned out to fix nothing). A clean rebuild is not re-flagged. Shipping this requires code
signing and a documented exclusion path; CI excludes only its own build-output directory.

**4. The harness is not a detection suite.** `harness/detect.html` measures coherence
between the profile and what the page sees. It is not a substitute for running against real
detection services.

**5. Windows only.** The shim is Win32/x64. The Linux (`LD_PRELOAD`) and macOS
(`DYLD_INSERT_LIBRARIES` + ad-hoc re-sign) paths are designed in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) but not implemented.

---

## Layout

```
native/
  common/inject.h          injection, PE export parsing, remote loader-list walk
  ghost_shim/              the DLL that is injected into the browser
    src/hooks_sysinfo.cpp    CPU count, memory
    src/hooks_display.cpp    screen geometry, DPI
    src/hooks_time.cpp       timezone, locale, registry
    src/hooks_gpu.cpp        DXGI adapter identity (feeds ANGLE's WebGL strings)
    src/hooks_proc.cpp       child-process propagation
    src/hook_engine.*        MinHook wrapper
  ghost_launch/            the launcher and injector
  tests/probe/             ground-truth value dumper (run with and without the shim)
harness/
  detect.html              in-renderer fingerprint collector
  run_detect.py            orchestrator + assertion table
  profiles/                profile files
tools/
  pe_exports.py            dependency-free PE export-table parser
  token_sids.ps1           process token / integrity / restricted-SID dumper
  mitigations.ps1          process mitigation policy dumper
docs/
  ARCHITECTURE.md          full design, incl. the Track B plan
  CI.md                    what runs on free runners vs. a self-hosted one
  superpowers/plans/       the implementation plan
.github/workflows/ci.yml   MSVC build + probe smoke test (with a negative control)
```

## License

BSD-3-Clause — see [`LICENSE`](LICENSE). Third-party components are listed in
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md); note in particular that this repository
does **not** redistribute Chromium or Chrome.

## Compliance

Intended for privacy protection, ad verification, price monitoring, public-data collection,
web-app QA, automated testing and accessibility automation. Users are responsible for
complying with the target site's terms of service and applicable law (CFAA, GDPR, and
platform anti-automation policies).

This project is not intended for, and must not be used for, bypassing authentication,
bulk fake account registration, fraud, or unauthorized access.
