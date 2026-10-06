#include "hook_engine.h"

#include <windows.h>

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "MinHook.h"

namespace ghost {
namespace {

HookStats g_stats;
std::mutex g_stats_mu;
bool g_initialised = false;
void* g_shim_module = nullptr;

// Records (hook target, trampoline slot) pairs so install_hook can reject aliasing.
std::vector<std::pair<void*, void**>> g_bindings;

// Readiness handshake state. Deliberately plain process-local objects: no named
// kernel object means no security descriptor, no integrity label, and no way for
// Chromium's sandbox to deny the child its own signal.
std::mutex g_ready_mu;
std::condition_variable g_ready_cv;
bool g_ready = false;
int g_ready_installed = 0;

// Caller must hold g_stats_mu.
void record_failure_locked(const std::string& what) {
  g_stats.failed++;
  g_stats.failures.push_back(what);
}

// Re-entrancy guard for the logger: a hook that logs while the logger itself is
// running (e.g. the logger touching a hooked API) must not recurse forever.
thread_local bool t_in_log = false;

std::string log_path() {
  char dir[MAX_PATH] = {0};
  if (GetTempPathA(MAX_PATH, dir) == 0) return "ghost_shim.log";
  return std::string(dir) + "ghost_shim.log";
}

void record_failure(const std::string& what) {
  std::lock_guard<std::mutex> lock(g_stats_mu);
  record_failure_locked(what);
}

}  // namespace

void ghost_log(const char* fmt, ...) {
  if (t_in_log) return;
  t_in_log = true;

  const std::string path = log_path();
  FILE* f = nullptr;
  if (fopen_s(&f, path.c_str(), "a") == 0 && f != nullptr) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d.%03d pid=%lu] ", st.wYear, st.wMonth,
            st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            GetCurrentProcessId());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
  }

  t_in_log = false;
}

const HookStats& stats() { return g_stats; }

void set_shim_module(void* module) { g_shim_module = module; }
void* shim_module() { return g_shim_module; }

bool hook_engine_init() {
  if (g_initialised) return true;
  const MH_STATUS s = MH_Initialize();
  if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
    ghost_log("MH_Initialize failed: %s", MH_StatusToString(s));
    return false;
  }
  g_initialised = true;
  return true;
}

bool install_hook(const char* name, void* target, void* detour, void** trampoline) {
  if (!g_initialised) hook_engine_init();

  if (target == nullptr) {
    record_failure(std::string(name) + ": unresolved target");
    return false;
  }

  // Guard against the trampoline aliasing bug, which is silent and fatal.
  //
  // If two hook targets share one trampoline slot, the second install overwrites the
  // first's trampoline. When one target forwards to the other — exactly the kernel32
  // -> kernelbase export-forwarder relationship — the detour calls the wrong
  // trampoline, lands back on the patched target, and recurses until the stack
  // overflows (observed in the wild as exit code 0xC00000FD).
  //
  // Refusing the install turns a stack overflow into a logged, ignorable failure,
  // which is the behaviour this whole engine is built around.
  {
    std::lock_guard<std::mutex> lock(g_stats_mu);
    for (const auto& binding : g_bindings) {
      if (binding.first == target && binding.second != trampoline) {
        record_failure_locked(std::string(name) +
                              ": target already hooked through a different trampoline");
        return false;
      }
      if (binding.second == trampoline && trampoline != nullptr &&
          binding.first != target) {
        record_failure_locked(std::string(name) +
                              ": trampoline slot already bound to another target "
                              "(would alias and recurse)");
        return false;
      }
    }
    g_bindings.emplace_back(target, trampoline);
  }

  if (trampoline != nullptr) *trampoline = nullptr;

  const MH_STATUS s = MH_CreateHook(target, detour, trampoline);
  if (s != MH_OK) {
    record_failure(std::string(name) + ": " + MH_StatusToString(s));
    return false;
  }
  if (trampoline != nullptr && *trampoline == nullptr) {
    record_failure(std::string(name) + ": null trampoline after create");
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(g_stats_mu);
    g_stats.installed++;
  }
  ghost_log("hook ok: %s (target=%p tramp=%p)", name, target,
            trampoline ? *trampoline : nullptr);
  return true;
}

bool install_hook_export(const char* dll, const char* symbol, void* detour,
                         void** trampoline) {
  HMODULE mod = GetModuleHandleA(dll);
  if (mod == nullptr) mod = LoadLibraryA(dll);

  const std::string name = std::string(dll) + "!" + symbol;
  if (mod == nullptr) {
    record_failure(name + ": module not loadable");
    return false;
  }

  void* target = reinterpret_cast<void*>(GetProcAddress(mod, symbol));
  return install_hook(name.c_str(), target, detour, trampoline);
}

bool install_hook_pattern(const char* name, void* module_base, size_t module_size,
                          const char* pattern, const char* mask, void* detour,
                          void** trampoline) {
  if (module_base == nullptr || module_size == 0) {
    record_failure(std::string(name) + ": empty module range");
    return false;
  }

  const size_t plen = std::strlen(mask);
  auto* bytes = static_cast<unsigned char*>(module_base);
  for (size_t i = 0; i + plen <= module_size; ++i) {
    bool hit = true;
    for (size_t k = 0; k < plen; ++k) {
      if (mask[k] == '?') continue;
      if (bytes[i + k] != static_cast<unsigned char>(pattern[k])) {
        hit = false;
        break;
      }
    }
    if (hit) return install_hook(name, bytes + i, detour, trampoline);
  }

  record_failure(std::string(name) + ": pattern not found");
  return false;
}

void hook_engine_enable_all() {
  const MH_STATUS s = MH_EnableHook(MH_ALL_HOOKS);
  if (s != MH_OK) ghost_log("MH_EnableHook(MH_ALL_HOOKS) failed: %s", MH_StatusToString(s));
}

void mark_hooks_ready(int installed, int failed) {
  {
    std::lock_guard<std::mutex> lock(g_ready_mu);
    g_ready = true;
    g_ready_installed = installed;
  }
  (void)failed;
  g_ready_cv.notify_all();
}

int wait_hooks_ready(unsigned long timeout_ms) {
  std::unique_lock<std::mutex> lock(g_ready_mu);
  if (!g_ready_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                           [] { return g_ready; })) {
    return -1;
  }
  return g_ready_installed;
}

}  // namespace ghost
