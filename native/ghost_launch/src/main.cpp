// ghost_launch — seeds a browser process with the native shim.
//
// Sequence, and why each part is ordered the way it is:
//   1. CreateProcessW(..., CREATE_SUSPENDED)  — the target must not have run any of
//      its own startup code yet.
//   2. VirtualAllocEx + WriteProcessMemory + CreateRemoteThread(LoadLibraryW)
//      — classic remote injection; requires bInheritHandles=TRUE so the child also
//      receives the inherited environment (GHOST_PROFILE_JSON, TZ) and handles.
//   3. Wait for the shim's readiness event — LoadLibraryW returns when DllMain
//      returns, but the shim installs hooks on a background thread. Resuming early
//      would let Chromium cache real hardware values before the hooks exist.
//   4. ResumeThread — only now does the browser begin to execute.
//
// Usage:
//   ghost_launch [--dll <shim.dll>] [--profile <file.json> | --profile-json <json>]
//                [--tz <IANA>] [--no-wait] [--verbose] -- <exe> [args...]
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../common/inject.h"

namespace {

std::string read_file(const std::string& path) {
  FILE* f = nullptr;
  if (fopen_s(&f, path.c_str(), "rb") != 0 || f == nullptr) return std::string();
  std::string data;
  char buffer[8192];
  size_t read = 0;
  while ((read = fread(buffer, 1, sizeof(buffer), f)) > 0) data.append(buffer, read);
  fclose(f);
  return data;
}

std::string executable_dir() {
  char path[MAX_PATH] = {0};
  GetModuleFileNameA(nullptr, path, MAX_PATH);
  std::string s(path);
  const size_t slash = s.find_last_of("\\/");
  return slash == std::string::npos ? std::string(".") : s.substr(0, slash);
}

std::string quote(const std::string& s) { return "\"" + s + "\""; }

void usage() {
  fprintf(stderr,
          "usage: ghost_launch [--dll <shim.dll>] [--profile <file.json>]\n"
          "                    [--profile-json <json>] [--tz <IANA>] [--lang <tag>]\n"
          "                    [--no-wait] [--verbose] -- <exe> [args...]\n");
}

}  // namespace

int main(int argc, char** argv) {
  std::string dll;
  std::string profile_json;
  std::string timezone;
  std::string language;
  std::string exe;
  std::vector<std::string> target_args;
  bool wait = true;
  bool verbose = false;
  bool allow_unspoofed = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--dll" && i + 1 < argc) {
      dll = argv[++i];
    } else if (arg == "--profile" && i + 1 < argc) {
      profile_json = read_file(argv[++i]);
    } else if (arg == "--profile-json" && i + 1 < argc) {
      profile_json = argv[++i];
    } else if (arg == "--tz" && i + 1 < argc) {
      timezone = argv[++i];
    } else if (arg == "--lang" && i + 1 < argc) {
      language = argv[++i];
    } else if (arg == "--no-wait") {
      wait = false;
    } else if (arg == "--allow-unspoofed") {
      allow_unspoofed = true;
    } else if (arg == "--verbose") {
      verbose = true;
    } else if (arg == "--") {
      if (i + 1 < argc) exe = argv[++i];
      for (int j = i + 1; j < argc; ++j) target_args.push_back(argv[j]);
      break;
    } else if (exe.empty()) {
      exe = arg;
    } else {
      target_args.push_back(arg);
    }
  }

  if (exe.empty()) {
    usage();
    return 64;
  }

  if (dll.empty()) dll = executable_dir() + "\\ghost_shim.dll";
  if (profile_json.empty()) {
    fprintf(stderr, "ghost_launch: no --profile/--profile-json given; "
                    "the shim will run as a pass-through\n");
  }

  // Environment is the profile transport: every Chromium child inherits it, so no
  // IPC and no CreateProcess hook is needed just to hand out configuration.
  if (!profile_json.empty()) SetEnvironmentVariableA("GHOST_PROFILE_JSON", profile_json.c_str());
  // ICU consults TZ before falling back to the Windows registry.
  if (!timezone.empty()) SetEnvironmentVariableA("TZ", timezone.c_str());

  std::string command_line = quote(exe);
  for (const std::string& a : target_args) command_line += " " + quote(a);

  // Chromium takes its application locale — and therefore Intl's default locale and
  // navigator.language — from the --lang switch. Hooking GetUserDefaultLocaleName is not
  // enough: the engine resolves and caches the application locale before the shim's hooks
  // are live, so without this switch navigator.language reports the host locale and Intl
  // formats dates in the host locale as well (an easily detected incoherence). Appending
  // it here keeps the profile's locale authoritative.
  if (!language.empty() && command_line.find("--lang=") == std::string::npos) {
    command_line += " --lang=" + quote(language);
  }

  if (verbose) {
    printf("shim        : %s\n", dll.c_str());
    printf("target      : %s\n", command_line.c_str());
    printf("profile len : %zu bytes\n", profile_json.size());
    printf("TZ          : %s\n", timezone.c_str());
  }

  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};

  // bInheritHandles must be TRUE: without it the child does not receive the
  // inherited environment block and the profile never reaches it.
  if (CreateProcessA(nullptr, command_line.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED,
                     nullptr, nullptr, &startup, &process) == FALSE) {
    fprintf(stderr, "ghost_launch: CreateProcess failed: %lu\n", GetLastError());
    return 1;
  }

  if (verbose) printf("suspended pid: %lu\n", process.dwProcessId);

  // Default is to abort rather than continue: a browser that silently runs with real
  // hardware values is worse than one that refuses to start, because the caller would
  // believe it is protected. --allow-unspoofed opts into the unsafe behaviour.
  if (!ghost::is_injectable_target(process.hProcess)) {
    fprintf(stderr, "ghost_launch: target bitness mismatch; injection impossible\n");
    if (!allow_unspoofed) {
      TerminateProcess(process.hProcess, 1);
      CloseHandle(process.hThread);
      CloseHandle(process.hProcess);
      return 3;
    }
    fprintf(stderr, "ghost_launch: --allow-unspoofed set; launching WITHOUT spoofing\n");
  } else {
    bool injected = false;
    int hooks = -1;
    int stage = -1;
    DWORD error = 0;
    const bool ready = ghost::inject_and_wait(
        process.hProcess, process.dwProcessId,
        std::wstring(dll.begin(), dll.end()).c_str(), 30000, &injected, &hooks, &stage,
        &error);
    if (!ready) {
      // stage: 0 = export RVA unreadable, 1 = LoadLibraryW unresolvable,
      //        2 = block allocation failed, 3 = block/stub write failed,
      //        4 = no remote thread could be started, 5 = success,
      //        100+n = the stub ran but never published a result (n = last marker)
      fprintf(stderr,
              "ghost_launch: readiness handshake failed "
              "(injected=%d hooks=%d stage=%d err=%lu)\n",
              injected ? 1 : 0, hooks, stage, error);
      if (!allow_unspoofed) {
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return 2;
      }
      fprintf(stderr, "ghost_launch: --allow-unspoofed set; launching WITHOUT spoofing\n");
    } else if (verbose) {
      printf("shim ready (hooks=%d)\n", hooks);
    }
  }

  ResumeThread(process.hThread);

  if (!wait) {
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return 0;
  }

  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exit_code = 0;
  GetExitCodeProcess(process.hProcess, &exit_code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  if (verbose) printf("target exited with %lu\n", exit_code);
  return static_cast<int>(exit_code);
}
