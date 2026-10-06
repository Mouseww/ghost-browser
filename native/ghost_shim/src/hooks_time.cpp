// hooks_time.cpp — channel A: time zone and locale.
//
// JS surface covered:
//   Intl.DateTimeFormat().resolvedOptions().timeZone
//   new Date().getTimezoneOffset()
//   navigator.language / navigator.languages (partly, via --lang)
//   Intl.DateTimeFormat().resolvedOptions().locale
//
// Two independent code paths must both be covered, which is the whole reason this
// file is longer than the others:
//
//   1. Win32 API path — GetTimeZoneInformation / GetDynamicTimeZoneInformation.
//      Used by Chromium's own time formatting and by some ICU fallbacks.
//
//   2. ICU registry path — ICU's uprv_detectWindowsTimeZone() does NOT call the
//      Win32 API. It opens HKLM\SYSTEM\CurrentControlSet\Control\TimeZoneInformation
//      and reads the REG_SZ value "TimeZoneKeyName", then maps that Windows key
//      through ICU's windowsZones table to an IANA id. Hooking only the Win32 API
//      therefore leaves Intl.DateTimeFormat().timeZone completely unspoofed.
//
// The launcher additionally exports TZ, which ICU consults before falling back to
// the registry; the registry hook is what makes the result deterministic.
#include <windows.h>

#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <vector>

#include "../include/ghost_profile.h"
#include "hook_engine.h"

namespace ghost {
namespace {

DWORD(WINAPI* real_GetTimeZoneInformation)(LPTIME_ZONE_INFORMATION) = nullptr;
DWORD(WINAPI* real_GetDynamicTimeZoneInformation)(PDYNAMIC_TIME_ZONE_INFORMATION) = nullptr;
BOOL(WINAPI* real_GetTimeZoneInformationForYear)(USHORT, PDYNAMIC_TIME_ZONE_INFORMATION,
                                                 LPTIME_ZONE_INFORMATION) = nullptr;
int(WINAPI* real_GetUserDefaultLocaleName)(LPWSTR, int) = nullptr;
LANGID(WINAPI* real_GetUserDefaultUILanguage)() = nullptr;
LANGID(WINAPI* real_GetSystemDefaultUILanguage)() = nullptr;
LSTATUS(WINAPI* real_RegQueryValueExW)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD) = nullptr;

std::wstring utf8_to_wide(const std::string& s) {
  if (s.empty()) return std::wstring();
  const int need = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                       nullptr, 0);
  if (need <= 0) return std::wstring();
  std::wstring out(static_cast<size_t>(need), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), need);
  return out;
}

// Windows TIME_ZONE_INFORMATION.Bias is "UTC = local time + Bias", in minutes, with
// the opposite sign convention from JavaScript's getTimezoneOffset().
// JS offset for UTC+8 is -480; Win32 Bias for UTC+8 is -480 as well, because
// UTC = local + Bias  =>  local = UTC - Bias  =>  UTC+8 needs Bias = -480.
// The profile therefore stores Bias directly in Win32 convention.
//
// Patching only `Bias` is not enough. StandardBias and DaylightBias are offsets
// *relative to* Bias, and the StandardDate/DaylightDate transition rules decide when
// each applies. Rewriting Bias alone while leaving another zone's transition dates in
// place produces a timezone that does not exist anywhere on Earth — and coherence is
// what fingerprinting actually measures.
//
// So instead of synthesising a timezone, ask Windows for the real one: the profile
// carries a Windows timezone key ("GMT Standard Time"), and
// EnumDynamicTimeZoneInformation enumerates the system timezone database including
// Bias, StandardBias, DaylightBias and both transition dates. Copying that entry
// wholesale yields exactly what the OS would have reported had the machine really
// been in that zone, DST rules included.
struct ResolvedTimeZone {
  bool valid = false;
  DYNAMIC_TIME_ZONE_INFORMATION dtz = {};
};

const ResolvedTimeZone& resolved_timezone() {
  static ResolvedTimeZone cache;
  static std::once_flag once;
  std::call_once(once, [] {
    const Profile& p = Profile::current();
    if (!p.enabled) return;
    const std::wstring want = utf8_to_wide(p.time.timezone_windows_key);
    if (want.empty()) return;

    for (DWORD index = 0;; ++index) {
      DYNAMIC_TIME_ZONE_INFORMATION candidate = {};
      const DWORD r = EnumDynamicTimeZoneInformation(index, &candidate);
      if (r != ERROR_SUCCESS) break;  // ERROR_NO_MORE_ITEMS ends the enumeration
      if (want == candidate.TimeZoneKeyName) {
        cache.dtz = candidate;
        cache.valid = true;
        ghost_log("timezone resolved: %ls bias=%ld std=%ld dst=%ld", want.c_str(),
                  candidate.Bias, candidate.StandardBias, candidate.DaylightBias);
        return;
      }
    }
    ghost_log("timezone key '%ls' not found in the system tz database", want.c_str());
  });
  return cache;
}

// Projects the resolved DYNAMIC_TIME_ZONE_INFORMATION onto the legacy
// TIME_ZONE_INFORMATION shape, which embeds the same fields plus the zone names.
void copy_dynamic_to_legacy(const DYNAMIC_TIME_ZONE_INFORMATION& src,
                            LPTIME_ZONE_INFORMATION dst) {
  dst->Bias = src.Bias;
  dst->StandardBias = src.StandardBias;
  dst->DaylightBias = src.DaylightBias;
  dst->StandardDate = src.StandardDate;
  dst->DaylightDate = src.DaylightDate;
  std::wmemcpy(dst->StandardName, src.StandardName,
               sizeof(dst->StandardName) / sizeof(wchar_t));
  std::wmemcpy(dst->DaylightName, src.DaylightName,
               sizeof(dst->DaylightName) / sizeof(wchar_t));
}

// The return value of GetTimeZoneInformation tells the caller whether the target zone
// is *currently* observing DST — TIME_ZONE_ID_STANDARD or TIME_ZONE_ID_DAYLIGHT.
// Passing through the host's value would be wrong whenever the two zones disagree
// (London in July is on DST; Shanghai never is).
//
// Rather than reimplement Windows' transition arithmetic, ask the OS: convert the
// current UTC instant into the target zone's wall clock with the dynamic zone we
// resolved, and compare the resulting offset against the zone's standard and daylight
// offsets.
DWORD dst_state_for(const DYNAMIC_TIME_ZONE_INFORMATION& tz) {
  if (tz.DaylightDate.wMonth == 0) return TIME_ZONE_ID_STANDARD;  // zone has no DST

  SYSTEMTIME utc = {};
  GetSystemTime(&utc);
  SYSTEMTIME local = {};
  if (SystemTimeToTzSpecificLocalTimeEx(
          const_cast<PDYNAMIC_TIME_ZONE_INFORMATION>(&tz), &utc, &local) == FALSE) {
    return TIME_ZONE_ID_UNKNOWN;
  }

  FILETIME ft_utc = {};
  FILETIME ft_local = {};
  if (SystemTimeToFileTime(&utc, &ft_utc) == FALSE ||
      SystemTimeToFileTime(&local, &ft_local) == FALSE) {
    return TIME_ZONE_ID_UNKNOWN;
  }
  ULARGE_INTEGER a;
  ULARGE_INTEGER b;
  a.LowPart = ft_utc.dwLowDateTime;
  a.HighPart = ft_utc.dwHighDateTime;
  b.LowPart = ft_local.dwLowDateTime;
  b.HighPart = ft_local.dwHighDateTime;

  // 100-nanosecond ticks -> minutes. Win32 stores Bias as "UTC = local + Bias".
  const LONGLONG observed_minutes =
      (static_cast<LONGLONG>(b.QuadPart) - static_cast<LONGLONG>(a.QuadPart)) / 600000000LL;
  const LONGLONG daylight_minutes = -static_cast<LONGLONG>(tz.Bias + tz.DaylightBias);

  return observed_minutes == daylight_minutes ? TIME_ZONE_ID_DAYLIGHT
                                              : TIME_ZONE_ID_STANDARD;
}

DWORD WINAPI hook_GetTimeZoneInformation(LPTIME_ZONE_INFORMATION tz) {
  if (real_GetTimeZoneInformation == nullptr) return TIME_ZONE_ID_INVALID;
  const DWORD r = real_GetTimeZoneInformation(tz);
  const Profile& p = Profile::current();
  if (tz == nullptr || !p.enabled) return r;

  const ResolvedTimeZone& resolved = resolved_timezone();
  if (resolved.valid) {
    copy_dynamic_to_legacy(resolved.dtz, tz);
    return dst_state_for(resolved.dtz);
  }
  if (p.time.has_bias) tz->Bias = p.time.bias_minutes;
  return r;
}

DWORD WINAPI hook_GetDynamicTimeZoneInformation(PDYNAMIC_TIME_ZONE_INFORMATION tz) {
  if (real_GetDynamicTimeZoneInformation == nullptr) return TIME_ZONE_ID_INVALID;
  const DWORD r = real_GetDynamicTimeZoneInformation(tz);
  const Profile& p = Profile::current();
  if (tz == nullptr || !p.enabled) return r;

  const ResolvedTimeZone& resolved = resolved_timezone();
  if (resolved.valid) {
    *tz = resolved.dtz;
    return dst_state_for(resolved.dtz);
  }

  if (p.time.has_bias) tz->Bias = p.time.bias_minutes;
  const std::wstring key = utf8_to_wide(p.time.timezone_windows_key);
  if (!key.empty()) {
    const size_t n = key.size() < 127 ? key.size() : 127;
    std::wmemcpy(tz->TimeZoneKeyName, key.c_str(), n);
    tz->TimeZoneKeyName[n] = L'\0';
    tz->DynamicDaylightTimeDisabled = FALSE;
  }
  return r;
}

BOOL WINAPI hook_GetTimeZoneInformationForYear(USHORT year, PDYNAMIC_TIME_ZONE_INFORMATION dtz,
                                               LPTIME_ZONE_INFORMATION tz) {
  if (real_GetTimeZoneInformationForYear == nullptr) return FALSE;
  const Profile& p = Profile::current();
  if (!p.enabled) return real_GetTimeZoneInformationForYear(year, dtz, tz);

  // Feed the real API the spoofed dynamic zone, so the returned transition dates and
  // biases are computed for the target zone rather than the host's.
  const ResolvedTimeZone& resolved = resolved_timezone();
  if (resolved.valid) {
    return real_GetTimeZoneInformationForYear(year, const_cast<PDYNAMIC_TIME_ZONE_INFORMATION>(
                                                         &resolved.dtz),
                                              tz);
  }
  const BOOL ok = real_GetTimeZoneInformationForYear(year, dtz, tz);
  if (ok && tz != nullptr && p.time.has_bias) tz->Bias = p.time.bias_minutes;
  return ok;
}

int WINAPI hook_GetUserDefaultLocaleName(LPWSTR buffer, int size) {
  const Profile& p = Profile::current();
  const std::wstring locale = utf8_to_wide(p.time.locale);
  if (p.enabled && !locale.empty() && buffer != nullptr && size > 0) {
    const int need = static_cast<int>(locale.size()) + 1;
    if (need > size) return 0;
    std::wmemcpy(buffer, locale.c_str(), static_cast<size_t>(need));
    return need;
  }
  if (real_GetUserDefaultLocaleName == nullptr) return 0;
  return real_GetUserDefaultLocaleName(buffer, size);
}

LANGID WINAPI hook_GetUserDefaultUILanguage() {
  const Profile& p = Profile::current();
  if (p.enabled && p.time.langid != 0) return static_cast<LANGID>(p.time.langid);
  return real_GetUserDefaultUILanguage ? real_GetUserDefaultUILanguage() : 0;
}

LANGID WINAPI hook_GetSystemDefaultUILanguage() {
  const Profile& p = Profile::current();
  if (p.enabled && p.time.langid != 0) return static_cast<LANGID>(p.time.langid);
  return real_GetSystemDefaultUILanguage ? real_GetSystemDefaultUILanguage() : 0;
}

// The ICU registry path. Matching on the value name alone is safe inside a browser
// process: nothing else in Chromium reads a value called TimeZoneKeyName.
LSTATUS WINAPI hook_RegQueryValueExW(HKEY key, LPCWSTR value_name, LPDWORD reserved,
                                     LPDWORD type, LPBYTE data, LPDWORD data_size) {
  if (real_RegQueryValueExW == nullptr) return ERROR_CALL_NOT_IMPLEMENTED;

  const Profile& p = Profile::current();
  const std::wstring target = utf8_to_wide(p.time.timezone_windows_key);
  if (!p.enabled || target.empty() || value_name == nullptr ||
      _wcsicmp(value_name, L"TimeZoneKeyName") != 0) {
    return real_RegQueryValueExW(key, value_name, reserved, type, data, data_size);
  }

  const DWORD bytes = static_cast<DWORD>((target.size() + 1) * sizeof(wchar_t));
  if (type != nullptr) *type = REG_SZ;
  if (data == nullptr) {
    if (data_size != nullptr) *data_size = bytes;
    return ERROR_SUCCESS;
  }
  if (data_size == nullptr || *data_size < bytes) {
    if (data_size != nullptr) *data_size = bytes;
    return ERROR_MORE_DATA;
  }
  std::memcpy(data, target.c_str(), bytes);
  *data_size = bytes;
  return ERROR_SUCCESS;
}

}  // namespace

void install_time_hooks() {
  install_hook_export("kernel32.dll", "GetTimeZoneInformation",
                      reinterpret_cast<void*>(hook_GetTimeZoneInformation),
                      reinterpret_cast<void**>(&real_GetTimeZoneInformation));
  install_hook_export("kernel32.dll", "GetDynamicTimeZoneInformation",
                      reinterpret_cast<void*>(hook_GetDynamicTimeZoneInformation),
                      reinterpret_cast<void**>(&real_GetDynamicTimeZoneInformation));
  install_hook_export("kernel32.dll", "GetTimeZoneInformationForYear",
                      reinterpret_cast<void*>(hook_GetTimeZoneInformationForYear),
                      reinterpret_cast<void**>(&real_GetTimeZoneInformationForYear));
  install_hook_export("kernel32.dll", "GetUserDefaultLocaleName",
                      reinterpret_cast<void*>(hook_GetUserDefaultLocaleName),
                      reinterpret_cast<void**>(&real_GetUserDefaultLocaleName));
  install_hook_export("kernel32.dll", "GetUserDefaultUILanguage",
                      reinterpret_cast<void*>(hook_GetUserDefaultUILanguage),
                      reinterpret_cast<void**>(&real_GetUserDefaultUILanguage));
  install_hook_export("kernel32.dll", "GetSystemDefaultUILanguage",
                      reinterpret_cast<void*>(hook_GetSystemDefaultUILanguage),
                      reinterpret_cast<void**>(&real_GetSystemDefaultUILanguage));
  install_hook_export("advapi32.dll", "RegQueryValueExW",
                      reinterpret_cast<void*>(hook_RegQueryValueExW),
                      reinterpret_cast<void**>(&real_RegQueryValueExW));
}

}  // namespace ghost
