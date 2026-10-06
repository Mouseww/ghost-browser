// probe — a deliberately boring Win32 program that reads exactly the OS values the
// shim claims to spoof.
//
// This exists so that shim behaviour can be verified without a browser in the loop.
// The same probe run twice — once bare, once under ghost_launch — is the fastest
// possible signal that injection and hooks actually work, and it isolates a shim bug
// from a Chromium-integration bug.
//
// The report itself lives in common/probe_report.cpp, because ghost.exe's hidden
// `__probe` subcommand prints the identical lines and there must be only one
// definition of what counts as spoofed.
//
// `probe --child` re-executes itself; the parent checks that the grandchild also
// reports spoofed values, which is what proves subprocess propagation.
#include <windows.h>

#include <cstdio>
#include <string>

#include "../../../common/probe_report.h"

int main(int argc, char** argv) {
  if (argc > 1 && std::string(argv[1]) == "--child") {
    printf("=== CHILD PROCESS ===\n");
    ghost::probe_report();
    return 0;
  }

  ghost::probe_report();

  if (argc > 1 && std::string(argv[1]) == "--spawn-child") {
    printf("\n=== spawning a child to verify propagation ===\n");

    char self[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, self, MAX_PATH);
    std::string command = std::string("\"") + self + "\" --child";

    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION pi{};
    if (CreateProcessA(nullptr, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                       &startup, &pi)) {
      WaitForSingleObject(pi.hProcess, 30000);
      DWORD exit_code = 0;
      GetExitCodeProcess(pi.hProcess, &exit_code);
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);
      printf("child exit code = %lu\n", exit_code);
    } else {
      printf("CreateProcess failed: %lu\n", GetLastError());
    }
  }

  return 0;
}
