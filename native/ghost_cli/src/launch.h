// Launching a process with the native shim already inside it.
//
// The order matters and is not an implementation detail:
//   1. CreateProcess(CREATE_SUSPENDED) so the target has not run a single
//      instruction of its own startup.
//   2. Inject and wait for the shim to finish installing hooks. LoadLibraryW
//      returns as soon as DllMain returns, but the shim installs hooks on a
//      background thread; resuming early would let Chromium cache the real
//      hardware values before the hooks exist.
//   3. ResumeThread. Only now does the browser start.
#pragma once

#include <string>
#include <vector>

namespace ghost {

struct LaunchOptions {
  std::string shim_dll;      // absolute path to ghost_shim.dll
  std::string profile_json;  // empty means the shim runs as a pass-through
  std::string timezone;      // IANA name; also exported as TZ for ICU
  std::string language;      // BCP-47 tag; appended as --lang when absent
  std::string exe;
  std::vector<std::string> args;
  // When set, the target's stdout and stderr are redirected into this file. The
  // self-test uses it instead of a pipe: a pipe would need the read end to be
  // inherited, and inherited handles in a restricted child are exactly what the
  // sandbox exists to prevent.
  std::string capture_to;
  bool wait = true;
  bool verbose = false;
  bool allow_unspoofed = false;

  // Whether the target may inherit this process's stdio handles. A browser
  // outlives its launcher, so handing it the caller's pipe would keep that pipe
  // open for as long as the browser runs — `ghost browse | anything` would never
  // finish. Long-lived targets therefore get no inherited handles.
  bool forward_stdio = true;
};

// Returns the target's exit code. Codes 1-3 are the launcher's own failures:
// 1 = CreateProcess failed, 2 = the shim never became ready, 3 = bitness
// mismatch. All of them abort the launch unless allow_unspoofed is set.
int launch_under_shim(const LaunchOptions& options);

}  // namespace ghost
