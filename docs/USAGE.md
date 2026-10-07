# Using ghost.exe

The [latest release](https://github.com/Mouseww/ghost-browser/releases/latest)
ships **one file**:

| File | What it is |
|---|---|
| `ghost.exe` | the whole product. It finds your Chromium, invents a machine identity, extracts its own native shim, and starts the browser with the shim already inside it. |
| `SHA256SUMS.txt` | checksums, for the paranoid. |

You do not need to clone the repository, build anything, or keep any other file
next to it. Drop `ghost.exe` anywhere and run it.

```powershell
.\ghost.exe help
```

```
usage:
  ghost browse [options] [url ...]      launch the browser under the shim
  ghost serve [options] [url ...]       serve a JSON control plane on a named pipe
  ghost call [options] <json>           send one request to a running control plane
  ghost run [options] -- <exe> [args]   run any program under the shim
  ghost profile new [options]           create a profile
  ghost profile show [options]          print a profile
  ghost profile list                    list stored profiles
  ghost selftest                        verify injection end to end
  ghost doctor                          report what ghost can see here
  ghost install [--uninstall]           put ghost on the user PATH
  ghost version
  ghost help
```

The browser window it opens is titled **Ghost Browser**, wears the project icon,
and carries no "unsupported command-line flag" infobar. Three things still say
Chrome and cannot be changed at runtime — `chrome://version`, the on-disk
`chrome.exe` file name, and the window class `Chrome_WidgetWin_1`.

## 1. Check that it works (30 seconds, no browser needed)

```powershell
.\ghost.exe selftest
```

`selftest` extracts its embedded shim, injects it into **a copy of itself**
running a hidden `__probe` subcommand, and compares what that process reads back
from the real Win32 APIs against a profile whose values this machine cannot
satisfy by accident:

```
[PASS] ghost_shim.dll loaded   = yes
[PASS] GetActiveProcessorCount = 7
[PASS] dwNumberOfProcessors    = 7
[PASS] ullTotalPhys            = 12884901888
[PASS] GetSystemMetrics(CX)    = 2880
[PASS] GetSystemMetrics(CY)    = 1620
[PASS] GetDpiForMonitor        = 144 x 144
[PASS] GetUserDefaultLocaleName= en-GB
[PASS] GetTimeZoneInformation Bias=0

9/9 checks passed (target exit 0)
```

If you see 9/9 on a machine whose real values are different, the whole chain
works: resource extraction, process suspension, remote injection, the readiness
handshake, and every hook.

`ghost doctor` prints what the tool can see on this machine — the browser it
found and its version, the cache and profile directories, and your real time
zone and locale (useful as a sanity check that the profile differs from them).

## 2. Actually browse through it

```powershell
.\ghost.exe browse https://example.com
```

That is the whole command. `browse` will:

1. find Chrome (or Edge, if Chrome is absent),
2. create a profile under `%LOCALAPPDATA%\GhostBrowser\profiles\default.json`
   if one does not exist,
3. align the profile's `user_agent` with the browser version actually installed,
4. extract the shim into a content-addressed cache directory,
5. start the browser suspended, inject, wait for the hooks, then resume it.

The browser gets its own `--user-data-dir`, so it never attaches to a running
instance and never touches your normal profile.

Useful flags:

| Flag | Effect |
|---|---|
| `--id <name>` | use a different profile (default `default`) |
| `--seed <hex>` | deterministic identity; the same seed always yields the same machine |
| `--tz <IANA>` | override the time zone, e.g. `--tz Europe/London` |
| `--locale <tag>` | override the locale, e.g. `--locale en-GB` |
| `--browser edge` | use Edge instead of Chrome, or pass a full path to any Chromium build |
| `--chrome-arg <arg>` | extra browser argument; repeatable |
| `--user-data-dir <d>` | where cookies and storage live |
| `--no-wait` | return as soon as the browser is running |
| `--verbose` | print every step, including the injection result |

### The `--sandbox` trade-off

Chromium's renderer sandbox runs the renderer with a restricted token whose only
restricted SID is `S-1-0-0`. `LoadLibraryW` inside such a process returns
`STATUS_ACCESS_DENIED`; **no DLL of ours can ever load there**. Since
`navigator.hardwareConcurrency` and `navigator.deviceMemory` are computed in the
renderer, spoofing them requires that sandbox to be off, so `browse` passes
`--no-sandbox` by default and says so in `ghost help`.

Pass `--sandbox` to keep the sandbox. You then get a fully sandboxed browser
whose `hardwareConcurrency`, `deviceMemory` and WebGL adapter are real. That is a
legitimate choice, but it is a weaker one, and the tool tells you so rather than
quietly doing it.

`browse` also passes `--disable-gpu-sandbox`. With it, the GPU process is created
through `CreateProcessW` and becomes injectable, which is what makes WebGL
adapter spoofing work; the renderer sandbox is unaffected.

## 3. Run anything else under the shim

```powershell
.\ghost.exe run --profile C:\path\to\profile.json -- .\some-program.exe --its-args
```

`run` is the escape hatch: same injection machinery, no browser discovery. The
target's output is forwarded to your console.

## 4. Drive it from a program, without CDP

`ghost serve` runs the browser behind a JSON control plane. The transport is a
**named pipe** (`\\.\pipe\ghost-<id>`), never a TCP port, because a page can scan
ports and cannot scan pipes. There is no `--remote-debugging-port`, no
`Runtime.enable`, no injected utility script, and no `navigator.webdriver`.

```powershell
.\ghost.exe serve --id demo --pipe demo --tz Europe/London --locale en-GB https://example.com
```

It prints the browser pid and the pipe name, then serves one request per line:

```jsonc
{"cmd":"status"}
{"cmd":"navigate","url":"https://example.com"}
{"cmd":"tree","max_nodes":400}
{"cmd":"find","role":"button","name":"Accept"}
{"cmd":"click","role":"button","name":"Accept"}
{"cmd":"type","text":"hello"}
{"cmd":"key","keys":["ctrl","a"]}
{"cmd":"scroll","delta":-600}
{"cmd":"screenshot","path":"shot.bmp"}
{"cmd":"shutdown"}
```

Every reply is `{"ok":true, ...}` or `{"ok":false,"error":"..."}`. Anything that
needs the window waits up to 30 s for it to appear, so a client can send its
first command the moment `serve` prints the pid.

`shutdown` closes the browser it started, politely first (`WM_CLOSE`, so the
profile is flushed) and by force if it will not go. That is deliberate: Chromium
locks its `--user-data-dir` exclusively, so a browser left behind makes the next
run's browser exit instantly and show no window at all. A session started with
`--attach <pid>` owns nothing and leaves the browser running.

### Input needs a connected session

`click`, `type`, `key` and `scroll` are real `SendInput` events, and Windows
**discards synthesized input while the session is disconnected** — there is no
foreground window to deliver it to, and the call reports success anyway. A
disconnected or headless RDP session therefore cannot be driven, and because the
failure is silent, every command after the first would simply time out. The
control plane now names the condition instead:

```
control plane error: this session has no foreground window, so Windows discards
synthesized input (a disconnected or headless session); connect the session and retry
```

This is an environment requirement, not a limitation of the approach: the same
suite passes in full once the session is connected. Everything that does not need
input still works in a disconnected session — `status`, `tree`, `find`,
`screenshot`, and reading cookies after shutdown — and passing a URL on the
command line (`ghost serve <url>`) loads a page without typing.

### The Python client

```python
from tools.ghost_client import Ghost

with Ghost("demo").start(url="https://example.com") as browser:
    browser.wait_for(role="button", name="Accept")
    browser.click(role="button", name="Accept")   # isTrusted
    print(browser.tree(max_nodes=40))             # accessibility tree
    browser.screenshot("shot.bmp")                # PrintWindow
```

`Ghost(...)` alone does nothing — call `.start()` (or use the
`with Ghost("x").start(...) as g:` form). The client is a thin wrapper over
`open(pipe, "r+b")`, so if you prefer another language the protocol is one JSON
line each way.

### Why nothing here is detectable

| Need | How | What the page sees |
|---|---|---|
| input | `SendInput` | `isTrusted: true`, real hardware timestamps |
| page structure | UI Automation tree | nothing — no binding, no script |
| screenshot | `PrintWindow` | nothing |
| cookies / storage | read the profile's SQLite | nothing |
| navigation | `Ctrl+L`, then type the URL | ordinary typing |

Mouse movement is interpolated (smoothstep easing with a slight vertical bow)
rather than teleported, clicks hold for 35–90 ms, and typing sends
`KEYEVENTF_UNICODE` per UTF-16 code unit, which ignores the keyboard layout
entirely. `python tools/serve_check.py` measures all of this against a local page
that reports its own state through `aria-label` — including a trusted/untrusted
event counter, which ends at `trusted=109, untrusted=0`.

### Cookies can only be read after the browser exits

Chrome holds `Default/Network/Cookies` with **no sharing at all**: `CreateFileW`
fails with `ERROR_SHARING_VIOLATION` (32) under every share mode, while other
files in the same directory open normally. It is not an ACL or sandbox effect, so
cookies are only readable once the browser has exited — which makes this a
harvest-after-shutdown channel, and also why `cf_clearance` can be collected but
not injected mid-session.

Order matters in client code: `cookie_db()` asks the control plane where the
profile lives, so resolve the path *before* calling `stop()`.

```python
db = browser.cookie_db()      # needs the control plane
browser.stop()                # closes the browser
print(browser.cookies(db=db)) # now readable
```

Two more things about that database, both of which cost real debugging time:

- **Session cookies are never written to it.** A cookie without `max-age` or
  `expires` lives only in the browser's memory. If you need a value after
  shutdown, set an expiry.
- **The `value` column is empty.** Chrome 154 keeps the real bytes in
  `encrypted_value` as `b"v10"` + AES-256-GCM(`nonce(12) || ciphertext || tag(16)`),
  with the key in `<data_dir>/Local State` under `os_crypt.encrypted_key`
  (base64 of `b"DPAPI"` + a DPAPI-wrapped 32-byte key). The plaintext is **32
  bytes of domain binding followed by the value**, so `ghost_check=ok` decrypts
  to 32 unknown bytes then `ok`.

`ghost_client` does all of that with `ctypes` against `crypt32` and `bcrypt`, so
reading cookies still needs no third-party package.

## 5. The profile

Every spoofed value comes from one JSON file. `ghost profile new` generates one
with a coherent machine preset chosen from the seed:

```powershell
.\ghost.exe profile new --id work --seed 9f2c41d7a8b30e56 --tz Europe/London --locale en-GB
.\ghost.exe profile show --id work
.\ghost.exe profile list
```

```jsonc
{
  "enabled": true,
  "profile_id": "work",
  "profile_seed_hex": "9f2c41d7a8b30e56",   // reserved for per-origin noise
  "cpu_hardware_concurrency": 4,
  "memory_total_bytes": 8589934592,          // 8 GiB
  "screen_width": 2560,                      // logical px
  "screen_height": 1440,
  "screen_avail_width": 2560,
  "screen_avail_height": 1400,               // < height: a taskbar exists
  "device_pixel_ratio": 1.25,                // screen_width * DPR = 3200 physical
  "color_depth": 24,
  "timezone_id": "Europe/London",            // IANA
  "timezone_windows_key": "GMT Standard Time",
  "timezone_bias_minutes": 0,                // standard UTC offset, not the DST one
  "locale": "en-GB",
  "locale_langid": 2057,                     // must match locale
  "gpu_adapter_description": "NVIDIA GeForce RTX 3060",
  "gpu_adapter_vendor_id": 4318,             // 0x10DE
  "gpu_adapter_device_id": 9476,             // 0x2504
  "gpu_adapter_video_memory": 12884901888,
  "user_agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) ... Chrome/154.0.0.0 ...",
  "platform": "Win32",
  "window_width": 1280,
  "window_height": 800,
  "fonts": ["Arial", "Arial Black", "Bahnschrift", "Calibri", "..."],
}
```

Things that must agree, or the profile is worse than no profile:

- `screen_width` × `device_pixel_ratio` should be a real physical resolution.
  `2560 × 1.25 = 3200`, `1920 × 1.0` and `3840 × 1.5` are plausible;
  `1920 × 1.25` is not.
- `timezone_id` ↔ `timezone_windows_key` ↔ `timezone_bias_minutes` must describe
  the same zone. `Europe/London` is `GMT Standard Time` with bias `0`.
- `locale` ↔ `locale_langid` (`en-GB` is `2057`) ↔ the locale passed to the
  browser. `browse` handles the last one for you.
- `gpu_adapter_description` ↔ `gpu_adapter_device_id`. ANGLE prints the adapter
  description and then the device id in parentheses; if they disagree the
  renderer string contradicts itself. The pair above produces
  `ANGLE (NVIDIA, NVIDIA GeForce RTX 3060 (0x00002504) Direct3D11 vs_5_0 ps_5_0, D3D11)`.
- The time zone should match where your egress IP geolocates, or the profile
  contradicts the network.

The preset table covers twelve real machines (GTX 1650 through RTX 4080, RX 6600,
RX 6700 XT, UHD 630, Iris Xe) with matching core counts, memory, panel sizes,
device ids and video memory. Editing the JSON by hand is supported; just keep the
pairs above consistent.

### The `fonts` list

The installed font list is one of the loudest fingerprints a machine has, because
it is a direct readout of what software is installed. The machine this was built
on carries **266** families: the stock Windows set, all of Office, a Chinese IME
pack, and a developer's toolchain (`Cascadia Code`, `Noto Sans SC`,
`Ubuntu Mono`). Any one of those dates a profile; the combination identifies it.

`fonts` is the allow-list the shim filters DirectWrite down to, and `ghost profile
new` fills it with the ~89 families a clean Windows install ships. The shim hooks
`dwrite.dll!DWriteCreateFactory` and the `IDWriteFontCollection` it returns, so
`document.fonts`, canvas `measureText`, and font-metric probing all agree.

Two limits worth knowing:

- **It hides fonts, it cannot invent them.** The visible set is the profile's list
  intersected with what the OS actually has. Listing a font you do not have does
  nothing; it will not appear.
- **If none of the listed families exist on the host, the hook does not patch at
  all**, rather than reporting an empty font collection. A browser that believes
  it has no fonts renders text very visibly wrong, which is a worse signal than an
  unfiltered list.

Deleting the `fonts` key disables font filtering entirely.

## 6. Verify it in a real browser

There are three graders. All of them measure from **inside the renderer** and
POST the result to a local collector — the only honest way to measure when there
is no debugging protocol to query.

```powershell
git clone https://github.com/Mouseww/ghost-browser
cd ghost-browser

# grades the built ghost.exe end to end, exactly as a user would run it
python tools\product_check.py

# grades the shim through the developer launcher, with the sandbox on
python harness\run_detect.py --sandboxed --chrome-arg=--disable-gpu-sandbox

# grades the control plane: click, type, scroll, screenshot, cookies
python tools\serve_check.py

# ...or grade the artifact users actually download
python tools\serve_check.py --ghost "$env:TEMP\ghost.exe"
```

The first two print a per-check table and end with `34 checks, 0 failed`; the
control plane check ends with `17 checks, 0 failed`. The harness run shows
`[GAP]` (not `[PASS]`) on `hardwareConcurrency` and `deviceMemory`, because the
sandbox makes those structurally unreachable — a gap, not a failure.

`run_detect.py` is roughly 50% flaky (`NO REPORT RECEIVED` from a Chrome startup
race). Run it again.

`serve_check.py` drives a real window, so it needs the foreground: it clicks and
types with `SendInput`, which goes to whatever is in front. Do not touch the
mouse or keyboard while it runs.

## 7. Building from source

```powershell
cmake -S native -B native/build -A x64
cmake --build native/build --config Release
```

Needs the Visual Studio C++ toolset (any recent version; CMake picks it).
Output lands in `native\build\bin`. MinHook is vendored under
`native\vendor\minhook`, so there are no submodules to fetch.

Stop any running Chrome before rebuilding: it holds `ghost_shim.dll` open and the
link step fails with `LNK1104`.

## 8. Troubleshooting

**`ghost: CreateProcess failed: 225`**
`225` is `ERROR_VIRUS_INFECTED`: antivirus blocked the launch. "Create a
suspended process, write a DLL path into it, start a remote thread" is exactly
what process injectors do, so the heuristic fires
(`Behavior:Win32/DefenseEvasion.A!ml`). `ghost.exe` itself is usually fine —
it is the extracted `ghost_shim.dll` that gets flagged. Add an exclusion for

```
%LOCALAPPDATA%\GhostBrowser
```

and for the folder you keep `ghost.exe` in, or build from source. The binaries
are **not code-signed**; that is the honest reason this happens.

**`ghost: readiness handshake failed (...)`**
Injection did not complete, and `ghost` refuses to start an unspoofed browser.
It prints `stage=N`:

| stage | meaning |
|---|---|
| 0 | could not find the shim's readiness export |
| 1 | could not resolve `kernel32!LoadLibraryW` in the target |
| 2 | could not allocate memory in the target |
| 3 | could not write the shim path into the target |
| 4 | `CreateRemoteThread` was refused (target already terminating) |
| 5 | injection succeeded but the shim never signalled ready |

Pass `--allow-unspoofed` to start anyway. **Do not do this for real work** — you
get an ordinary, fully fingerprintable browser.

**The browser starts but nothing is spoofed**
Check `%TEMP%\ghost_shim.log`. If it is missing entirely, the shim never loaded.
If it stops before `hooks installed=N failed=0`, something failed early — the
shim deliberately never crashes the host, it just gives up.

**The browser exits immediately**
Almost always a missing or shared `--user-data-dir`. `browse` creates one per
profile; if you passed your own, make it private and fresh.

**Sandboxed child processes have no shim**
Correct and unavoidable. The log line `load_exit=0xC0000022` means
`STATUS_ACCESS_DENIED` from a restricted token. Only the browser, GPU and
utility processes are reachable.

## 9. Human-verification challenges

The `captcha` command on the control plane reads the challenge and, when it can,
clears it. It works through the accessibility tree rather than the DOM, so there is
no CDP involved and nothing is injected. Send it with `ghost call` to a session that
is already running (`ghost serve --id work` in another window):

```
ghost call --id work "{\"cmd\":\"captcha\"}"                          # detect, then try to solve
ghost call --id work "{\"cmd\":\"captcha\",\"action\":\"detect\"}"    # only report
ghost call --id work "{\"cmd\":\"captcha\",\"action\":\"solve-audio\",\"language\":\"en-US\"}"
```

A solved Cloudflare Turnstile looks like this:

```json
{
  "provider": "turnstile",
  "state": "solved",
  "page_url": "https://nopecha.com/demo/cloudflare",
  "detail": "the widget is gone"
}
```

`state` is one of:

| state | meaning | what to do |
|---|---|---|
| `absent` | no widget on the page, or one is mid-verification | nothing, or wait |
| `checkbox` | a widget is showing its checkbox | click it — that is what `solve` does |
| `visual` | an image challenge is open | **stop** — nothing here solves it |
| `audio` | an audio challenge is open | `action=solve-audio` records it and types the answer |
| `solved` | the widget is gone | continue |

What actually happens today, measured against the three real challenges
([`tools/captcha_check.py`](tools/captcha_check.py), **18 checks, 0 failed**):

| challenge | result |
|---|---|
| Cloudflare Turnstile | **passes**, on one trusted click |
| hCaptcha | sitekey read, checkbox clicked, image challenge opens |
| reCAPTCHA v2 | sitekey read, checkbox clicked, image challenge opens; the audio button is reachable and opens the audio challenge |

### Solving the audio challenge

`action=solve-audio` takes the route that does not need to see the page: it opens
the audio challenge, starts recording the default output device in loopback mode,
asks for a replay so the clip starts inside the recording window, transcribes the
digits locally and types them back. Nothing is downloaded and no URL is parsed —
what gets transcribed is what the machine actually played.

```json
{
  "provider": "recaptcha",
  "state": "audio",
  "device": "Speakers (Realtek)",
  "captured_seconds": 9.9,
  "peak": 0.61,
  "streams": "pid 4128 active peak 0.6000",
  "heard": "37194",
  "confidence": 0.91,
  "typed": "37194",
  "solved": true
}
```

Three fields are there for when it does *not* work, because "it heard nothing" has
two very different causes and the samples cannot tell them apart:

- `peak` and `rms` — the level of what was captured. Zero means silence.
- `streams` — who was holding a stream on the output device at that moment, and
  how loud. This is the same list the Windows volume mixer draws. If it is empty,
  the page never played anything and retrying will not help; if a browser process
  is `active` with a non-zero peak while `peak` is zero, the recording is at fault.
- `confidence` — how sure the recogniser was. A wrong answer costs an attempt, so
  a low-confidence answer is reported as such rather than submitted blindly.

Two honest limits on this machine, both written into
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) §14.6: local speech-to-text needs a
recogniser for the challenge's language (**only a Chinese one is installed here**,
and installing an English one needs administrator rights), and the reCAPTCHA audio
challenge measured `peak 0` in every run that reached it, which is still being
explained.

Two things are worth saying plainly. **A widget that is already there is not a
failure** — Turnstile often passes with no visible challenge at all, and hCaptcha
and reCAPTCHA only decide to escalate after you click. And **do not retry in a
loop**: repeated attempts are themselves a bot signal and make things worse, not
better. Ask once, report what you got, move on.

From an agent the same thing is one MCP call — `ghost_captcha`, with
`action="solve-audio"` for the audio route.

## 10. What is not implemented yet

This is a vertical slice, not a finished product. Not built yet:

- **No CAPTCHA solving beyond Turnstile.** Reading the challenge and clicking the
  checkbox works (§9). Image challenges are not solved, audio challenges are not
  transcribed, and the third-party API tier is not wired up. The plan is in
  `docs/ARCHITECTURE.md` §14.
- **Windows x64 only.** Linux and macOS are designed in §8 and not implemented.
- **No canvas / audio / font-metric spoofing.** Canvas hashing, audio
  fingerprinting and font metrics are still measured from the real machine. See
  `docs/ARCHITECTURE.md` §11.5.
- **Three things still say Chrome**, and cannot be changed at runtime because
  they are compiled into the engine: `chrome://version`, the on-disk file name
  `chrome.exe`, and the window class `Chrome_WidgetWin_1`. Only a source-level
  build (Track B) can fix them.
- **No CDP escape hatch.** `control.mode = "cdp-pipe"` is designed in
  `docs/ARCHITECTURE.md` §4.3 as a fallback for tools that need the protocol, and
  is not wired up. It would use `--remote-debugging-pipe`, never a TCP port.
