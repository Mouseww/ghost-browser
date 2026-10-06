// dllmain.cpp — shim entry point.
//
// CRITICAL: never install a hook from DllMain. DllMain runs while the loader lock is
// held; anything that touches the loader from there — including the LoadLibrary calls
// MinHook performs to resolve trampoline targets — deadlocks the process. Every hook
// installation therefore happens on a separate thread started here.
//
// Second rule: a failed hook must never take the process down. If the shim cannot
// install a hook it logs and continues, because a browser that fails to launch is far
// more conspicuous than a browser with an imperfect fingerprint.
#include <windows.h>

#include <cstdlib>
#include <string>

#include "../../common/inject.h"
#include "../include/ghost_profile.h"
#include "hook_engine.h"

namespace ghost {
void install_sysinfo_hooks();
void install_display_hooks();
void install_time_hooks();
void install_gpu_hooks();
void install_proc_hooks();
void install_brand_hooks();
}  // namespace ghost

namespace {

DWORD WINAPI init_thread(LPVOID) {
  ghost::hook_engine_init();

  const ghost::Profile& profile = ghost::Profile::current();
  ghost::ghost_log("=== shim attach pid=%lu enabled=%d profile='%s' ===",
                   GetCurrentProcessId(), profile.enabled ? 1 : 0,
                   profile.profile_id.c_str());

  if (!profile.enabled) {
    ghost::ghost_log("no usable profile in GHOST_PROFILE_JSON; shim is pass-through");
    ghost::mark_hooks_ready(0, 0);
    return 0;
  }

  // GHOST_HOOK_MASK lets a hook group be switched off without a rebuild, so a group
  // that destabilises the host can be isolated in one run. Bit 0 sysinfo, 1 display,
  // 2 time, 3 gpu, 4 process-creation, 5 branding. Absent or unparsable means
  // "install everything".
  unsigned long mask = 0xFFFFFFFFul;
  wchar_t mask_text[32] = {0};
  if (GetEnvironmentVariableW(L"GHOST_HOOK_MASK", mask_text, 32) > 0) {
    mask = std::wcstoul(mask_text, nullptr, 16);
    ghost::ghost_log("hook mask override: 0x%lX", mask);
  }

  if ((mask & 0x01ul) != 0) ghost::install_sysinfo_hooks();
  if ((mask & 0x02ul) != 0) ghost::install_display_hooks();
  if ((mask & 0x04ul) != 0) ghost::install_time_hooks();
  if ((mask & 0x08ul) != 0) ghost::install_gpu_hooks();
  if ((mask & 0x10ul) != 0) ghost::install_proc_hooks();
  if ((mask & 0x20ul) != 0) ghost::install_brand_hooks();
  ghost::hook_engine_enable_all();

  const ghost::HookStats& s = ghost::stats();
  ghost::ghost_log("hooks installed=%d failed=%d", s.installed, s.failed);
  for (const std::string& failure : s.failures) {
    ghost::ghost_log("  hook FAILED: %s", failure.c_str());
  }

  // Release the injector, which is holding this process suspended.
  ghost::mark_hooks_ready(s.installed, s.failed);
  return 0;
}

}  // namespace

// Called by the injector on a remote thread inside this process, so that readiness is
// reported without any named object. The exit code carries the installed-hook count
// biased by one, so that 0 unambiguously means "never became ready" even for a
// pass-through shim that legitimately installs zero hooks.
extern "C" __declspec(dllexport) DWORD WINAPI ghost_shim_ready_probe(LPVOID) {
  const int installed = ghost::wait_hooks_ready(20000);
  if (installed < 0) return 0;
  return static_cast<DWORD>(installed) + 1;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(module);
    // Remember our own module: GetModuleFileNameW(nullptr, ...) would return the host
    // EXE, and propagation must inject THIS dll into children.
    ghost::set_shim_module(module);
    // Must not block: just spawn the worker and return so the loader can proceed.
    HANDLE worker = CreateThread(nullptr, 0, init_thread, nullptr, 0, nullptr);
    if (worker != nullptr) CloseHandle(worker);
  }
  return TRUE;
}
