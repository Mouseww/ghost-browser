// inject.h — shared remote-DLL injection primitive plus the readiness handshake.
//
// Used by ghost_launch (to seed the first browser process) and by the shim itself
// (to propagate into Chromium's renderer / GPU / utility children).
#pragma once

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <vector>

namespace ghost {

// Starts a thread in `process` at `start`, passing `parameter`.
//
// CreateRemoteThread is only a wrapper over NtCreateThreadEx, but the wrapper adds
// user-mode work that a sandboxed target can refuse. Chromium creates its renderer and
// utility children with a restricted token, and there CreateRemoteThread succeeds when
// the start address is an image-backed function (kernel32!LoadLibraryW) yet fails when
// it is a private RWX page holding an injected stub -- on the very same process handle,
// so PROCESS_CREATE_THREAD is demonstrably granted and the kernel check cannot be the
// cause. NtCreateThreadEx is the primitive underneath and performs none of that extra
// work, so it is tried first and CreateRemoteThread remains the fallback.
inline HANDLE create_remote_thread(HANDLE process, void* start, void* parameter) {
  using NtCreateThreadExFn = LONG(NTAPI*)(PHANDLE, ACCESS_MASK, LPVOID, HANDLE, PVOID,
                                          PVOID, ULONG, SIZE_T, SIZE_T, SIZE_T, LPVOID);
  static NtCreateThreadExFn nt_create = reinterpret_cast<NtCreateThreadExFn>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateThreadEx"));
  if (nt_create != nullptr) {
    HANDLE thread = nullptr;
    // Only the rights actually used downstream: SYNCHRONIZE for the wait and
    // THREAD_QUERY_INFORMATION for the exit code. Asking for THREAD_ALL_ACCESS on a
    // restricted-token target is itself a way to be refused.
    const LONG status = nt_create(&thread, THREAD_QUERY_INFORMATION | SYNCHRONIZE, nullptr,
                                  process, start, parameter, 0, 0, 0, 0, nullptr);
    if (status >= 0 && thread != nullptr) return thread;
    if (thread != nullptr) CloseHandle(thread);
  }
  return CreateRemoteThread(process, nullptr, 0,
                            reinterpret_cast<LPTHREAD_START_ROUTINE>(start), parameter, 0,
                            nullptr);
}

// Injects `dll_path` into `process` and waits for LoadLibraryW to return.
//
// The target must still be CREATE_SUSPENDED. That is not a convenience: it is what
// guarantees the shim's DllMain runs before the target's own startup code, so hooks
// are in place before Chromium reads CPU count, screen metrics or GPU info and
// caches them for the lifetime of the process.
//
// `remote_module_out`, when supplied, receives the module base as mapped inside the
// target — the anchor the readiness handshake needs to find an export remotely.
//
// Returns true when LoadLibraryW reported a module base address.
inline bool inject_dll_into(HANDLE process, const wchar_t* dll_path,
                            void** remote_module_out = nullptr,
                            DWORD* thread_exit_out = nullptr) {
  if (remote_module_out != nullptr) *remote_module_out = nullptr;
  if (thread_exit_out != nullptr) *thread_exit_out = 0;
  if (process == nullptr || dll_path == nullptr || *dll_path == L'\0') return false;

  const SIZE_T bytes = (std::wcslen(dll_path) + 1) * sizeof(wchar_t);
  void* remote =
      VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (remote == nullptr) return false;

  bool loaded = false;
  if (WriteProcessMemory(process, remote, dll_path, bytes, nullptr) != FALSE) {
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    auto load_library =
        reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(kernel32, "LoadLibraryW"));
    if (load_library != nullptr) {
      HANDLE thread = create_remote_thread(process, reinterpret_cast<void*>(load_library),
                                           remote);
      if (thread != nullptr) {
        WaitForSingleObject(thread, 20000);
        DWORD exit_code = 0;
        if (GetExitCodeThread(thread, &exit_code) != FALSE) {
          if (thread_exit_out != nullptr) *thread_exit_out = exit_code;
          // LoadLibraryW returns the HMODULE, or NULL on failure.
          loaded = exit_code != 0 && exit_code != STILL_ACTIVE;
          if (loaded && remote_module_out != nullptr) {
            *remote_module_out = reinterpret_cast<void*>(static_cast<ULONG_PTR>(exit_code));
          }
        }
        CloseHandle(thread);
      }
    }
  }

  VirtualFreeEx(process, remote, 0, MEM_RELEASE);
  return loaded;
}

// The binary-signature policy Windows enforces on `process`.
//
// Chromium gives renderer children
// PROCESS_CREATION_MITIGATION_POLICY_BLOCK_NON_MICROSOFT_BINARIES_ALWAYS_ON, under which
// loading an unsigned image fails with STATUS_ACCESS_DENIED -- the very error LoadLibraryW
// reports as 0xC0000022. Reading the policy back is how propagation tells a target it can
// actually serve from one it can only break.
inline const char* target_signature_policy(HANDLE process) {
  PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY policy{};
  if (GetProcessMitigationPolicy(process, ProcessSignaturePolicy, &policy, sizeof(policy)) ==
      FALSE) {
    return "unknown";
  }
  if (policy.MicrosoftSignedOnly != 0) return "microsoft-signed-only";
  if (policy.StoreSignedOnly != 0) return "store-signed-only";
  return "any";
}

// True when `process` refuses to map images that are not signed by the required authority.
inline bool target_blocks_unsigned_images(HANDLE process) {
  PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY policy{};
  if (GetProcessMitigationPolicy(process, ProcessSignaturePolicy, &policy, sizeof(policy)) ==
      FALSE) {
    return false;
  }
  return policy.MicrosoftSignedOnly != 0 || policy.StoreSignedOnly != 0;
}

// True when `process` cannot host this DLL: a 32-bit target cannot load a 64-bit
// shim, and CreateRemoteThread would pass it a 64-bit function address it cannot use.
inline bool is_injectable_target(HANDLE process) {
  if (process == nullptr) return false;
  BOOL target_wow64 = FALSE;
  BOOL self_wow64 = FALSE;
  if (IsWow64Process(process, &target_wow64) == FALSE) return false;
  IsWow64Process(GetCurrentProcess(), &self_wow64);
  return target_wow64 == self_wow64;
}

// ---------------------------------------------------------------------------
// Readiness handshake
//
// LoadLibraryW returns as soon as DllMain returns — but the shim deliberately does
// all of its work on a background thread, so "DLL loaded" does not mean "hooks are
// installed". Resuming the child at that point would let Chromium read CPU count,
// screen metrics and GPU info *before* the hooks exist, and Chromium caches those
// values for the whole process lifetime. The child would then report real hardware
// while believing it had been spoofed.
//
// The obvious implementation — a named event the child signals — does not survive
// Chromium's sandbox, and fails in a way that is worse than failing outright. The
// browser process creates the event at medium integrity; a sandboxed renderer or GPU
// process runs from a restricted token at low integrity and is denied write access to
// it, so OpenEventW(EVENT_MODIFY_STATE) fails inside the child. The injector then
// blocks for its entire timeout on every sandboxed child. Because Chromium creates
// children from the browser's own threads, that stall is not local: the browser stops
// servicing its IPC, page loads hang, and Chrome starts reaping the children we are
// waiting on. Observed live: one 20-second stall per sandboxed child, a page that
// fetched but never executed, and a browser that looked hung.
//
// So no named object is used at all. The injector starts a *second* remote thread in
// the child that calls the exported ghost_shim_ready_probe(); that function blocks
// inside the child until the hook installer finishes and returns the installed-hook
// count as the thread exit code. The signal never leaves the child's address space, so
// integrity levels are irrelevant — and the exit code gives the injector visibility
// into sandboxed children whose writes to %TEMP%\ghost_shim.log are denied.
// ---------------------------------------------------------------------------

// Reads the RVA of an exported symbol straight out of the PE file on disk.
//
// Parsing the file rather than calling LoadLibraryEx(DONT_RESOLVE_DLL_REFERENCES)
// keeps the injector from mapping (and having to unmap) a second copy of the shim
// image just to learn one address.
inline DWORD pe_export_rva(const wchar_t* path, const char* symbol) {
  HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return 0;

  LARGE_INTEGER size{};
  if (GetFileSizeEx(file, &size) == FALSE || size.QuadPart < 0x1000) {
    CloseHandle(file);
    return 0;
  }

  std::vector<BYTE> buf(static_cast<size_t>(size.QuadPart));
  DWORD got = 0;
  const BOOL read_ok =
      ReadFile(file, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr);
  CloseHandle(file);
  if (read_ok == FALSE || got < 0x1000) return 0;

  const BYTE* base = buf.data();
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return 0;
  const size_t nt_off = static_cast<size_t>(dos->e_lfanew);
  if (nt_off + sizeof(IMAGE_NT_HEADERS64) > buf.size()) return 0;

  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + nt_off);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
  if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return 0;

  const IMAGE_DATA_DIRECTORY& dir =
      nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
  if (dir.VirtualAddress == 0 || dir.Size == 0) return 0;

  // RVA -> file offset. The export directory and its three tables are all reachable
  // this way; nothing in this file is mapped outside a section.
  auto rva_to_offset = [&](DWORD rva) -> size_t {
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
      const DWORD va = sec[i].VirtualAddress;
      const DWORD vsize = sec[i].Misc.VirtualSize != 0 ? sec[i].Misc.VirtualSize
                                                       : sec[i].SizeOfRawData;
      if (rva >= va && rva < va + vsize) {
        return static_cast<size_t>(sec[i].PointerToRawData) + (rva - va);
      }
    }
    return static_cast<size_t>(-1);
  };

  const size_t exp_off = rva_to_offset(dir.VirtualAddress);
  if (exp_off == static_cast<size_t>(-1) ||
      exp_off + sizeof(IMAGE_EXPORT_DIRECTORY) > buf.size()) {
    return 0;
  }
  const auto* exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + exp_off);

  const size_t names_off = rva_to_offset(exp->AddressOfNames);
  const size_t ords_off = rva_to_offset(exp->AddressOfNameOrdinals);
  const size_t funcs_off = rva_to_offset(exp->AddressOfFunctions);
  if (names_off == static_cast<size_t>(-1) || ords_off == static_cast<size_t>(-1) ||
      funcs_off == static_cast<size_t>(-1)) {
    return 0;
  }
  if (names_off + exp->NumberOfNames * sizeof(DWORD) > buf.size()) return 0;
  if (funcs_off + exp->NumberOfFunctions * sizeof(DWORD) > buf.size()) return 0;

  const auto* names = reinterpret_cast<const DWORD*>(base + names_off);
  const auto* ordinals = reinterpret_cast<const WORD*>(base + ords_off);
  const auto* functions = reinterpret_cast<const DWORD*>(base + funcs_off);

  for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
    const size_t name_off = rva_to_offset(names[i]);
    if (name_off == static_cast<size_t>(-1) || name_off >= buf.size()) continue;
    if (std::strcmp(reinterpret_cast<const char*>(base + name_off), symbol) != 0) {
      continue;
    }
    const WORD ordinal = ordinals[i];
    if (ordinal >= exp->NumberOfFunctions) return 0;
    return functions[ordinal];
  }
  return 0;
}

// Reads the load address of `module_name` inside `process`.
//
// LoadLibraryW returns the true 64-bit module base in RAX, but GetExitCodeThread only
// hands back a DWORD, so an address such as 0x00007FFC85B30000 would lose its high half
// and every probe address derived from it would be invalid. Enumerating modules through
// psapi's K32* entry points was tried first and refused the call outright
// (ERROR_INVALID_PARAMETER). Walking the loader's own list through ReadProcessMemory
// needs no such API and works against a sandboxed, restricted-token child exactly as it
// does against the browser process.
//
// The offsets below are the stable x64 layout of PEB and LDR_DATA_TABLE_ENTRY, neither
// of which appears in a public header.
inline void* walk_remote_loader_list(HANDLE process, const wchar_t* wanted,
                                     wchar_t* names_out, size_t names_cap,
                                     size_t* scanned_out, int* stage_out) {
  auto fail = [&](int stage) -> void* {
    if (stage_out != nullptr) *stage_out = stage;
    return nullptr;
  };
  if (names_out != nullptr && names_cap > 0) names_out[0] = L'\0';
  if (scanned_out != nullptr) *scanned_out = 0;
  struct ProcessBasicInformation {
    LONG ExitStatus;
    void* PebBaseAddress;
    ULONG_PTR AffinityMask;
    LONG BasePriority;
    ULONG_PTR UniqueProcessId;
    ULONG_PTR InheritedFromUniqueProcessId;
  };
  struct RemoteUnicodeString {
    unsigned short Length;
    unsigned short MaximumLength;
    wchar_t* Buffer;
  };
  using NtQueryInformationProcessFn = LONG(NTAPI*)(HANDLE, ULONG, void*, ULONG, ULONG*);
  static NtQueryInformationProcessFn query = reinterpret_cast<NtQueryInformationProcessFn>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
  if (query == nullptr || process == nullptr) return fail(1);

  constexpr ULONG kProcessBasicInformation = 0;
  constexpr SIZE_T kPebLdr = 0x18;
  constexpr SIZE_T kLdrInLoadOrderModuleList = 0x10;
  constexpr SIZE_T kEntryDllBase = 0x30;
  constexpr SIZE_T kEntryBaseDllName = 0x58;
  constexpr SIZE_T kMaxModules = 512;

  ProcessBasicInformation pbi{};
  ULONG returned = 0;
  if (query(process, kProcessBasicInformation, &pbi, sizeof(pbi), &returned) < 0) {
    return fail(1);
  }
  if (pbi.PebBaseAddress == nullptr) return fail(1);

  auto* peb = static_cast<BYTE*>(pbi.PebBaseAddress);
  BYTE* ldr = nullptr;
  if (ReadProcessMemory(process, peb + kPebLdr, &ldr, sizeof(ldr), nullptr) == FALSE) {
    return fail(6);
  }
  if (ldr == nullptr) return fail(6);

  // The list head lives in the target, so cycle detection compares against its remote
  // address rather than against the local copy of the entry.
  BYTE* const remote_head = ldr + kLdrInLoadOrderModuleList;
  LIST_ENTRY head{};
  if (ReadProcessMemory(process, remote_head, &head, sizeof(head), nullptr) == FALSE) {
    return fail(6);
  }

  BYTE* cursor = reinterpret_cast<BYTE*>(head.Flink);
  size_t scanned = 0;
  size_t written = 0;
  // The caller-supplied buffer doubles as the diagnostic channel: when a match fails there
  // is otherwise no way to tell an empty loader list from a name that never matched.
  if (names_out != nullptr && names_cap > 0) {
    _snwprintf_s(names_out, names_cap, _TRUNCATE,
                 L"ldr=%p head=%p flink=%p blinks=%p;", ldr, remote_head, head.Flink,
                 head.Blink);
    written = wcslen(names_out);
  }
  for (SIZE_T i = 0; i < kMaxModules && cursor != nullptr && cursor != remote_head; ++i) {
    ++scanned;
    void* dll_base = nullptr;
    RemoteUnicodeString name{};
    if (ReadProcessMemory(process, cursor + kEntryDllBase, &dll_base, sizeof(dll_base),
                          nullptr) != FALSE &&
        ReadProcessMemory(process, cursor + kEntryBaseDllName, &name, sizeof(name),
                          nullptr) != FALSE &&
        name.Buffer != nullptr && name.Length > 0 && name.Length <= 260 * sizeof(wchar_t)) {
      wchar_t buffer[261] = {0};
      if (ReadProcessMemory(process, name.Buffer, buffer, name.Length, nullptr) != FALSE) {
        const size_t len = name.Length / sizeof(wchar_t);
        buffer[len] = L'\0';
        if (names_out != nullptr && names_cap > 0) {
          if (written > 0 && written + 1 < names_cap) names_out[written++] = L';';
          for (size_t k = 0; k < len && written + 1 < names_cap; ++k) {
            names_out[written++] = buffer[k];
          }
          names_out[written] = L'\0';
        }
        if (wanted != nullptr && _wcsicmp(buffer, wanted) == 0) {
          if (stage_out != nullptr) *stage_out = 5;
          if (scanned_out != nullptr) *scanned_out = scanned;
          return dll_base;
        }
      }
    }
    LIST_ENTRY links{};
    if (ReadProcessMemory(process, cursor, &links, sizeof(links), nullptr) == FALSE) break;
    cursor = reinterpret_cast<BYTE*>(links.Flink);
  }
  if (scanned_out != nullptr) *scanned_out = scanned;
  return fail(7);
}

// Locates `module_name` (matched on basename, case-insensitively) in `process`.
inline void* find_remote_module_base(HANDLE process, const wchar_t* module_name,
                                     int* stage_out = nullptr) {
  if (module_name == nullptr) return nullptr;
  const wchar_t* wanted = module_name;
  for (const wchar_t* q = module_name; *q != L'\0'; ++q) {
    if (*q == L'\\' || *q == L'/') wanted = q + 1;
  }
  return walk_remote_loader_list(process, wanted, nullptr, 0, nullptr, stage_out);
}

// Diagnostic helper: the basenames of the modules loaded in `process`, semicolon separated.
// Returns the number of loader-list entries visited.
inline int dump_remote_module_names(HANDLE process, wchar_t* out, size_t cap) {
  size_t scanned = 0;
  walk_remote_loader_list(process, nullptr, out, cap, &scanned, nullptr);
  return static_cast<int>(scanned);
}

// Asks the shim inside `process` to confirm that its hooks are installed.
//
// The probe runs as a remote thread whose entry point is an export of the shim image
// already mapped into the target, and its return value arrives as that thread's exit
// code. That shape is forced by the sandbox: a Chromium child created with a restricted
// token accepts a remote thread started at an image-backed address (a thread at
// kernel32!LoadLibraryW demonstrably succeeds on the very same handle) but refuses one
// started at a private RWX page with ERROR_ACCESS_DENIED, even though
// PROCESS_CREATE_THREAD is granted. Injecting a code stub is therefore not an option,
// and neither is VirtualAllocEx(PAGE_EXECUTE_READWRITE), which a PROHIBIT_DYNAMIC_CODE
// child rejects outright.
//
// Returns the installed-hook count, or -1 if the child never became ready.
// `stage_out`, when supplied, reports where a failure happened:
//   0 = could not read the export RVA from the DLL on disk
//   1 = could not query the target's PEB (or resolve NtQueryInformationProcess)
//   2 = no remote thread could be started at the probe
//   3 = the probe ran but reported no hooks
//   4 = the probe never finished within the timeout
//   5 = success
//   6 = the PEB, the loader data or the loader list head could not be read
//   7 = the shim image was not present in the target's loader list
// `error_out`, when supplied, receives the real GetLastError() from the failing call
// rather than whatever happened to be left over in the caller.
inline int wait_for_shim_ready(HANDLE process, const wchar_t* dll_path, DWORD timeout_ms,
                               int* stage_out = nullptr, DWORD* error_out = nullptr) {
  int stage = 0;
  if (error_out != nullptr) *error_out = 0;
  auto finish = [&](int value) {
    if (stage_out != nullptr) *stage_out = stage;
    return value;
  };

  if (process == nullptr || dll_path == nullptr) return finish(-1);

  const DWORD rva = pe_export_rva(dll_path, "ghost_shim_ready_probe");
  if (rva == 0) return finish(-1);

  stage = 1;
  auto* base = static_cast<BYTE*>(find_remote_module_base(process, dll_path, &stage));
  if (base == nullptr) return finish(-1);

  stage = 2;
  HANDLE thread = create_remote_thread(process, base + rva, nullptr);
  if (thread == nullptr) {
    if (error_out != nullptr) *error_out = GetLastError();
    return finish(-1);
  }

  const DWORD waited = WaitForSingleObject(thread, timeout_ms);
  DWORD exit_code = 0;
  if (waited != WAIT_OBJECT_0) {
    stage = 4;
    CloseHandle(thread);
    return finish(-1);
  }
  if (GetExitCodeThread(thread, &exit_code) == FALSE || exit_code == 0) {
    stage = 3;
    CloseHandle(thread);
    return finish(-1);
  }
  CloseHandle(thread);
  stage = 5;
  return finish(static_cast<int>(exit_code) - 1);  // undo the +1 bias from the shim
}

// Reopens the target with every right the injection sequence uses.
//
// The process handle that CreateProcessAsUserW hands back for a restricted-token child is
// not guaranteed to carry all of them. On such a handle VirtualAllocEx, WriteProcessMemory
// and a remote thread at kernel32!LoadLibraryW all succeed, which proves
// PROCESS_VM_OPERATION, PROCESS_VM_WRITE and PROCESS_CREATE_THREAD are granted, while
// PROCESS_VM_READ and PROCESS_QUERY_INFORMATION are not -- exactly the pair that reading
// the child's PEB and loader list needs. Reopening by pid asks for precisely the rights
// that get used; a medium-integrity parent is allowed to open a lower-integrity child.
inline HANDLE open_target_for_injection(HANDLE process, DWORD pid) {
  (void)process;
  return OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                         PROCESS_VM_READ | PROCESS_QUERY_INFORMATION | SYNCHRONIZE,
                     FALSE, pid);
}

// Injector side: inject, then block until the shim reports that hooks are installed.
inline bool inject_and_wait(HANDLE process, DWORD pid, const wchar_t* dll_path,
                            DWORD timeout_ms, bool* injected_out = nullptr,
                            int* hooks_out = nullptr, int* stage_out = nullptr,
                            DWORD* error_out = nullptr, DWORD* load_exit_out = nullptr) {
  // Prefer a handle that is known to carry PROCESS_VM_READ and PROCESS_QUERY_INFORMATION;
  // fall back to the caller's handle when the target cannot be reopened.
  HANDLE reopened = open_target_for_injection(process, pid);
  HANDLE target = reopened != nullptr ? reopened : process;

  DWORD load_exit = 0;
  const bool injected = inject_dll_into(target, dll_path, nullptr, &load_exit);
  if (injected_out != nullptr) *injected_out = injected;

  int hooks = -1;
  int stage = -1;
  DWORD error = 0;
  if (injected) {
    hooks = wait_for_shim_ready(target, dll_path, timeout_ms, &stage, &error);
  }
  if (reopened != nullptr) CloseHandle(reopened);
  if (hooks_out != nullptr) *hooks_out = hooks;
  if (stage_out != nullptr) *stage_out = stage;
  if (error_out != nullptr) *error_out = error;
  if (load_exit_out != nullptr) *load_exit_out = load_exit;
  return injected && hooks >= 0;
}

}  // namespace ghost
