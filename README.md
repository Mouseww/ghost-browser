# Ghost Browser

An agent-native browser: instead of driving a stock browser and injecting JavaScript to
hide the automation, this project changes what the **engine itself** reports. The browser
is a real Chromium; the fingerprint it presents is synthesized before the engine ever
caches a value, so there is no script layer for a detector to find.

This repository currently contains **Track A — the native shim vertical slice**: a working
end-to-end path from a profile file to a launched browser that passes a 34-assertion
fingerprint coherence harness, including WebGL vendor/renderer spoofing.

**Just want to run it?** Download `ghost.exe` from the
[latest release](https://github.com/Mouseww/ghost-browser/releases/latest) — that one file
is the whole product, with nothing to install and no companion DLLs, profiles or config to
place beside it. Run `ghost.exe selftest` to prove it works on your machine, then
`ghost.exe browse https://example.com`. **[docs/USAGE.md](docs/USAGE.md)** walks through the
rest: the 30-second probe check, launching a real browser through the shim, what each
profile field means, and what is deliberately not spoofed. Cloning and building are only
needed to work on the source or run the fingerprint test page under `harness/`.

---

## Status

| Area | State |
|---|---|
| Profile → engine flag/API spoofing | **working** |
| Native shim (27 hooks, Win32/x64) | **working** |
| Injection into the browser process | **working** |
| Injection into the GPU process | **working** (needs `--disable-gpu-sandbox`) |
| Injection into sandboxed renderers | **blocked by the restricted token** |
| WebGL vendor / renderer (`UNMASKED_*`) | **working** — via DXGI adapter identity |
| Font enumeration (`document.fonts`, `measureText`) | **working** — DirectWrite collection filtered |
| Control plane (`ghost serve`) | **working** — 19 checks, named pipe, no TCP port |
| DevTools channel (anonymous pipe, on by default) | **working** — no port, no `DevToolsActivePort` in the profile, `navigator.webdriver` still false |
| Working with no foreground window | **working** — falls back to UI Automation and says which channel it used |
| Reading a human-verification challenge | **working** — 35 checks |
| Waiting for a challenge without touching it | **working** — `captcha action=wait` |
| Clearing Cloudflare Turnstile | **working** — one click answers it |
| hCaptcha / reCAPTCHA image challenges | **answered by a solving service** — `captcha action=solve-token` supplies the DOM write, not the recognition |
| reCAPTCHA audio challenge | **answered two ways** — from the clip's own URL when a service is configured, otherwise recorded and transcribed |
| hCaptcha audio challenge | **not driven** — a measured dead end: its menu button ignores UI Automation |
| Window branding (title, icon, taskbar) | **working** |
| Reading cookies while the browser runs | **impossible** — Chrome holds the file unshared |
| Fingerprint coherence harness | **34 checks, 0 failed** |
| Canvas / Audio | **not spoofed** (Track A gap — no OS API to hook) |
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
it against a local page that reports its own state through `aria-label` — **19 checks, 0 failed**,
including that every event the page received was `isTrusted`.

Cookies are the one thing that cannot be read live: Chrome holds `Default/Network/Cookies` with
**no sharing at all** (`ERROR_SHARING_VIOLATION` under every share mode), so
`ghost_client.cookies()` harvests them after the browser exits. Reading them then takes two more
steps — session cookies never reach the database, and the `value` column is always empty because
Chrome 154 stores `b"v10"` + AES-256-GCM in `encrypted_value`, with the key DPAPI-wrapped in
`Local State`. The client undoes all of it with `crypt32` and `bcrypt` through `ctypes`, so there
is still no third-party dependency.

### Installing it on your PATH

`ghost.exe` runs from wherever you put it, but `ghost install` copies it to
`%LOCALAPPDATA%\GhostBrowser\bin` and prepends that directory to your user `PATH`
(`HKCU\Environment\Path`), so `ghost` works from any shell. It broadcasts
`WM_SETTINGCHANGE`, so shells that are already open may need a restart to see it.
`ghost uninstall` reverses both steps. That is the entire installation: one file, one
user environment variable. No service, no scheduled task, no registry key beyond that
`PATH` entry, and no administrator rights.

### Using it from your language

The control plane is a Windows named pipe carrying one JSON object per line, so a
client is about sixty lines in any language that can open a file: write a request
followed by `\n`, read until `\n`. [docs/PROTOCOL.md](docs/PROTOCOL.md) is the full
specification, and [examples/](examples) has a client that was compiled and run
against a live browser for each language:

| Language | Client | Run it |
|---|---|---|
| Python | [tools/ghost_client.py](tools/ghost_client.py) | `python tools/serve_check.py` |
| C# / .NET | [examples/dotnet](examples/dotnet) | `dotnet run --project examples/dotnet` |
| Go | [examples/go](examples/go) | `go run examples/go/ghost.go` |
| Rust | [examples/rust](examples/rust) | `cargo run --manifest-path examples/rust/Cargo.toml` |
| Node.js | [examples/node](examples/node) | `node examples/node/example.js` |

If you would rather not take on a dependency at all, every language can shell out to
the one-shot client instead:

```bash
ghost call '{"cmd":"navigate","url":"https://example.com"}' --id demo
ghost call '{"cmd":"tree","max_nodes":50}' --id demo
```

`ghost call` opens the pipe, sends one request, prints one JSON reply and exits — no
long-lived process, no library, no build step. That path works from a shell script, a
Makefile, or a CI job as readily as from an application.

### Using it from an agent

Two pieces, because they answer different questions.

**The MCP server** ([mcp/ghost_mcp.py](mcp/ghost_mcp.py)) supplies the capability:
fourteen tools covering open, navigate, read, find, wait, click, type, key, scroll,
captcha, a raw DevTools call and close. It is a single standard-library Python file,
it owns the browser's lifecycle, it renders the accessibility tree as text an LLM can
act on (`[14] button "Sign in" @144,256`), and it converts screenshots to PNG because
MCP image content does not carry BMP.

```bash
claude mcp add ghost -- python /absolute/path/to/mcp/ghost_mcp.py
```

[mcp/README.md](mcp/README.md) has the generic `mcpServers` JSON, the tool table and
the environment variables. `python tools/mcp_check.py` drives the whole thing the way
a client does and reports **36 checks, 0 failed**.

**The skill** ([skills/ghost/SKILL.md](skills/ghost/SKILL.md)) supplies the judgement:
when this browser is the right tool and when an HTTP client is, the
navigate → read → act → re-read loop, and the failure that otherwise looks like a bug —
synthesized input is silently discarded on a disconnected desktop session, so a click
appears to do nothing. An MCP server alone leaves an agent to rediscover that every
time.

### Human-verification challenges

The control plane's `captcha` command reads the challenge on the page and, with
`action=solve` (the default), clicks it. It needs no CDP and no injected script, because
the accessibility tree already sees inside
the challenge's cross-origin iframe — and a challenge frame's document node carries its
own URL as its `value`, which is where the sitekey lives:

```bash
ghost call '{"cmd":"captcha","action":"detect"}' --id agent
```
```json
{"ok": true, "provider": "hcaptcha", "state": "checkbox",
 "site_key": "a5f74b19-9e45-40e0-b45d-47ff91b7a6c2",
 "page_url": "https://accounts.hcaptcha.com/demo",
 "frame_url": "https://newassets.hcaptcha.com/captcha/v1/.../hcaptcha.html#frame=checkbox&id=...",
 "detail": "the widget is showing its checkbox"}
```

Measured against the three real challenges (`python tools/captcha_check.py` → **35 checks,
0 failed, 0 not measurable**):

| Challenge | What the browser does |
|---|---|
| Cloudflare Turnstile | **solves it** — one click, the widget goes |
| hCaptcha | reads the sitekey, clicks, opens the image challenge |
| reCAPTCHA v2 | reads the sitekey, clicks, opens the challenge; the audio challenge is reachable, recorded and answered |

The audio route has two forms. On the machine it records what the browser **actually
played** through a WASAPI loopback of the output device, presses the challenge's own play
control (the challenge does not start itself), transcribes the digits and types them
back — nothing is downloaded and no URL is parsed, so it does not depend on the
challenge's internal structure. When a solving service is configured it instead reads the
clip's own address out of the page and fetches it (`captcha action=audio-url`), which
needs neither the sound card nor a recogniser. The clip usually lives in a frame with a
process of its own, which is why the page's own tree does not contain it:

```json
{"ok": true, "provider": "recaptcha", "state": "audio",
 "play": "pressed the challenge's play control",
 "device": "远程音频", "captured_seconds": 4.93,
 "peak": 0.6771, "streams": "pid 20460 active peak 0.0409",
 "solved_by": "api", "heard": "385124", "confidence": 1,
 "typed": true, "input": "synthesized", "solved": false}
```

`solved_by` says where the digits came from: `local` is the Windows speech recogniser on
this machine, `api` is a solving service — used only when no recogniser for the
challenge's language is installed **and** a key is configured (`GHOST_CAPTCHA_KEY`, or
`captcha_api_key` in the profile). There is no built-in key: with none configured the
third tier is off, not broken. The upload is down-mixed to 16 kHz mono 16-bit, about
157 KB for a five-second challenge.

An **image** challenge has no route like that: it cannot be read out of the
accessibility tree, and it cannot be answered by typing. `captcha action=solve-token`
hands it to a solving service instead — the service rebuilds the challenge from the site
key and the page URL, and the browser writes the token it returns into the page's hidden
`g-recaptcha-response` field and submits the form. That last step is a DOM write, and it
is the reason the DevTools channel exists at all; under `--no-cdp` this route is
unavailable. It reports `fields_filled` and refuses to call a token nobody received an
answer.

**One look is not a detection.** The widget animates in — on this machine Cloudflare's
interstitial exposes no challenge at all for its first ~1.2 s, and hCaptcha's checkbox
for ~1.2 s. During that window a page that is about to challenge you looks exactly like
a page that never will, so `captcha` keeps looking for up to `wait_ms` (default 3000;
pass `0` for the old single sample) before it will say `provider: none`, and reports
`appeared_ms` so you can tell a fast page from a slow one.

If you want to watch rather than act, `action=wait` clicks nothing: it reports whether a
challenge appeared, how long that took, and whether it then cleared by itself.

```json
{"ok": true, "provider": "turnstile", "state": "checkbox",
 "appeared": true, "appeared_ms": 953, "cleared": false,
 "waited_ms": 9094, "clicked": false,
 "detail": "the widget is showing its checkbox"}
```

`cleared: false` is the honest answer for that demo: it never passes on its own, so
patience is not a solution and the command does not pretend otherwise. `wait` stops
early once a challenge settles into an image or audio one, because those are waiting for
a person.

Two things are worth saying plainly. **`peak 0` has an innocent explanation** — the
audio challenge does not autoplay, and until its play control is pressed the page holds
an open stream that renders nothing, which is why `play` and `streams` are reported
alongside the level. And **do not retry in a loop**: repeated attempts are themselves a
bot signal. Ask once, report what you got, move on.

What is *not* done: recognising an image challenge here — a solving service answers it,
this browser does not — and driving hCaptcha's audio route, which goes through its
accessibility menu.

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

1. **A DevTools channel with nothing left to scan.** The pipe, not the port:
   `--remote-debugging-pipe` on anonymous handles the launcher creates and passes to the
   child, so there is no TCP port for a page to scan and no `DevToolsActivePort` file in
   the profile. It is driven with `Runtime.evaluate`, which needs no `Runtime.enable` —
   that call changes the console object from inside the page and is one of the ways a
   page detects DevTools. Opening the pipe also switches on Blink's `AutomationControlled`
   feature, which sets `navigator.webdriver` to `true`; the launcher disables that feature
   alongside the pipe that needs it. `--no-cdp` closes the channel entirely and falls back
   to OS input synthesis and the accessibility tree.
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
| `DevToolsActivePort` in the profile | absent | absent | pass |
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

**2. Canvas and Audio are still the host's.** Their pixels and samples are produced by
Skia and Blink's DSP inside the renderer and never cross an OS API, so no hook can reach
them — not a matter of effort, a matter of there being nothing to intercept. They are
reported as `INFO` by the harness, not `PASS`. Fonts used to be on this list: font
*enumeration* does cross an API (DirectWrite), so it is now spoofed, but font *metrics*
are computed by Skia from the resolved font and remain the host's.

**3. The font filter hides fonts; it cannot invent them.** The visible set is the
profile's allow-list intersected with what the OS actually has. A profile cannot make a
machine appear to have a font that is not installed, and if none of the listed families
exist the hook declines to patch rather than present an empty collection (a browser that
believes it has no fonts renders text visibly wrong, which is a worse tell than an
unfiltered list).

**4. Windows Defender flags the tooling.** `ghost_launch.exe` was quarantined as
`Behavior:Win32/DefenseEvasion.A!ml` after an `icacls /setintegritylevel` experiment (which
turned out to fix nothing). A clean rebuild is not re-flagged. Shipping this requires code
signing and a documented exclusion path; CI excludes only its own build-output directory.

**5. The harness is not a detection suite.** `harness/detect.html` measures coherence
between the profile and what the page sees. It is not a substitute for running against real
detection services.

**6. Windows only.** The shim is Win32/x64. The Linux (`LD_PRELOAD`) and macOS
(`DYLD_INSERT_LIBRARIES` + ad-hoc re-sign) paths are designed in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) but not implemented.

---

## Layout

```
native/
  common/inject.h          injection, PE export parsing, remote loader-list walk
  common/probe_report.*    the single definition of what "spoofed" means
  ghost_shim/              the DLL that is injected into the browser
    src/hooks_sysinfo.cpp    CPU count, memory
    src/hooks_display.cpp    screen geometry, DPI
    src/hooks_time.cpp       timezone, locale, registry
    src/hooks_gpu.cpp        DXGI adapter identity (feeds ANGLE's WebGL strings)
    src/hooks_dwrite.cpp     DirectWrite font collection (document.fonts, measureText)
    src/hooks_proc.cpp       child-process propagation
    src/hook_engine.*        MinHook wrapper
  ghost_cli/               the single shipped file, ghost.exe
    src/main.cpp             subcommand dispatch
    src/daemon.cpp           the control plane (`ghost serve`)
    src/cdp.cpp              the DevTools channel over anonymous pipes
    src/uia.cpp              accessibility-tree reads and the priming fix
    src/pipe.cpp             the named pipe
    src/input.cpp, window.cpp, capture.cpp   OS input, window capture
  ghost_launch/            the launcher and injector
  tests/probe/             ground-truth value dumper (run with and without the shim)
harness/
  detect.html              in-renderer fingerprint collector
  run_detect.py            orchestrator + assertion table
  profiles/                profile files
mcp/ghost_mcp.py           MCP server: the browser as tools an agent can call
skills/ghost/SKILL.md      when to reach for it, and the failures that look like bugs
examples/                  C#, Go, Rust and Node clients, each run against a live browser
tools/
  ghost_client.py          the Python control-plane client (DPAPI + AES-GCM cookies)
  product_check.py         end-to-end acceptance of the single shipped file
  serve_check.py           control-plane acceptance
  mcp_check.py             MCP acceptance
  captcha_check.py         the three real challenges, end to end
  token_check.py           the solving-service token route, against a stand-in service
  verify_ghost.ps1         proves ghost.exe is self-contained
  pe_exports.py            dependency-free PE export-table parser
  token_sids.ps1           process token / integrity / restricted-SID dumper
  mitigations.ps1          process mitigation policy dumper
  push_via_api.py          publish through the Git Data API when :443 is blocked
docs/
  PROTOCOL.md              the control-plane wire protocol
  USAGE.md                 how to run it
  ARCHITECTURE.md          full design, incl. the Track B plan
  CI.md                    what runs on free runners vs. a self-hosted one
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
