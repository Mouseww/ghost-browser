// hooks_proc.cpp — channel A: propagate the shim into Chromium's child processes.
//
// Chromium is a multi-process browser. Spoofing only the browser process would be
// pointless: navigator.hardwareConcurrency is evaluated in the *renderer*, and the
// WebGL strings come from the *GPU* process. The shim therefore has to reach every
// child.
//
// Finding the chokepoint took two attempts, and the first one failed in a way worth
// recording. Hooking kernel32!CreateProcessW caught nothing at all: kernel32's
// CreateProcess*W are export forwarders into kernelbase.dll, and — the part that is
// easy to get wrong — kernelbase!CreateProcessA does not call CreateProcessW. Both
// documented entry points call the undocumented kernelbase!CreateProcessInternalW
// directly, so a hook on either documented name sits on a path nobody takes.
//
// CreateProcessInternalW is exported from both kernel32 and kernelbase (verified with
// GetProcAddress), which makes it usable with a plain export hook rather than symbol
// reconstruction. All documented variants funnel through it, so it is the single point
// that actually covers every child.
//
// The technique at that point: force CREATE_SUSPENDED on the child, inject, wait for
// the child's hooks to be live, then resume — but only if the caller did not already
// ask for a suspended process. Chromium uses CREATE_SUSPENDED deliberately (to assign
// the child to a job object before it runs); resuming in that case would break its
// startup handshake, so we leave it suspended and let Chromium resume it.
#include <windows.h>

#include <string>

#include "../../common/inject.h"
#include "hook_engine.h"

namespace ghost {
namespace {

BOOL(WINAPI* real_CreateProcessW)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES,
                                  LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCWSTR,
                                  LPSTARTUPINFOW, LPPROCESS_INFORMATION) = nullptr;
BOOL(WINAPI* real_CreateProcessAsUserW)(HANDLE, LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES,
                                        LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCWSTR,
                                        LPSTARTUPINFOW, LPPROCESS_INFORMATION) = nullptr;
// Undocumented but exported by both kernel32 and kernelbase. The trailing hNewToken
// parameter is why this needs its own hook signature rather than reusing the
// CreateProcessW one.
BOOL(WINAPI* real_CreateProcessInternalW)(HANDLE, LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES,
                                          LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCWSTR,
                                          LPSTARTUPINFOW, LPPROCESS_INFORMATION, PHANDLE) = nullptr;

std::wstring g_shim_path;
thread_local bool t_in_spawn_hook = false;

// Logs the child's token at the moment of injection.
//
// Three theories about why LoadLibraryW returns STATUS_ACCESS_DENIED here were wrong in a
// row (CIG, AppContainer ACLs, file integrity level), so the token is measured rather than
// assumed: integrity RID, AppContainer flag, restricted flag, and how many restricted SIDs
// an access check has to satisfy.
void log_target_token(DWORD pid) {
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (process == nullptr) {
    ghost_log("propagate: pid=%lu token probe: OpenProcess err=%lu", pid, GetLastError());
    return;
  }
  HANDLE token = nullptr;
  if (!OpenProcessToken(process, TOKEN_QUERY, &token)) {
    ghost_log("propagate: pid=%lu token probe: OpenProcessToken err=%lu", pid,
              GetLastError());
    CloseHandle(process);
    return;
  }

  DWORD rid = 0xFFFFFFFFu;
  BYTE buffer[256] = {};
  DWORD size = 0;
  if (GetTokenInformation(token, TokenIntegrityLevel, buffer, sizeof(buffer), &size)) {
    auto* label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buffer);
    const UCHAR* count = GetSidSubAuthorityCount(label->Label.Sid);
    if (count != nullptr && *count > 0) {
      rid = *GetSidSubAuthority(label->Label.Sid, static_cast<DWORD>(*count) - 1);
    }
  }

  DWORD appcontainer = 0;
  DWORD value_size = sizeof(appcontainer);
  GetTokenInformation(token, TokenIsAppContainer, &appcontainer, value_size, &value_size);
  DWORD restricted = 0;
  value_size = sizeof(restricted);
  GetTokenInformation(token, TokenHasRestrictions, &restricted, value_size, &value_size);

  DWORD restricted_sid_count = 0;
  wchar_t restricted_text[512] = {};
  DWORD sids_size = 0;
  GetTokenInformation(token, TokenRestrictedSids, nullptr, 0, &sids_size);
  if (sids_size >= sizeof(TOKEN_GROUPS)) {
    BYTE* raw = new BYTE[sids_size];
    if (GetTokenInformation(token, TokenRestrictedSids, raw, sids_size, &sids_size)) {
      auto* groups = reinterpret_cast<TOKEN_GROUPS*>(raw);
      restricted_sid_count = groups->GroupCount;
      // Render the list as S-1-a-b-... A restricted token must pass the ordinary DACL check
      // AND a second check against this list, so a SID that appears here but in no ACE --
      // S-1-0-0 (the NULL SID) is the classic case -- denies access no matter how generous
      // the DACL looks.
      size_t used = 0;
      for (DWORD i = 0; i < groups->GroupCount && used < 400; ++i) {
        PSID sid = groups->Groups[i].Sid;
        if (sid == nullptr) continue;
        const UCHAR* count = GetSidSubAuthorityCount(sid);
        const UCHAR sub_count = count != nullptr ? *count : 0;
        int written = swprintf_s(restricted_text + used, 512 - used, L"S-1");
        if (written > 0) used += static_cast<size_t>(written);
        for (UCHAR k = 0; k < sub_count && used < 460; ++k) {
          written = swprintf_s(restricted_text + used, 512 - used, L"-%lu",
                               *GetSidSubAuthority(sid, k));
          if (written > 0) used += static_cast<size_t>(written);
        }
        written = swprintf_s(restricted_text + used, 512 - used, L" ");
        if (written > 0) used += static_cast<size_t>(written);
      }
    }
    delete[] raw;
  }

  ghost_log(
      "propagate: pid=%lu token il_rid=%lu appcontainer=%lu restricted=%lu restricted_sids=%lu [%ls]",
      pid, rid, appcontainer, restricted, restricted_sid_count, restricted_text);
  CloseHandle(token);
  CloseHandle(process);
}

// Injects into a freshly created suspended child and optionally resumes it.
void propagate(PROCESS_INFORMATION* pi, bool resume_after) {
  if (pi == nullptr || pi->hProcess == nullptr) {
    ghost_log("propagate: no process handle; skipping");
    return;
  }
  if (!is_injectable_target(pi->hProcess)) {
    ghost_log("propagate: pid=%lu rejected by is_injectable_target (err=%lu)",
              pi->dwProcessId, GetLastError());
    if (resume_after && pi->hThread != nullptr) ResumeThread(pi->hThread);
    return;
  }

  // A target that enforces code integrity can never host an unsigned DLL: LoadLibraryW
  // fails with STATUS_ACCESS_DENIED and the attempt only risks the child. Skip it.
  ghost_log("propagate: pid=%lu signature policy=%s", pi->dwProcessId,
            target_signature_policy(pi->hProcess));
  if (target_blocks_unsigned_images(pi->hProcess)) {
    ghost_log("propagate: pid=%lu skipped: unsigned images are blocked", pi->dwProcessId);
    if (resume_after && pi->hThread != nullptr) ResumeThread(pi->hThread);
    return;
  }

  // Measure the child's token before touching it. LoadLibraryW returns STATUS_ACCESS_DENIED
  // for sandboxed children and the token is what explains it, so record it rather than
  // infer it from the process type.
  log_target_token(pi->dwProcessId);

  // inject_and_wait asks the shim inside the child to confirm its hooks are installed,
  // over a remote thread rather than a named event: a sandboxed child cannot open a
  // medium-integrity event, and a stalled handshake blocks the browser's own thread.
  // The timeout is deliberately short for the same reason — never let one unresponsive
  // child freeze the whole browser.
  bool injected = false;
  int hooks = -1;
  int stage = -1;
  DWORD error = 0;
  DWORD load_exit = 0;
  const bool ready = inject_and_wait(pi->hProcess, pi->dwProcessId, g_shim_path.c_str(),
                                     8000, &injected, &hooks, &stage, &error, &load_exit);
  if (ready) {
    ghost_log("propagate: pid=%lu injected+ready hooks=%d", pi->dwProcessId, hooks);
  } else if (!injected) {
    ghost_log("propagate: pid=%lu injection failed (load_exit=0x%08lX err=%lu)",
              pi->dwProcessId, load_exit, error);
  } else {
    ghost_log("propagate: pid=%lu loaded but never ready (hooks=%d stage=%d err=%lu)",
              pi->dwProcessId, hooks, stage, error);
    // The loader list is the only place the handshake can find the shim image, so when it
    // fails, record what the walk actually saw.
    wchar_t names[2048] = {0};
    HANDLE probe = open_target_for_injection(pi->hProcess, pi->dwProcessId);
    const int scanned =
        dump_remote_module_names(probe != nullptr ? probe : pi->hProcess, names, 2048);
    if (probe != nullptr) CloseHandle(probe);
    ghost_log("propagate: pid=%lu load_exit=0x%08lX loader list: scanned=%d [%ls]",
              pi->dwProcessId, load_exit, scanned, names);
  }
  if (resume_after && pi->hThread != nullptr) ResumeThread(pi->hThread);
}

BOOL WINAPI hook_CreateProcessW(LPCWSTR application, LPWSTR command_line,
                                LPSECURITY_ATTRIBUTES process_attrs,
                                LPSECURITY_ATTRIBUTES thread_attrs, BOOL inherit_handles,
                                DWORD flags, LPVOID environment, LPCWSTR current_dir,
                                LPSTARTUPINFOW startup_info,
                                LPPROCESS_INFORMATION process_info) {
  if (real_CreateProcessW == nullptr) return FALSE;
  if (t_in_spawn_hook || g_shim_path.empty()) {
    return real_CreateProcessW(application, command_line, process_attrs, thread_attrs,
                               inherit_handles, flags, environment, current_dir,
                               startup_info, process_info);
  }

  t_in_spawn_hook = true;
  const bool already_suspended = (flags & CREATE_SUSPENDED) != 0;
  const BOOL ok = real_CreateProcessW(application, command_line, process_attrs, thread_attrs,
                                      inherit_handles, flags | CREATE_SUSPENDED, environment,
                                      current_dir, startup_info, process_info);
  if (ok) {
    ghost_log("spawn intercepted at CreateProcessW: pid=%lu already_suspended=%d",
              process_info->dwProcessId, already_suspended ? 1 : 0);
    propagate(process_info, !already_suspended);
  }
  t_in_spawn_hook = false;
  return ok;
}

BOOL WINAPI hook_CreateProcessAsUserW(HANDLE token, LPCWSTR application, LPWSTR command_line,
                                      LPSECURITY_ATTRIBUTES process_attrs,
                                      LPSECURITY_ATTRIBUTES thread_attrs,
                                      BOOL inherit_handles, DWORD flags, LPVOID environment,
                                      LPCWSTR current_dir, LPSTARTUPINFOW startup_info,
                                      LPPROCESS_INFORMATION process_info) {
  if (real_CreateProcessAsUserW == nullptr) return FALSE;
  if (t_in_spawn_hook || g_shim_path.empty()) {
    return real_CreateProcessAsUserW(token, application, command_line, process_attrs,
                                     thread_attrs, inherit_handles, flags, environment,
                                     current_dir, startup_info, process_info);
  }

  t_in_spawn_hook = true;
  const bool already_suspended = (flags & CREATE_SUSPENDED) != 0;
  const BOOL ok = real_CreateProcessAsUserW(
      token, application, command_line, process_attrs, thread_attrs, inherit_handles,
      flags | CREATE_SUSPENDED, environment, current_dir, startup_info, process_info);
  if (ok) {
    ghost_log("spawn intercepted at CreateProcessAsUserW: pid=%lu already_suspended=%d",
              process_info->dwProcessId, already_suspended ? 1 : 0);
    propagate(process_info, !already_suspended);
  }
  t_in_spawn_hook = false;
  return ok;
}

BOOL WINAPI hook_CreateProcessInternalW(HANDLE token, LPCWSTR application, LPWSTR command_line,
                                        LPSECURITY_ATTRIBUTES process_attrs,
                                        LPSECURITY_ATTRIBUTES thread_attrs,
                                        BOOL inherit_handles, DWORD flags, LPVOID environment,
                                        LPCWSTR current_dir, LPSTARTUPINFOW startup_info,
                                        LPPROCESS_INFORMATION process_info,
                                        PHANDLE new_token) {
  if (real_CreateProcessInternalW == nullptr) return FALSE;
  if (t_in_spawn_hook || g_shim_path.empty()) {
    return real_CreateProcessInternalW(token, application, command_line, process_attrs,
                                       thread_attrs, inherit_handles, flags, environment,
                                       current_dir, startup_info, process_info, new_token);
  }

  t_in_spawn_hook = true;
  const bool already_suspended = (flags & CREATE_SUSPENDED) != 0;
  const BOOL ok = real_CreateProcessInternalW(
      token, application, command_line, process_attrs, thread_attrs, inherit_handles,
      flags | CREATE_SUSPENDED, environment, current_dir, startup_info, process_info,
      new_token);
  if (ok) {
    ghost_log("spawn intercepted at CreateProcessInternalW: pid=%lu already_suspended=%d",
              process_info->dwProcessId, already_suspended ? 1 : 0);
    propagate(process_info, !already_suspended);
  }
  t_in_spawn_hook = false;
  return ok;
}

}  // namespace

void install_proc_hooks() {
  // The shim's OWN path, not the host executable's. GetModuleFileNameW(nullptr, ...)
  // returns the host EXE, which would make every child process get the EXE injected
  // instead of the shim.
  HMODULE self = static_cast<HMODULE>(shim_module());
  wchar_t path[MAX_PATH] = {0};
  if (self == nullptr || GetModuleFileNameW(self, path, MAX_PATH) == 0) {
    ghost_log("install_proc_hooks: cannot resolve shim path; propagation disabled");
    return;
  }
  g_shim_path = path;
  ghost_log("propagation source: %ls", g_shim_path.c_str());

  // Hook ONLY the implementation module. kernel32's CreateProcess*W and advapi32's
  // CreateProcessAsUserW are export forwarders into kernelbase, so hooking kernelbase
  // already covers callers of the documented names.
  //
  // Hooking both layers is not merely redundant, it is actively broken here: all six
  // sites would share one trampoline variable per function, so the second install
  // overwrites the first's trampoline. A call then runs hook -> wrong trampoline ->
  // the other layer's forwarder -> the patched target again, recursing until the stack
  // overflows (observed: exit code 0xC00000FD). One target, one trampoline.
  install_hook_export("kernelbase.dll", "CreateProcessInternalW",
                      reinterpret_cast<void*>(hook_CreateProcessInternalW),
                      reinterpret_cast<void**>(&real_CreateProcessInternalW));

  // Documented entry points, kept as a safety net for callers that invoke them
  // directly. They reach CreateProcessInternalW above, and t_in_spawn_hook keeps the
  // overlapping coverage idempotent instead of injecting twice.
  install_hook_export("kernelbase.dll", "CreateProcessW",
                      reinterpret_cast<void*>(hook_CreateProcessW),
                      reinterpret_cast<void**>(&real_CreateProcessW));
  install_hook_export("kernelbase.dll", "CreateProcessAsUserW",
                      reinterpret_cast<void*>(hook_CreateProcessAsUserW),
                      reinterpret_cast<void**>(&real_CreateProcessAsUserW));
}

}  // namespace ghost
