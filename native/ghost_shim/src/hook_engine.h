// hook_engine.h — thin, failure-tolerant wrapper around MinHook.
//
// Design rule: a hook that cannot be installed must NEVER crash or abort the
// process. If the shim fails, the browser still has to open and behave normally —
// a half-spoofed browser is bad, but a browser that will not start is worse and
// far more conspicuous.
#pragma once

#include <string>
#include <vector>

namespace ghost {

struct HookStats {
  int installed = 0;
  int failed = 0;
  std::vector<std::string> failures;
};

const HookStats& stats();

// Initialises MinHook. Safe to call once per process; returns false on failure.
bool hook_engine_init();

// Installs an inline hook. `target` may be null, in which case it is recorded as
// a failure rather than dereferenced.
bool install_hook(const char* name, void* target, void* detour, void** trampoline);

// Resolves dll!symbol and hooks it. The DLL is loaded if not already present.
bool install_hook_export(const char* dll, const char* symbol, void* detour,
                         void** trampoline);

// Installs a hook by address inside an already-loaded module, located by a byte
// pattern. Used for the tracks where no export exists (channel C).
bool install_hook_pattern(const char* name, void* module_base, size_t module_size,
                          const char* pattern, const char* mask, void* detour,
                          void** trampoline);

// MinHook requires enabling after all hooks are created.
void hook_engine_enable_all();

// ---------------------------------------------------------------------------
// Readiness handshake, entirely inside the child process.
//
// A named event does not survive Chromium's sandbox: the browser process creates it
// at medium integrity, while a sandboxed renderer or GPU process runs from a
// restricted token at low integrity and is denied write access, so OpenEventW fails
// inside the child and the injector blocks for its whole timeout on every child.
//
// Instead the injector calls ghost_shim_ready_probe() on a remote thread in the
// child; that function blocks here until the installer is done and returns the
// installed-hook count as the thread exit code. Nothing crosses a security boundary,
// and the count gives the injector visibility into sandboxed children whose log
// writes to %TEMP% are denied.
// ---------------------------------------------------------------------------
void mark_hooks_ready(int installed, int failed);

// Blocks until mark_hooks_ready() has run. Returns the installed-hook count, or -1
// if the timeout elapsed first.
int wait_hooks_ready(unsigned long timeout_ms);

// Writes a line to %TEMP%\ghost_shim.log. Always safe to call.
void ghost_log(const char* fmt, ...);

// ---------------------------------------------------------------------------
// The shim's own module handle.
//
// GetModuleFileNameW(nullptr, ...) returns the *host executable*, not the DLL whose
// code is running. Using it to locate the shim would make propagation inject the
// host EXE into every child process. DllMain stores the real handle here instead.
// ---------------------------------------------------------------------------
void set_shim_module(void* module);
void* shim_module();

}  // namespace ghost
