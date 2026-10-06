// Resources embedded into ghost.exe, plus the small filesystem helpers the
// launcher needs. Keeping the shim inside the executable is what makes a single
// downloaded file a complete product.
#pragma once

#include <cstdint>
#include <string>

namespace ghost {

// Resource ids. Kept in sync with ghost_cli/res/ghost.rc.in.
enum ResourceId {
  kResShimDll = 101,
};

std::string join_path(const std::string& a, const std::string& b);

// UTF-8 <-> UTF-16. Every Win32 W entry point needs these.
std::wstring widen(const std::string& s);
std::string narrow(const std::wstring& w);

// Absolute path of the running executable, and the directory holding it.
std::string exe_path();
std::string exe_dir();

// %LOCALAPPDATA%\GhostBrowser, created on demand. Falls back to the directory
// holding the executable when LOCALAPPDATA is unset.
std::string cache_root();

bool file_exists(const std::string& path);
bool make_dirs(const std::string& path);
bool write_file(const std::string& path, const std::string& data);
std::string read_file(const std::string& path);

// Raw bytes of an embedded RCDATA resource; empty when the resource is absent.
std::string resource_bytes(int id);

// Extract a resource into an engine directory named after its content hash and
// return the path. Because the directory name is the hash, a rebuild that
// changes the shim lands in a fresh directory and never reuses a stale file,
// while repeated runs of the same build write nothing at all.
std::string extract_resource(int id, const char* filename);

}  // namespace ghost
