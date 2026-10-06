#include "launch.h"

#include <windows.h>

#include <cstdio>

#include "../../common/inject.h"
#include "embed.h"

namespace ghost {
namespace {

std::string quote(const std::string& s) { return "\"" + s + "\""; }

}  // namespace

int launch_under_shim(const LaunchOptions& options) {
  if (options.exe.empty()) {
    std::fprintf(stderr, "ghost: no target executable\n");
    return 64;
  }
  if (options.shim_dll.empty()) {
    std::fprintf(stderr, "ghost: no shim available\n");
    return 65;
  }

  // The environment is the configuration transport. Every Chromium child
  // inherits it, so handing out the profile needs no IPC and no hook of its own.
  if (!options.profile_json.empty()) {
    SetEnvironmentVariableA("GHOST_PROFILE_JSON", options.profile_json.c_str());
  }
  // ICU consults TZ before it falls back to the Windows registry.
  if (!options.timezone.empty()) {
    SetEnvironmentVariableA("TZ", options.timezone.c_str());
  }
  // Read by the shim's branding hooks. The engine's compiled-in product name
  // cannot be changed, but the window title, icon and taskbar identity can.
  if (!options.brand_name.empty()) {
    SetEnvironmentVariableA("GHOST_BRAND_NAME", options.brand_name.c_str());
  }

  std::string command_line = quote(options.exe);
  for (const std::string& a : options.args) command_line += " " + quote(a);

  // Chromium resolves its application locale — and therefore Intl's default
  // locale and navigator.language — from the --lang switch, and it does so
  // before the shim's hooks are live. Hooking GetUserDefaultLocaleName alone
  // leaves navigator.language and date formatting on the host locale.
  if (!options.language.empty() && command_line.find("--lang=") == std::string::npos) {
    command_line += " --lang=" + options.language;
  }

  if (options.verbose) {
    std::printf("shim        : %s\n", options.shim_dll.c_str());
    std::printf("target      : %s\n", command_line.c_str());
    std::printf("profile     : %zu bytes\n", options.profile_json.size());
    std::printf("TZ          : %s\n", options.timezone.c_str());
  }

  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);

  // An inheritable file handle rather than a pipe: the child writes its output
  // straight to disk, so nothing has to be read back through a pipe that the
  // sandboxed child may not be allowed to open.
  HANDLE capture = INVALID_HANDLE_VALUE;
  if (!options.capture_to.empty()) {
    SECURITY_ATTRIBUTES attrs{};
    attrs.nLength = sizeof(attrs);
    attrs.bInheritHandle = TRUE;
    capture = CreateFileW(widen(options.capture_to).c_str(), GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, &attrs, CREATE_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
    if (capture == INVALID_HANDLE_VALUE) {
      std::fprintf(stderr, "ghost: cannot open capture file: %lu\n", GetLastError());
      return 1;
    }
    startup.dwFlags |= STARTF_USESTDHANDLES;
    startup.hStdOutput = capture;
    startup.hStdError = capture;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  }

  PROCESS_INFORMATION process{};

  // Handle inheritance is only about handles. The child's environment comes from
  // lpEnvironment (nullptr = inherit ours) whether or not handles are inherited,
  // so the profile reaches the target either way.
  const BOOL inherit =
      (capture != INVALID_HANDLE_VALUE || options.forward_stdio) ? TRUE : FALSE;
  if (CreateProcessA(nullptr, command_line.data(), nullptr, nullptr, inherit,
                     CREATE_SUSPENDED, nullptr, nullptr, &startup, &process) == FALSE) {
    const DWORD error = GetLastError();
    std::fprintf(stderr, "ghost: CreateProcess failed: %lu\n", error);
    // 225 = ERROR_VIRUS_INFECTED. "Suspend a process, write a DLL path into it,
    // start a remote thread" is the shape of a loader, so antivirus heuristics
    // do flag the shim. The fix is an exclusion, not a retry.
    if (error == 225) {
      std::fprintf(stderr,
                   "ghost: antivirus blocked the launch. Add an exclusion for %s\n"
                   "       (see docs/USAGE.md, Troubleshooting).\n",
                   options.shim_dll.c_str());
    }
    return 1;
  }

  if (options.verbose) std::printf("suspended pid: %lu\n", process.dwProcessId);
  if (options.pid_out != nullptr) *options.pid_out = process.dwProcessId;

  const auto release_capture = [&]() {
    if (capture != INVALID_HANDLE_VALUE) {
      CloseHandle(capture);
      capture = INVALID_HANDLE_VALUE;
    }
  };

  const auto abort_with = [&](int code) {
    TerminateProcess(process.hProcess, 1);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    release_capture();
    return code;
  };

  // Aborting is the default. A browser that quietly runs with real hardware
  // values is worse than one that refuses to start, because the caller would
  // believe it is protected.
  if (!is_injectable_target(process.hProcess)) {
    std::fprintf(stderr, "ghost: target bitness mismatch; injection impossible\n");
    if (!options.allow_unspoofed) return abort_with(3);
    std::fprintf(stderr, "ghost: --allow-unspoofed set; launching WITHOUT spoofing\n");
  } else {
    bool injected = false;
    int hooks = -1;
    int stage = -1;
    DWORD error = 0;
    const std::wstring shim = widen(options.shim_dll);
    const bool ready = inject_and_wait(process.hProcess, process.dwProcessId, shim.c_str(),
                                       30000, &injected, &hooks, &stage, &error);
    if (!ready) {
      // stage: 0 = export RVA unreadable, 1 = LoadLibraryW unresolvable,
      //        2 = block allocation failed, 3 = block/stub write failed,
      //        4 = no remote thread could be started, 5 = success,
      //        100+n = the stub ran but never published a result
      std::fprintf(stderr,
                   "ghost: readiness handshake failed "
                   "(injected=%d hooks=%d stage=%d err=%lu)\n",
                   injected ? 1 : 0, hooks, stage, error);
      if (!options.allow_unspoofed) return abort_with(2);
      std::fprintf(stderr, "ghost: --allow-unspoofed set; launching WITHOUT spoofing\n");
    } else if (options.verbose) {
      std::printf("shim ready (hooks=%d)\n", hooks);
    }
  }

  ResumeThread(process.hThread);
  // The child owns the write handle now; closing ours lets the file flush and
  // close as soon as the child exits.
  release_capture();

  if (!options.wait) {
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return 0;
  }

  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exit_code = 0;
  GetExitCodeProcess(process.hProcess, &exit_code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  if (options.verbose) std::printf("target exited with %lu\n", exit_code);
  return static_cast<int>(exit_code);
}

}  // namespace ghost
