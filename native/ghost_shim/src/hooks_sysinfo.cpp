// hooks_sysinfo.cpp — channel A: CPU count and physical memory.
//
// JS surface covered:
//   navigator.hardwareConcurrency <- base::SysInfo::NumberOfProcessors()
//                                     -> GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)
//   navigator.deviceMemory        <- base::SysInfo::AmountOfPhysicalMemory()
//                                     -> GlobalMemoryStatusEx().ullTotalPhys
//
// The same APIs feed Blink's memory pressure heuristics and Chromium's own thread
// pool sizing, so spoofing them here keeps the page's view consistent with how the
// browser actually behaves (a browser that reports 4 cores but spawns 8 workers is
// itself a fingerprint).
#include <windows.h>

#include "../include/ghost_profile.h"
#include "hook_engine.h"

namespace ghost {
namespace {

DWORD(WINAPI* real_GetActiveProcessorCount)(WORD) = nullptr;
BOOL(WINAPI* real_GlobalMemoryStatusEx)(LPMEMORYSTATUSEX) = nullptr;
void(WINAPI* real_GetSystemInfo)(LPSYSTEM_INFO) = nullptr;
BOOL(WINAPI* real_GetPhysicallyInstalledSystemMemory)(PULONGLONG) = nullptr;
void(WINAPI* real_GetNativeSystemInfo)(LPSYSTEM_INFO) = nullptr;

DWORD WINAPI hook_GetActiveProcessorCount(WORD group) {
  const Profile& p = Profile::current();
  if (p.enabled && p.cpu.hardware_concurrency != 0) {
    return static_cast<DWORD>(p.cpu.hardware_concurrency);
  }
  return real_GetActiveProcessorCount ? real_GetActiveProcessorCount(group) : 1;
}

BOOL WINAPI hook_GlobalMemoryStatusEx(LPMEMORYSTATUSEX status) {
  if (real_GlobalMemoryStatusEx == nullptr) return FALSE;
  const BOOL ok = real_GlobalMemoryStatusEx(status);
  if (!ok || status == nullptr) return ok;

  const Profile& p = Profile::current();
  if (!p.enabled || p.memory.total_bytes == 0) return ok;

  // Preserve the real available/total ratio so memory-pressure signals stay
  // plausible rather than reporting a freshly-booted machine forever.
  const double ratio =
      status->ullTotalPhys != 0
          ? static_cast<double>(status->ullAvailPhys) / static_cast<double>(status->ullTotalPhys)
          : 0.5;
  const DWORD64 real_total = status->ullTotalPhys;
  const DWORD64 total = p.memory.total_bytes;
  const DWORD64 avail = static_cast<DWORD64>(static_cast<double>(total) * ratio);

  // Keep the commit limit offset (page file overhead) rather than inventing one.
  const DWORD64 pagefile_overhead =
      status->ullTotalPageFile > real_total ? status->ullTotalPageFile - real_total : 0;

  status->ullTotalPhys = total;
  status->ullAvailPhys = avail;
  status->ullTotalPageFile = total + pagefile_overhead;
  status->ullAvailPageFile = avail;
  status->dwMemoryLoad =
      total != 0 ? static_cast<DWORD>((total - avail) * 100 / total) : 0;
  return ok;
}

void WINAPI hook_GetSystemInfo(LPSYSTEM_INFO info) {
  if (real_GetSystemInfo == nullptr) return;
  real_GetSystemInfo(info);
  const Profile& p = Profile::current();
  if (info != nullptr && p.enabled && p.cpu.hardware_concurrency != 0) {
    info->dwNumberOfProcessors = static_cast<DWORD>(p.cpu.hardware_concurrency);
  }
}

// This is the API that actually backs navigator.hardwareConcurrency. Chromium's
// base/win/windows_version.cc caches GetNativeSystemInfo().dwNumberOfProcessors into a
// function-local static and returns it from SysInfo::NumberOfProcessors(), so hooking
// GetActiveProcessorCount or GetSystemInfo alone changes nothing on the JS surface --
// verified against the Chromium source after the renderer kept reporting 8 cores.
void WINAPI hook_GetNativeSystemInfo(LPSYSTEM_INFO info) {
  if (real_GetNativeSystemInfo == nullptr) return;
  real_GetNativeSystemInfo(info);
  const Profile& p = Profile::current();
  if (info != nullptr && p.enabled && p.cpu.hardware_concurrency != 0) {
    info->dwNumberOfProcessors = static_cast<DWORD>(p.cpu.hardware_concurrency);
  }
}

BOOL WINAPI hook_GetPhysicallyInstalledSystemMemory(PULONGLONG total_kb) {
  if (real_GetPhysicallyInstalledSystemMemory == nullptr) return FALSE;
  const Profile& p = Profile::current();
  if (p.enabled && p.memory.total_bytes != 0) {
    if (total_kb != nullptr) *total_kb = p.memory.total_bytes / 1024;
    return TRUE;
  }
  return real_GetPhysicallyInstalledSystemMemory(total_kb);
}

}  // namespace

void install_sysinfo_hooks() {
  install_hook_export("kernel32.dll", "GetActiveProcessorCount",
                      reinterpret_cast<void*>(hook_GetActiveProcessorCount),
                      reinterpret_cast<void**>(&real_GetActiveProcessorCount));
  install_hook_export("kernel32.dll", "GlobalMemoryStatusEx",
                      reinterpret_cast<void*>(hook_GlobalMemoryStatusEx),
                      reinterpret_cast<void**>(&real_GlobalMemoryStatusEx));
  install_hook_export("kernel32.dll", "GetSystemInfo",
                      reinterpret_cast<void*>(hook_GetSystemInfo),
                      reinterpret_cast<void**>(&real_GetSystemInfo));
  install_hook_export("kernel32.dll", "GetPhysicallyInstalledSystemMemory",
                      reinterpret_cast<void*>(hook_GetPhysicallyInstalledSystemMemory),
                      reinterpret_cast<void**>(&real_GetPhysicallyInstalledSystemMemory));
  install_hook_export("kernel32.dll", "GetNativeSystemInfo",
                      reinterpret_cast<void*>(hook_GetNativeSystemInfo),
                      reinterpret_cast<void**>(&real_GetNativeSystemInfo));
}

}  // namespace ghost
