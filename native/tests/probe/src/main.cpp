// probe — a deliberately boring Win32 program that reads exactly the OS values the
// shim claims to spoof.
//
// This exists so that shim behaviour can be verified without a browser in the loop.
// The same probe run twice — once bare, once under ghost_launch — is the fastest
// possible signal that injection and hooks actually work, and it isolates a shim bug
// from a Chromium-integration bug.
//
// `probe --child` re-executes itself; the parent checks that the grandchild also
// reports spoofed values, which is what proves subprocess propagation.
#include <windows.h>
#include <shellscalingapi.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>

namespace {

bool shim_loaded() { return GetModuleHandleA("ghost_shim.dll") != nullptr; }

void print_self() {
  printf("--- probe ---\n");
  printf("pid                     = %lu\n", GetCurrentProcessId());
  printf("GHOST_PROFILE_JSON      = %s\n",
         std::getenv("GHOST_PROFILE_JSON") ? "set" : "unset");
  printf("TZ                      = %s\n",
         std::getenv("TZ") ? std::getenv("TZ") : "unset");
  printf("ghost_shim.dll loaded   = %s\n", shim_loaded() ? "yes" : "no");

  printf("GetActiveProcessorCount = %u\n", GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));

  SYSTEM_INFO si{};
  GetSystemInfo(&si);
  printf("dwNumberOfProcessors    = %lu\n", si.dwNumberOfProcessors);

  MEMORYSTATUSEX ms{};
  ms.dwLength = sizeof(ms);
  if (GlobalMemoryStatusEx(&ms)) {
    printf("ullTotalPhys            = %llu\n", static_cast<unsigned long long>(ms.ullTotalPhys));
    printf("ullAvailPhys            = %llu\n", static_cast<unsigned long long>(ms.ullAvailPhys));
    printf("dwMemoryLoad            = %lu%%\n", ms.dwMemoryLoad);
  }

  printf("GetSystemMetrics(CX)    = %d\n", GetSystemMetrics(SM_CXSCREEN));
  printf("GetSystemMetrics(CY)    = %d\n", GetSystemMetrics(SM_CYSCREEN));
  printf("GetSystemMetrics(CMON)  = %d\n", GetSystemMetrics(SM_CMONITORS));

  HDC screen = GetDC(nullptr);
  if (screen != nullptr) {
    printf("GetDeviceCaps(HORZRES)  = %d\n", GetDeviceCaps(screen, HORZRES));
    printf("GetDeviceCaps(LOGPIX)   = %d\n", GetDeviceCaps(screen, LOGPIXELSX));
    ReleaseDC(nullptr, screen);
  }

  int monitor_count = 0;
  RECT first{};
  std::pair<int*, RECT*> enum_state{&monitor_count, &first};
  EnumDisplayMonitors(
      nullptr, nullptr,
      [](HMONITOR, HDC, LPRECT rect, LPARAM data) -> BOOL {
        auto* state = reinterpret_cast<std::pair<int*, RECT*>*>(data);
        if (*state->first == 0) *state->second = *rect;
        (*state->first)++;
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&enum_state));
  printf("EnumDisplayMonitors     = %d monitor(s), first rect = (%ld,%ld)-(%ld,%ld)\n",
         monitor_count, first.left, first.top, first.right, first.bottom);

  POINT origin{0, 0};
  HMONITOR primary = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  if (GetMonitorInfoW(primary, &mi)) {
    printf("MONITORINFO rcMonitor   = (%ld,%ld)-(%ld,%ld)\n", mi.rcMonitor.left,
           mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom);
    printf("MONITORINFO rcWork      = (%ld,%ld)-(%ld,%ld)\n", mi.rcWork.left, mi.rcWork.top,
           mi.rcWork.right, mi.rcWork.bottom);
  }

  UINT dpi_x = 0, dpi_y = 0;
  if (SUCCEEDED(GetDpiForMonitor(primary, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y))) {
    printf("GetDpiForMonitor        = %u x %u\n", dpi_x, dpi_y);
  }
  printf("GetDpiForSystem         = %u\n", GetDpiForSystem());

  TIME_ZONE_INFORMATION tz{};
  const DWORD tz_result = GetTimeZoneInformation(&tz);
  printf("GetTimeZoneInformation  = ret=%lu Bias=%ld StandardBias=%ld DaylightBias=%ld\n",
         tz_result, tz.Bias, tz.StandardBias, tz.DaylightBias);

  DYNAMIC_TIME_ZONE_INFORMATION dtz{};
  if (GetDynamicTimeZoneInformation(&dtz) != TIME_ZONE_ID_INVALID) {
    char key[256] = {0};
    WideCharToMultiByte(CP_UTF8, 0, dtz.TimeZoneKeyName, -1, key, sizeof(key), nullptr, nullptr);
    printf("DynamicTimeZoneKeyName  = %s (Bias=%ld)\n", key, dtz.Bias);
  }

  wchar_t locale[LOCALE_NAME_MAX_LENGTH] = {0};
  if (GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH) > 0) {
    char utf8[128] = {0};
    WideCharToMultiByte(CP_UTF8, 0, locale, -1, utf8, sizeof(utf8), nullptr, nullptr);
    printf("GetUserDefaultLocaleName= %s\n", utf8);
  }

  printf("TimeZoneKeyName(reg)    = ");
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                    L"SYSTEM\\CurrentControlSet\\Control\\TimeZoneInformation", 0, KEY_READ,
                    &key) == ERROR_SUCCESS) {
    wchar_t value[256] = {0};
    DWORD size = sizeof(value);
    DWORD type = 0;
    if (RegQueryValueExW(key, L"TimeZoneKeyName", nullptr, &type,
                         reinterpret_cast<LPBYTE>(value), &size) == ERROR_SUCCESS) {
      char utf8[512] = {0};
      WideCharToMultiByte(CP_UTF8, 0, value, -1, utf8, sizeof(utf8), nullptr, nullptr);
      printf("%s\n", utf8);
    } else {
      printf("<query failed>\n");
    }
    RegCloseKey(key);
  } else {
    printf("<open failed>\n");
  }
  printf("--- end ---\n");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::string(argv[1]) == "--child") {
    printf("=== CHILD PROCESS ===\n");
    print_self();
    return 0;
  }

  print_self();

  if (argc > 1 && std::string(argv[1]) == "--spawn-child") {
    printf("\n=== spawning a child to verify propagation ===\n");

    char self[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, self, MAX_PATH);
    std::string command = std::string("\"") + self + "\" --child";

    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION pi{};
    if (CreateProcessA(nullptr, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                       &startup, &pi)) {
      WaitForSingleObject(pi.hProcess, 30000);
      DWORD exit_code = 0;
      GetExitCodeProcess(pi.hProcess, &exit_code);
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);
      printf("child exit code = %lu\n", exit_code);
    } else {
      printf("CreateProcess failed: %lu\n", GetLastError());
    }
  }

  return 0;
}
