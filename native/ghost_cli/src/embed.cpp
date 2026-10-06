#include "embed.h"

#include <windows.h>

#include <cstdio>

namespace ghost {
namespace {

uint64_t fnv1a(const void* data, size_t size) {
  const unsigned char* p = static_cast<const unsigned char*>(data);
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < size; ++i) {
    h ^= p[i];
    h *= 1099511628211ull;
  }
  return h;
}

std::string hex16(uint64_t v) {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
  return std::string(buf);
}

}  // namespace

std::wstring widen(const std::string& s) {
  if (s.empty()) return std::wstring();
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                    nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

std::string narrow(const std::wstring& w) {
  if (w.empty()) return std::string();
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                    nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n,
                      nullptr, nullptr);
  return s;
}

std::string join_path(const std::string& a, const std::string& b) {
  if (a.empty()) return b;
  if (a.back() == '\\' || a.back() == '/') return a + b;
  return a + "\\" + b;
}

std::string exe_path() {
  wchar_t buf[MAX_PATH * 4];
  const DWORD n = GetModuleFileNameW(nullptr, buf,
                                     static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
  if (n == 0) return std::string();
  return narrow(std::wstring(buf, n));
}

std::string exe_dir() {
  const std::string p = exe_path();
  const size_t slash = p.find_last_of("\\/");
  return slash == std::string::npos ? std::string(".") : p.substr(0, slash);
}

bool make_dirs(const std::string& path) {
  const std::wstring w = widen(path);
  if (w.empty()) return false;
  // Walk the path and create every component that is missing. CreateDirectoryW
  // failing on an existing directory is not an error here.
  for (size_t i = 3; i <= w.size(); ++i) {
    if (i == w.size() || w[i] == L'\\' || w[i] == L'/') {
      const std::wstring part = w.substr(0, i);
      if (GetFileAttributesW(part.c_str()) == INVALID_FILE_ATTRIBUTES) {
        CreateDirectoryW(part.c_str(), nullptr);
      }
    }
  }
  return GetFileAttributesW(w.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::string cache_root() {
  wchar_t buf[MAX_PATH * 4];
  const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf,
                                          static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
  const std::string base =
      (n > 0 && n < sizeof(buf) / sizeof(buf[0])) ? narrow(std::wstring(buf, n)) : exe_dir();
  const std::string dir = join_path(base, "GhostBrowser");
  make_dirs(dir);
  return dir;
}

bool file_exists(const std::string& path) {
  const DWORD attrs = GetFileAttributesW(widen(path).c_str());
  return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool write_file(const std::string& path, const std::string& data) {
  HANDLE h = CreateFileW(widen(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  bool ok = true;
  if (!data.empty()) {
    ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
         written == data.size();
  }
  CloseHandle(h);
  return ok;
}

std::string read_file(const std::string& path) {
  HANDLE h = CreateFileW(widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return std::string();
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(h, &size)) {
    CloseHandle(h);
    return std::string();
  }
  std::string data(static_cast<size_t>(size.QuadPart), '\0');
  DWORD read = 0;
  const bool ok = data.empty() ||
                  (ReadFile(h, data.data(), static_cast<DWORD>(data.size()), &read, nullptr) &&
                   read == data.size());
  CloseHandle(h);
  return ok ? data : std::string();
}

std::string resource_bytes(int id) {
  HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
  if (res == nullptr) return std::string();
  const DWORD size = SizeofResource(nullptr, res);
  HGLOBAL handle = LoadResource(nullptr, res);
  if (handle == nullptr) return std::string();
  const void* data = LockResource(handle);
  if (data == nullptr) return std::string();
  return std::string(static_cast<const char*>(data), size);
}

std::string extract_resource(int id, const char* filename) {
  const std::string bytes = resource_bytes(id);
  if (bytes.empty()) return std::string();

  const std::string dir =
      join_path(join_path(cache_root(), "engine"), hex16(fnv1a(bytes.data(), bytes.size())));
  if (!make_dirs(dir)) return std::string();

  const std::string path = join_path(dir, filename);
  if (file_exists(path)) return path;

  // Write beside the target and rename, so a killed process cannot leave a
  // truncated DLL behind under the real name.
  const std::string tmp = path + ".tmp";
  if (!write_file(tmp, bytes)) return std::string();
  if (!MoveFileExW(widen(tmp).c_str(), widen(path).c_str(), MOVEFILE_REPLACE_EXISTING)) {
    DeleteFileW(widen(tmp).c_str());
    return std::string();
  }
  return path;
}

}  // namespace ghost
