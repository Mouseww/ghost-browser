# Third-party notices

This project's own source is BSD-3-Clause (see [`LICENSE`](LICENSE)). It
depends on, or links against, the following third-party components.

## Vendored in this repository

### MinHook

- **Location:** [`native/vendor/minhook/`](native/vendor/minhook/)
- **License:** BSD-2-Clause — see [`native/vendor/minhook/LICENSE.txt`](native/vendor/minhook/LICENSE.txt)
- **Use:** x86/x64 inline hooking engine (trampoline-based), used by `ghost_shim.dll`.
- **Modifications:** none. Consumed as-is via `native/CMakeLists.txt`.

## Not distributed — obtained by the user

### Chromium / Google Chrome

`ghost_launch` injects `ghost_shim.dll` into a **user-supplied** Chromium or
Chrome installation. This repository does **not** contain, mirror, or
redistribute Chromium or Chrome binaries, and the build does not download them.

If you later distribute a **patched Chromium binary** (Track B), you take on the
obligations that come with it:

- Chromium's own license is BSD-3-Clause, but a Chromium build aggregates
  hundreds of third-party components under their own terms; ship
  `about:credits` / the generated license file alongside the binary.
- **Do not use the "Google Chrome", "Chrome", or "Chromium" names or logos** in
  a way that implies endorsement or origin. Ship under your own product name.
- Code-signing keys and the `chrome_elf` DLL blocklist are part of Google's
  distribution, not the open-source project; a rebuilt binary has neither.

### Microsoft Detours / Frida / others

Not used. See `docs/ARCHITECTURE.md` §2 for the hooking-library comparison and
why MinHook (BSD-2) was chosen over frida-gum (LGPL) and Detours (MIT but
heavier).

## Toolchain (build-time only, not redistributed)

CMake, MSVC / Windows SDK, Python 3.11, and the Rust toolchain. None of their
code is copied into this repository's artifacts beyond standard static linking
of the MSVC runtime.
