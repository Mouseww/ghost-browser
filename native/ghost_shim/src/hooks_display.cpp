// hooks_display.cpp — channel A: screen geometry and device pixel ratio.
//
// JS surface covered:
//   screen.width / height / availWidth / availHeight / colorDepth
//   window.devicePixelRatio
//   window.outerWidth / outerHeight (derived from the monitor work area)
//
// Windows geometry subtlety that must not be gotten wrong: a per-monitor-DPI-aware
// process (which Chromium is) receives *physical* pixels from GetMonitorInfoW and
// GetSystemMetrics. Chromium then divides by the device scale factor to obtain the
// CSS-pixel values that JS sees. So to make screen.width == profile.screen.width we
// must report width * dpr, not width. Reporting the CSS value directly would make
// the page see screen.width == width / dpr — a self-inconsistent profile, which is
// exactly the kind of thing fingerprinters key on.
#include <windows.h>
#include <shellscalingapi.h>

#include <cmath>
#include <cstring>

#include "../include/ghost_profile.h"
#include "hook_engine.h"

namespace ghost {
namespace {

int(WINAPI* real_GetSystemMetrics)(int) = nullptr;
BOOL(WINAPI* real_GetMonitorInfoW)(HMONITOR, LPMONITORINFO) = nullptr;
BOOL(WINAPI* real_GetMonitorInfoA)(HMONITOR, LPMONITORINFO) = nullptr;
BOOL(WINAPI* real_EnumDisplayMonitors)(HDC, LPCRECT, MONITORENUMPROC, LPARAM) = nullptr;
int(WINAPI* real_GetDeviceCaps)(HDC, int) = nullptr;
HRESULT(WINAPI* real_GetDpiForMonitor)(HMONITOR, MONITOR_DPI_TYPE, UINT*, UINT*) = nullptr;
UINT(WINAPI* real_GetDpiForWindow)(HWND) = nullptr;
UINT(WINAPI* real_GetDpiForSystem)() = nullptr;

struct Geom {
  int phys_w = 0;
  int phys_h = 0;
  int phys_avail_w = 0;
  int phys_avail_h = 0;
  UINT dpi = 96;
};

// Returns false when the profile cannot produce a coherent geometry, in which case
// every hook falls through to the real API untouched.
bool geometry(Geom& g) {
  const Profile& p = Profile::current();
  if (!p.enabled || p.screen.width <= 0 || p.screen.height <= 0) return false;

  double dpr = p.screen.device_pixel_ratio;
  if (!(dpr > 0.0)) dpr = 1.0;

  const int avail_w = p.screen.avail_width > 0 ? p.screen.avail_width : p.screen.width;
  const int avail_h = p.screen.avail_height > 0 ? p.screen.avail_height : p.screen.height;

  g.phys_w = static_cast<int>(std::lround(p.screen.width * dpr));
  g.phys_h = static_cast<int>(std::lround(p.screen.height * dpr));
  g.phys_avail_w = static_cast<int>(std::lround(avail_w * dpr));
  g.phys_avail_h = static_cast<int>(std::lround(avail_h * dpr));
  g.dpi = static_cast<UINT>(std::lround(96.0 * dpr));
  if (g.dpi == 0) g.dpi = 96;
  return g.phys_w > 0 && g.phys_h > 0;
}

void apply_monitor_info(LPMONITORINFO mi, const Geom& g) {
  if (mi == nullptr) return;
  mi->rcMonitor.left = 0;
  mi->rcMonitor.top = 0;
  mi->rcMonitor.right = g.phys_w;
  mi->rcMonitor.bottom = g.phys_h;
  mi->rcWork.left = 0;
  mi->rcWork.top = 0;
  mi->rcWork.right = g.phys_avail_w;
  mi->rcWork.bottom = g.phys_avail_h;
}

int WINAPI hook_GetSystemMetrics(int index) {
  Geom g;
  if (geometry(g)) {
    switch (index) {
      case SM_CXSCREEN: return g.phys_w;
      case SM_CYSCREEN: return g.phys_h;
      case SM_CXMAXIMIZED: return g.phys_avail_w;
      case SM_CYMAXIMIZED: return g.phys_avail_h;
      case SM_CXVIRTUALSCREEN: return g.phys_w;
      case SM_CYVIRTUALSCREEN: return g.phys_h;
      case SM_XVIRTUALSCREEN: return 0;
      case SM_YVIRTUALSCREEN: return 0;
      case SM_CMONITORS: return 1;
      default: break;
    }
  }
  return real_GetSystemMetrics ? real_GetSystemMetrics(index) : 0;
}

BOOL WINAPI hook_GetMonitorInfoW(HMONITOR monitor, LPMONITORINFO info) {
  if (real_GetMonitorInfoW == nullptr) return FALSE;
  const BOOL ok = real_GetMonitorInfoW(monitor, info);
  if (!ok) return ok;
  Geom g;
  if (geometry(g)) apply_monitor_info(info, g);
  return ok;
}

BOOL WINAPI hook_GetMonitorInfoA(HMONITOR monitor, LPMONITORINFO info) {
  if (real_GetMonitorInfoA == nullptr) return FALSE;
  const BOOL ok = real_GetMonitorInfoA(monitor, info);
  if (!ok) return ok;
  Geom g;
  if (geometry(g)) apply_monitor_info(info, g);
  return ok;
}

struct EnumContext {
  MONITORENUMPROC callback = nullptr;
  LPARAM user_data = 0;
  int emitted = 0;
};

BOOL CALLBACK enum_monitor_wrapper(HMONITOR monitor, HDC dc, LPRECT rect, LPARAM param) {
  auto* ctx = reinterpret_cast<EnumContext*>(param);
  if (ctx == nullptr || ctx->callback == nullptr) return FALSE;
  if (ctx->emitted > 0) return FALSE;  // collapse a multi-monitor host into one monitor
  ctx->emitted++;

  Geom g;
  RECT fixed = *rect;
  if (geometry(g)) {
    fixed.left = 0;
    fixed.top = 0;
    fixed.right = g.phys_w;
    fixed.bottom = g.phys_h;
  }
  return ctx->callback(monitor, dc, &fixed, ctx->user_data);
}

BOOL WINAPI hook_EnumDisplayMonitors(HDC dc, LPCRECT clip, MONITORENUMPROC callback,
                                     LPARAM data) {
  if (real_EnumDisplayMonitors == nullptr) return FALSE;
  if (callback == nullptr) return real_EnumDisplayMonitors(dc, clip, callback, data);

  EnumContext ctx;
  ctx.callback = callback;
  ctx.user_data = data;
  return real_EnumDisplayMonitors(dc, clip, enum_monitor_wrapper,
                                  reinterpret_cast<LPARAM>(&ctx));
}

int WINAPI hook_GetDeviceCaps(HDC dc, int index) {
  Geom g;
  if (geometry(g)) {
    switch (index) {
      case HORZRES:
      case DESKTOPHORZRES: return g.phys_w;
      case VERTRES:
      case DESKTOPVERTRES: return g.phys_h;
      case LOGPIXELSX:
      case LOGPIXELSY: return static_cast<int>(g.dpi);
      default: break;
    }
  }
  return real_GetDeviceCaps ? real_GetDeviceCaps(dc, index) : 0;
}

HRESULT WINAPI hook_GetDpiForMonitor(HMONITOR monitor, MONITOR_DPI_TYPE type, UINT* x,
                                     UINT* y) {
  Geom g;
  if (geometry(g)) {
    if (x != nullptr) *x = g.dpi;
    if (y != nullptr) *y = g.dpi;
    return S_OK;
  }
  if (real_GetDpiForMonitor == nullptr) return E_FAIL;
  return real_GetDpiForMonitor(monitor, type, x, y);
}

UINT WINAPI hook_GetDpiForWindow(HWND window) {
  Geom g;
  if (geometry(g)) return g.dpi;
  return real_GetDpiForWindow ? real_GetDpiForWindow(window) : 96;
}

UINT WINAPI hook_GetDpiForSystem() {
  Geom g;
  if (geometry(g)) return g.dpi;
  return real_GetDpiForSystem ? real_GetDpiForSystem() : 96;
}

}  // namespace

void install_display_hooks() {
  install_hook_export("user32.dll", "GetSystemMetrics",
                      reinterpret_cast<void*>(hook_GetSystemMetrics),
                      reinterpret_cast<void**>(&real_GetSystemMetrics));
  install_hook_export("user32.dll", "GetMonitorInfoW",
                      reinterpret_cast<void*>(hook_GetMonitorInfoW),
                      reinterpret_cast<void**>(&real_GetMonitorInfoW));
  install_hook_export("user32.dll", "GetMonitorInfoA",
                      reinterpret_cast<void*>(hook_GetMonitorInfoA),
                      reinterpret_cast<void**>(&real_GetMonitorInfoA));
  install_hook_export("user32.dll", "EnumDisplayMonitors",
                      reinterpret_cast<void*>(hook_EnumDisplayMonitors),
                      reinterpret_cast<void**>(&real_EnumDisplayMonitors));
  install_hook_export("gdi32.dll", "GetDeviceCaps",
                      reinterpret_cast<void*>(hook_GetDeviceCaps),
                      reinterpret_cast<void**>(&real_GetDeviceCaps));
  install_hook_export("shcore.dll", "GetDpiForMonitor",
                      reinterpret_cast<void*>(hook_GetDpiForMonitor),
                      reinterpret_cast<void**>(&real_GetDpiForMonitor));
  install_hook_export("user32.dll", "GetDpiForWindow",
                      reinterpret_cast<void*>(hook_GetDpiForWindow),
                      reinterpret_cast<void**>(&real_GetDpiForWindow));
  install_hook_export("user32.dll", "GetDpiForSystem",
                      reinterpret_cast<void*>(hook_GetDpiForSystem),
                      reinterpret_cast<void**>(&real_GetDpiForSystem));
}

}  // namespace ghost
