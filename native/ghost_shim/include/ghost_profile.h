// ghost_profile.h — the coherent fingerprint profile handed to the shim.
//
// Transport: the launcher exports the profile as the environment variable
// GHOST_PROFILE_JSON. Environment variables are inherited by every Chromium child
// process automatically, so no IPC and no CreateProcess hook is needed just to
// distribute configuration. Absent/invalid JSON degrades to enabled=false, which
// makes the shim a pure pass-through (the browser must always still start).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ghost {

struct CpuProfile {
  uint32_t hardware_concurrency = 0;  // -> navigator.hardwareConcurrency
};

struct MemoryProfile {
  uint64_t total_bytes = 0;  // -> navigator.deviceMemory (after Blink's rounding)
};

struct ScreenProfile {
  int32_t width = 0;  // -> screen.width
  int32_t height = 0;  // -> screen.height
  int32_t avail_width = 0;  // -> screen.availWidth
  int32_t avail_height = 0;  // -> screen.availHeight
  double device_pixel_ratio = 0.0;  // -> window.devicePixelRatio
  int32_t color_depth = 24;
};

struct TimeProfile {
  std::string timezone_id;  // "Asia/Shanghai" — IANA id, informational
  // Windows key that ICU maps back to an IANA id, e.g. "China Standard Time".
  // Required: ICU's uprv_detectWindowsTimeZone() reads this straight out of the
  // registry, so without it Intl.DateTimeFormat().timeZone stays unspoofed.
  std::string timezone_windows_key;
  int32_t bias_minutes = 0;  // UTC offset, Win32 TIME_ZONE_INFORMATION.Bias convention
  // Whether bias_minutes was actually supplied. A zero bias is a legitimate value
  // (UTC / Europe/London in winter), so the value itself cannot double as the
  // "should I spoof this?" flag — the presence of the key has to be tracked.
  bool has_bias = false;
  std::string locale;        // "zh-CN"
  uint32_t langid = 0;       // Win32 LANGID, e.g. 0x0804 for zh-CN
};

struct GpuProfile {
  // --- ANGLE-as-a-DLL path (legacy Chromium builds only) ---
  // Hookable only while ANGLE ships as libGLESv2.dll. Chrome 154 links ANGLE
  // statically into chrome.dll, so on current Chrome these fields are inert and
  // the adapter_* fields below are what actually move the WebGL surface.
  std::string vendor;    // -> UNMASKED_VENDOR_WEBGL
  std::string renderer;  // -> UNMASKED_RENDERER_WEBGL
  std::string gl_version;
  int32_t max_texture_size = 0;

  // --- DXGI adapter path (works on current Chrome, no symbols required) ---
  // ANGLE composes GL_RENDERER itself as
  //   "ANGLE (<vendor>, <adapter description> Direct3D11 vs_5_0 ps_5_0, D3D11)"
  // so spoofing the *adapter description* rather than the finished string keeps
  // ANGLE's own formatting, feature level and device-id suffix authentic. The
  // real machine's string is
  //   "ANGLE (NVIDIA, NVIDIA GeForce GTX 1050 Ti (0x00001C82) Direct3D11 vs_5_0 ps_5_0, D3D11)"
  // and the "(0x0000XXXX)" part comes from DeviceId, which is why DeviceId has
  // to move together with the description or the profile contradicts itself.
  std::string adapter_description;      // e.g. "NVIDIA GeForce RTX 3060"
  uint32_t adapter_vendor_id = 0;       // e.g. 0x10DE
  uint32_t adapter_device_id = 0;       // e.g. 0x2504
  uint64_t adapter_video_memory = 0;    // -> DXGI_ADAPTER_DESC.DedicatedVideoMemory
};

// Installed font families, as a fingerprinting page sees them through
// document.fonts.check() and canvas text measurement.
//
// This is the one text surface Track A can actually reach. Canvas pixels and
// audio samples are produced by Skia and Blink inside the renderer and never
// cross an OS API, so no OS-level hook can touch them; font enumeration does
// cross one, because Skia asks DirectWrite which families exist.
//
// The list is an allow-list: a family the profile omits is reported as not
// installed. An empty list disables filtering entirely, which keeps a profile
// without a `fonts` key a pure pass-through.
struct FontProfile {
  std::vector<std::string> families;  // UTF-8 family names, e.g. "Segoe UI"
  bool has_families = false;
};

struct Profile {
  std::string profile_id;
  std::string profile_seed_hex;
  CpuProfile cpu;
  MemoryProfile memory;
  ScreenProfile screen;
  TimeProfile time;
  GpuProfile gpu;
  FontProfile fonts;
  std::string user_agent;
  std::string platform;
  int32_t window_width = 0;
  int32_t window_height = 0;
  bool enabled = false;

  // Reads GHOST_PROFILE_JSON once per process (thread-safe).
  static const Profile& current();
};

}  // namespace ghost
