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

  // DevTools over anonymous pipes.
  //
  // Chrome's usual transport is a TCP port plus a `DevToolsActivePort` file in
  // the profile, and both are discoverable from inside the browser's own user
  // account. On Windows there is a second one: `--remote-debugging-pipe` speaks
  // the protocol on file descriptors 3 and 4, and `--remote-debugging-io-pipes`
  // replaces those descriptors with two handle values the launcher picks
  // (content_switches.cc). When that switch is present Chrome skips the
  // descriptor check (chrome_main_delegate.cc:1221-1233), which is the only
  // reason a launcher can supply handles of its own.
  //
  // Handle inheritance preserves the numeric value, so the child ends are passed
  // on the command line exactly as created. Only the child's ends are made
  // inheritable: an inherited parent end would keep the pipe open inside the
  // browser and an end of stream would never arrive.
  HANDLE cdp_child_read = nullptr;
  HANDLE cdp_child_write = nullptr;
  HANDLE cdp_parent_read = nullptr;
  HANDLE cdp_parent_write = nullptr;
  if (options.cdp) {
    SECURITY_ATTRIBUTES attrs{};
    attrs.nLength = sizeof(attrs);
    attrs.bInheritHandle = TRUE;
    if (CreatePipe(&cdp_child_read, &cdp_parent_write, &attrs, 0) == FALSE ||
        CreatePipe(&cdp_parent_read, &cdp_child_write, &attrs, 0) == FALSE) {
      std::fprintf(stderr, "ghost: cannot create the DevTools pipes: %lu\n",
                   GetLastError());
      return 1;
    }
    SetHandleInformation(cdp_parent_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(cdp_parent_write, HANDLE_FLAG_INHERIT, 0);
    command_line += " --remote-debugging-pipe --remote-debugging-io-pipes=" +
                    std::to_string(static_cast<unsigned long long>(
                        reinterpret_cast<ULONG_PTR>(cdp_child_read))) +
                    "," +
                    std::to_string(static_cast<unsigned long long>(
                        reinterpret_cast<ULONG_PTR>(cdp_child_write)));

    // Opening the pipe also switches on Blink's AutomationControlled feature, so
    // the browser starts telling every page it is automated: `navigator.webdriver`
    // becomes true. That is the single most widely checked automation tell there
    // is, and it is true *only* because the pipe is open -- measured directly, the
    // same build reports false under --no-cdp. The pipe and the cure therefore
    // travel together.
    //
    // Chrome honours one occurrence of the switch, so the feature is merged into
    // whatever the caller already passed instead of being appended as a second
    // --disable-blink-features.
    const std::string blink_switch = "--disable-blink-features=";
    const std::string blink_feature = "AutomationControlled";
    const std::size_t blink_at = command_line.find(blink_switch);
    if (blink_at == std::string::npos) {
      command_line += " " + blink_switch + blink_feature;
    } else {
      std::size_t blink_end = command_line.find(' ', blink_at);
      if (blink_end == std::string::npos) blink_end = command_line.size();
      if (command_line.substr(blink_at, blink_end - blink_at).find(blink_feature) ==
          std::string::npos) {
        command_line.insert(blink_end, "," + blink_feature);
      }
    }
  }

  const auto release_cdp = [&]() {
    if (cdp_child_read != nullptr) {
      CloseHandle(cdp_child_read);
      cdp_child_read = nullptr;
    }
    if (cdp_child_write != nullptr) {
      CloseHandle(cdp_child_write);
      cdp_child_write = nullptr;
    }
    if (cdp_parent_read != nullptr) {
      CloseHandle(cdp_parent_read);
      cdp_parent_read = nullptr;
    }
    if (cdp_parent_write != nullptr) {
      CloseHandle(cdp_parent_write);
      cdp_parent_write = nullptr;
    }
  };

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
  const BOOL inherit = (capture != INVALID_HANDLE_VALUE || options.forward_stdio ||
                        options.cdp)
                           ? TRUE
                           : FALSE;
  if (CreateProcessA(nullptr, command_line.data(), nullptr, nullptr, inherit,
                     CREATE_SUSPENDED, nullptr, nullptr, &startup, &process) == FALSE) {
    const DWORD error = GetLastError();
    std::fprintf(stderr, "ghost: CreateProcess failed: %lu\n", error);
    release_cdp();
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

  if (options.cdp) {
    // The browser owns the child ends now. Closing ours in this process is what
    // makes the pipe report an end of stream when the browser exits, rather than
    // leaving a reader blocked on a handle only this dead launcher still holds.
    if (cdp_child_read != nullptr) {
      CloseHandle(cdp_child_read);
      cdp_child_read = nullptr;
    }
    if (cdp_child_write != nullptr) {
      CloseHandle(cdp_child_write);
      cdp_child_write = nullptr;
    }
    if (options.cdp_read_out != nullptr) *options.cdp_read_out = cdp_parent_read;
    if (options.cdp_write_out != nullptr) *options.cdp_write_out = cdp_parent_write;
    // Ownership moved to the caller; release_cdp must not close them.
    cdp_parent_read = nullptr;
    cdp_parent_write = nullptr;
  }

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
    release_cdp();
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
