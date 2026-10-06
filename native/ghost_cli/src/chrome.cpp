#include "chrome.h"

#include <windows.h>
#include <winver.h>

#include "embed.h"

namespace ghost {
namespace {

std::string reg_string(HKEY root, const wchar_t* subkey, const wchar_t* value) {
  wchar_t buf[MAX_PATH * 4];
  DWORD size = sizeof(buf);
  DWORD type = 0;
  const LSTATUS status = RegGetValueW(root, subkey, value, RRF_RT_REG_SZ, &type, buf, &size);
  if (status != ERROR_SUCCESS) return std::string();
  return narrow(std::wstring(buf));
}

// Chrome records its install location here; the default value of the key is the
// executable itself. Both the 64-bit and 32-bit views matter, because a 64-bit
// Chrome still registers under WOW6432Node on some installs.
std::string app_paths(const wchar_t* exe_name) {
  const std::wstring sub =
      std::wstring(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\") + exe_name;
  for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
    const std::string p = reg_string(root, sub.c_str(), nullptr);
    if (!p.empty() && file_exists(p)) return p;
  }
  const std::wstring wow = std::wstring(L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\"
                                        L"CurrentVersion\\App Paths\\") +
                           exe_name;
  const std::string p = reg_string(HKEY_LOCAL_MACHINE, wow.c_str(), nullptr);
  return (file_exists(p)) ? p : std::string();
}

std::string under_env(const char* env, const char* rest) {
  const std::string base = env_var(env);
  if (base.empty()) return std::string();
  const std::string p = join_path(base, rest);
  return file_exists(p) ? p : std::string();
}

struct Candidate {
  const char* name;
  std::string path;
};

std::string first_existing(std::initializer_list<std::string> paths) {
  for (const std::string& p : paths) {
    if (!p.empty() && file_exists(p)) return p;
  }
  return std::string();
}

}  // namespace

std::string env_var(const char* name) {
  wchar_t buf[MAX_PATH * 4];
  const DWORD n = GetEnvironmentVariableW(widen(name).c_str(), buf,
                                          static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
  if (n == 0 || n >= sizeof(buf) / sizeof(buf[0])) return std::string();
  return narrow(std::wstring(buf, n));
}

std::string file_version(const std::string& path) {
  const std::wstring w = widen(path);
  DWORD handle = 0;
  const DWORD size = GetFileVersionInfoSizeW(w.c_str(), &handle);
  if (size == 0) return std::string();

  std::string block(size, '\0');
  if (!GetFileVersionInfoW(w.c_str(), 0, size, block.data())) return std::string();

  VS_FIXEDFILEINFO* info = nullptr;
  UINT info_size = 0;
  if (!VerQueryValueW(block.data(), L"\\", reinterpret_cast<LPVOID*>(&info), &info_size) ||
      info == nullptr) {
    return std::string();
  }

  char buf[64];
  std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", HIWORD(info->dwFileVersionMS),
                LOWORD(info->dwFileVersionMS), HIWORD(info->dwFileVersionLS),
                LOWORD(info->dwFileVersionLS));
  return std::string(buf);
}

BrowserInstall find_browser(const std::string& prefer) {
  if (!prefer.empty() && prefer != "chrome" && prefer != "edge") {
    BrowserInstall install;
    install.name = "custom";
    install.path = prefer;
    install.version = file_version(prefer);
    return install;
  }

  const bool edge_only = (prefer == "edge");

  if (!edge_only) {
    std::string p = app_paths(L"chrome.exe");
    if (p.empty()) {
      p = first_existing({
          under_env("ProgramFiles", "Google\\Chrome\\Application\\chrome.exe"),
          under_env("ProgramFiles(x86)", "Google\\Chrome\\Application\\chrome.exe"),
          under_env("LOCALAPPDATA", "Google\\Chrome\\Application\\chrome.exe"),
      });
    }
    if (!p.empty()) return BrowserInstall{"Chrome", p, file_version(p)};
  }

  std::string p = app_paths(L"msedge.exe");
  if (p.empty()) {
    p = first_existing({
        under_env("ProgramFiles(x86)", "Microsoft\\Edge\\Application\\msedge.exe"),
        under_env("ProgramFiles", "Microsoft\\Edge\\Application\\msedge.exe"),
    });
  }
  if (!p.empty()) return BrowserInstall{"Edge", p, file_version(p)};

  return BrowserInstall{};
}

}  // namespace ghost
