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

## 4. The profile

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
  "window_height": 800
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

## 5. Verify it in a real browser

The repository has two graders. Both measure from **inside the renderer** and
POST the result to a local collector — the only honest way to measure when there
is no debugging protocol to query.

```powershell
git clone https://github.com/Mouseww/ghost-browser
cd ghost-browser

# grades the built ghost.exe end to end, exactly as a user would run it
python tools\product_check.py

# grades the shim through the developer launcher, with the sandbox on
python harness\run_detect.py --sandboxed --chrome-arg=--disable-gpu-sandbox
```

Both print a per-check table and end with `34 checks, 0 failed`. The harness run
shows `[GAP]` (not `[PASS]`) on `hardwareConcurrency` and `deviceMemory`, because
the sandbox makes those structurally unreachable — a gap, not a failure.

`run_detect.py` is roughly 50% flaky (`NO REPORT RECEIVED` from a Chrome startup
race). Run it again.

## 6. Building from source

```powershell
cmake -S native -B native/build -A x64
cmake --build native/build --config Release
```

Needs the Visual Studio C++ toolset (any recent version; CMake picks it).
Output lands in `native\build\bin`. MinHook is vendored under
`native\vendor\minhook`, so there are no submodules to fetch.

Stop any running Chrome before rebuilding: it holds `ghost_shim.dll` open and the
link step fails with `LNK1104`.

## 7. Troubleshooting

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

## 8. What is not implemented yet

This is a vertical slice, not a finished product. Not built yet:

- **No agent control API.** The original goal is a browser driven by an
  automated agent without CDP; the control plane (`ghostd`) is designed in
  `docs/ARCHITECTURE.md` §4 but not written. Today you can launch and spoof, but
  you cannot yet drive the page programmatically. The planned approach is OS
  input synthesis plus the accessibility tree, so that no debugging protocol
  exists for a page to detect.
- **No CAPTCHA solving.** The three-tier strategy (silent pass → local audio →
  third-party API) is designed in §6, not implemented.
- **Windows x64 only.** Linux and macOS are designed in §8 and not implemented.
- **No canvas / audio / font-metric spoofing.** Canvas hashing, audio
  fingerprinting and font metrics are still measured from the real machine. See
  `docs/ARCHITECTURE.md` §11.5.
