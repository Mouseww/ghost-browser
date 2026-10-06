# Using the prebuilt binaries

The [v0.1.0 release](https://github.com/Mouseww/ghost-browser/releases/tag/v0.1.0)
ships three files:

| File | What it is |
|---|---|
| `ghost_launch.exe` | the launcher. Starts a browser (or any program) with the shim injected. |
| `ghost_shim.dll` | the shim itself. Must sit **next to** `ghost_launch.exe`. |
| `probe.exe` | a test target that prints the real return value of every hooked API. |

Plus `SHA256SUMS.txt` and a sample profile, `slice-test-001.json`.

**These three files are all you need to run.** You do not need to clone the
repository or build anything, unless you want the fingerprint test page
(`harness/`), which is not part of the release.

## 1. Check that it works (30 seconds, no browser needed)

Put all the files in one directory and run the probe through the launcher:

```powershell
cd C:\wherever\you\unzipped

.\ghost_launch.exe --profile slice-test-001.json --tz Europe/London -- .\probe.exe
```

You should see `ghost_shim.dll loaded = yes` followed by the spoofed values:

```
GetActiveProcessorCount = 4
dwNumberOfProcessors    = 4          <- this is the one Chromium actually reads
ullTotalPhys            = 8589934592 <- 8 GiB, not your real RAM
GetSystemMetrics(CX)    = 3200
GetDeviceCaps(LOGPIX)   = 120
GetTimeZoneInformation  = ret=2 Bias=0 StandardBias=0 DaylightBias=-60
GetUserDefaultLocaleName= en-GB
```

If those numbers differ from your real machine, the shim is working. If
`ghost_shim.dll loaded = no`, the shim was not injected — see *Troubleshooting*.

## 2. Actually browse through it

`ghost_launch.exe` starts any program. To start a browser, pass it after `--`:

```powershell
.\ghost_launch.exe `
  --profile slice-test-001.json `
  --tz Europe/London `
  --lang en-GB `
  -- "C:\Program Files\Google\Chrome\Application\chrome.exe" `
     --user-data-dir="C:\temp\ghost-profile-001" `
     --no-first-run --no-default-browser-check
```

Notes on the flags:

- **`--user-data-dir` is not optional in practice.** Without it Chrome attaches
  to an already-running instance and your new process exits immediately, taking
  the injected shim with it.
- **`--tz` and `--lang` are passed to the browser as well** as to the shim, so
  the engine's own time zone and locale agree with the spoofed OS values. If you
  omit them the profile's `timezone_id` and `locale` are used.
- The launcher waits for the browser to exit. Add `--no-wait` to return
  immediately.
- Add `--disable-gpu-sandbox` to the Chrome arguments if you want WebGL
  spoofing: the GPU process is then created through `CreateProcessW` and becomes
  injectable, while the renderer sandbox stays on. Without it, WebGL reports
  your real adapter.

Everything after `--` goes to the browser verbatim.

### What is spoofed and what is not

| Spoofed | Not spoofed |
|---|---|
| processor count, physical memory | `hardwareConcurrency` / `deviceMemory` **inside sandboxed renderer processes** |
| screen geometry, DPI, `devicePixelRatio` | canvas `toDataURL` / `getImageData` |
| time zone, locale, `navigator.language` | `OfflineAudioContext` |
| WebGL vendor/renderer (with `--disable-gpu-sandbox`) | font **metrics** (font *lists* are fine) |
| `navigator.userAgent`, `platform` | |

The renderer rows are not "not yet done" — they are **structurally out of reach**
for this approach. A sandboxed renderer runs with a restricted token whose only
restricted SID is `S-1-0-0`, and `LoadLibraryW` inside it returns
`STATUS_ACCESS_DENIED`. No DLL of ours can ever load there. Reaching those
surfaces needs a source-level patch to Chromium (Track B, see
`docs/ARCHITECTURE.md` §11.5).

## 3. Verify it in a real browser

The repository has a fingerprint harness that measures from **inside the
renderer** and POSTs the result to a local collector — the only honest way to
measure when there is no CDP:

```powershell
git clone https://github.com/Mouseww/ghost-browser
cd ghost-browser
python harness\run_detect.py --sandboxed --chrome-arg=--disable-gpu-sandbox
```

Expected: `34 checks, 0 failed`, with `[GAP]` (not `[PASS]`) on
`hardwareConcurrency` and `deviceMemory` for the reason above. This script needs
the source tree because it serves `harness/detect.html`.

It is roughly 50% flaky (`NO REPORT RECEIVED` from a Chrome startup race). Just
run it again.

## 4. The profile file

Every spoofed value comes from one JSON file. Start from
`slice-test-001.json` and keep it **internally consistent** — incoherence
between fields is itself a fingerprint:

```jsonc
{
  "enabled": true,
  "profile_id": "slice-test-001",
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
  "timezone_bias_minutes": 0,                // UTC offset, not the DST offset
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
  `2560 × 1.25 = 3200`, `1920 × 1.0`, `3840 × 1.5` are all plausible;
  `1920 × 1.25` is not.
- `timezone_id` ↔ `timezone_windows_key` ↔ `timezone_bias_minutes` must describe
  the same zone. `Europe/London` is `GMT Standard Time` with bias `0`.
- `locale` ↔ `locale_langid` (`en-GB` is `2057`) ↔ the `--lang` argument.
- `gpu_adapter_description` ↔ `gpu_adapter_device_id`. ANGLE prints the adapter
  description and then the device id in parentheses; if they disagree the
  renderer string contradicts itself. The description above with device id
  `9476` produces
  `ANGLE (NVIDIA, NVIDIA GeForce RTX 3060 (0x00002504) Direct3D11 vs_5_0 ps_5_0, D3D11)`.
- The time zone should match where your egress IP geolocates, or the profile
  contradicts the network.

## 5. Building from source

```powershell
cmake -S native -B native/build -A x64
cmake --build native/build --config Release
```

Needs the Visual Studio C++ toolset (any recent version; CMake picks it).
Output lands in `native\build\bin`. MinHook is vendored under
`native/vendor/minhook`, so there are no submodules to fetch.

## 6. Troubleshooting

**`ghost_launch: readiness handshake failed (...)`**
Injection did not complete, and the launcher refuses to start an unspoofed
browser. It prints `stage=N`:

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

**Windows Defender deletes `ghost_launch.exe`**
Expected. "Create a suspended process, write a DLL path into it, start a remote
thread" is exactly what process injectors do, so the heuristic fires
(`Behavior:Win32/DefenseEvasion.A!ml`). Exclude the directory, or build from
source.

**The browser starts but nothing is spoofed**
Check `%TEMP%\ghost_shim.log`. If it is missing entirely, the shim never loaded.
If it stops before `hooks installed=N failed=0`, something failed early — the
shim deliberately never crashes the host, it just gives up.

**The browser exits immediately**
Almost always a missing or shared `--user-data-dir`. Use a fresh, private one
per profile.

**Sandboxed child processes have no shim**
Correct and unavoidable. The log line `load_exit=0xC0000022` means
`STATUS_ACCESS_DENIED` from a restricted token. Only the browser, GPU and
utility processes are reachable.

**`run_detect.py` says `NO REPORT RECEIVED`**
Known startup race, roughly 50% of runs. Retry. It is not a shim failure.

## 7. What is not implemented yet

This is a vertical slice, not a finished product. Not built yet:

- **No agent control API.** The original goal is a browser driven by an
  automated agent without CDP; the control plane (`ghostd`) is designed in
  `docs/ARCHITECTURE.md` §4 but not written. Today you can launch and spoof, but
  you cannot yet drive the page programmatically. The planned approach is OS
  input synthesis plus the accessibility tree, so that no debugging protocol
  exists for a page to detect.
- **No CAPTCHA solving.** The three-tier strategy (silent pass → local audio →
  third-party API) is designed in §6, not implemented.
- **No profile generator.** Profiles are hand-written JSON.
- **Windows x64 only.** Linux and macOS are designed in §8 and not implemented.
- **No canvas / audio / font-metric spoofing.** See §11.5.
