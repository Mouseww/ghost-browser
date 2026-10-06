# Ghost Browser 垂直切片 v0.1 — 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 端到端跑通一条链路——生成相干指纹档案 → 由自研启动器注入原生 Shim 启动真实 Chromium → 本地检测页确认 `navigator.webdriver` 不存在、CPU/内存/屏幕/DPI/时区/WebGL 全部与档案一致 → 零 CDP 端口 → OS 级输入点击生效。

**Architecture:** 五层（见 `docs/ARCHITECTURE.md`）。本切片只实现 L0–L3 的最小可用路径，其中 L1 原生 Shim 是全部风险所在：**不依赖 Blink/V8 符号**，改走 OS API Hook（通道 A）+ ANGLE 导出 Hook（通道 B）+ 二进制补丁（通道 C）。

**Tech Stack:** C++20 (MSVC, Shim/Launcher) · Rust (ghostprof/ghostd) · Python 3.11 (harness/captcha) · MinHook (inline hook) · Windows 优先，接口按三平台设计

---

## 前置事实（已实测，勿再假设）

| 事实 | 值 |
|---|---|
| 本机 CPU/RAM | Xeon E3-1280 v5 4C8T / 16GB |
| 编译器 | MSVC 14.44.35207 (BuildTools 2022) — **实测可编译 x64 C++20** |
| Rust | stable-msvc + stable-gnu 双工具链 |
| `chrome.dll` 导出 | **仅 5 个函数**，零 v8/blink 符号 |
| Chrome PDB | **不附带** |
| 网络 | google.com 不通；github.com / cef-builds.spotifycdn.com 通 |
| WSL | Ubuntu 22.04 + 24.04（Linux 分支可测） |
| 本机 Chrome | 154.0.8037.98 |

---

## 文件结构

```
unknowbrowser/
├── docs/
│   ├── ARCHITECTURE.md                  # 已写
│   └── superpowers/plans/2026-10-06-ghost-browser-slice.md   # 本文件
├── native/
│   ├── CMakeLists.txt
│   ├── vendor/minhook/                  # git submodule/vendored
│   ├── ghost_shim/                      # 原生 Shim DLL
│   │   ├── include/ghost_profile.h      # 档案结构 + 解析
│   │   ├── src/dllmain.cpp              # 入口：读 env → 装 hook
│   │   ├── src/hook_engine.cpp/.h       # MinHook 封装
│   │   ├── src/hooks_sysinfo.cpp        # CPU / RAM
│   │   ├── src/hooks_display.cpp        # 屏幕 / DPI
│   │   ├── src/hooks_time.cpp           # 时区 / locale
│   │   ├── src/hooks_gpu.cpp            # DXGI / ANGLE
│   │   └── src/hooks_proc.cpp           # CreateProcessW 传播
│   ├── ghost_launch/                    # 启动器（注入）
│   │   └── src/main.cpp
│   └── tests/probe/                     # 独立探针（验证 hook）
│       └── src/main.cpp
├── ghostprof/                           # Rust: 指纹档案引擎
├── ghostd/                              # Rust: 零 CDP 控制面
├── harness/
│   ├── detect.html                      # 本地检测页
│   └── run_detect.py                    # 端到端验证器
└── tools/pe_exports.py                  # 已写
```

---

## Task 1: 构建骨架 + MinHook 落地

**Files:**
- Create: `native/CMakeLists.txt`
- Create: `native/vendor/minhook/` (git clone)
- Test: `native/tests/smoke/`

- [ ] **Step 1: 拉取 MinHook**

```powershell
cd E:\projects\unknowbrowser\native
New-Item -ItemType Directory -Force -Path vendor | Out-Null
git clone --depth 1 https://github.com/TsudaKageyu/minhook.git vendor/minhook
```

Expected: `vendor/minhook/src/hook.c`, `vendor/minhook/include/MinHook.h` 存在。

- [ ] **Step 2: 写 CMakeLists 骨架**

```cmake
cmake_minimum_required(VERSION 3.20)
project(ghost_native CXX C)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(minhook STATIC
  vendor/minhook/src/buffer.c vendor/minhook/src/hook.c
  vendor/minhook/src/trampoline.c vendor/minhook/src/hde/hde64.c)
target_include_directories(minhook PUBLIC vendor/minhook/include vendor/minhook/src)

add_library(ghost_shim SHARED src/ghost_shim/dllmain.cpp ...)
target_link_libraries(ghost_shim PRIVATE minhook)
```

- [ ] **Step 3: 验证构建**

Run: `cmake -B build -G "Visual Studio 17 2022" -A x64 && cmake --build build --config Release`
Expected: 生成 `build/Release/ghost_shim.dll`

- [ ] **Step 4: Commit**

```bash
git init && git add -A && git commit -m "build: native skeleton with vendored MinHook"
```

---

## Task 2: 档案结构与环境变量下发

**Files:**
- Create: `native/ghost_shim/include/ghost_profile.h`
- Create: `native/ghost_shim/src/profile.cpp`
- Test: `native/tests/probe/src/main.cpp`

**设计要点**：配置经环境变量 `GHOST_PROFILE_JSON` 下发。选环境变量而非 IPC 的原因：**子进程自动继承**，无需 hook `CreateProcess` 传参。

- [ ] **Step 1: 定义档案结构**

```cpp
// native/ghost_shim/include/ghost_profile.h
#pragma once
#include <cstdint>
#include <string>
#include <optional>

namespace ghost {

struct CpuProfile {
  uint32_t hardware_concurrency = 0;   // navigator.hardwareConcurrency
};

struct MemoryProfile {
  uint64_t total_bytes = 0;            // GlobalMemoryStatusEx.ullTotalPhys
};

struct ScreenProfile {
  int32_t  width = 0, height = 0;      // screen.width / height
  int32_t  avail_width = 0, avail_height = 0;
  double   device_pixel_ratio = 0.0;   // window.devicePixelRatio
  int32_t  color_depth = 24;
};

struct TimeProfile {
  std::string timezone_id;             // "Asia/Shanghai"
  int32_t  bias_minutes = -480;        // UTC offset in minutes
  std::string locale;                  // "zh-CN"
};

struct GpuProfile {
  std::string vendor;                  // "Google Inc. (NVIDIA)"
  std::string renderer;                // "ANGLE (NVIDIA, NVIDIA GeForce RTX 3060 ...)"
  std::string gl_version;
  int32_t     max_texture_size = 16384;
};

struct Profile {
  std::string   profile_id;
  std::string   profile_seed_hex;
  CpuProfile    cpu;
  MemoryProfile memory;
  ScreenProfile screen;
  TimeProfile   time;
  GpuProfile    gpu;
  bool          enabled = true;

  // 从 GHOST_PROFILE_JSON 读取；缺失或非法 → enabled=false
  static const Profile& current();
};

} // namespace ghost
```

- [ ] **Step 2: 实现解析（用 vendored 单头 JSON）**

Vendor `nlohmann/json` single header:
```powershell
curl.exe -sL -o native/vendor/json.hpp https://raw.githubusercontent.com/nlohmann/json/develop/single_include/nlohmann/json.hpp
```

```cpp
// native/ghost_shim/src/profile.cpp
#include "ghost_profile.h"
#include <cstdlib>
#include <mutex>
#include "../../vendor/json.hpp"

namespace ghost {
static std::once_flag g_once;
static Profile g_profile;

static Profile load() {
  Profile p;
  const char* raw = std::getenv("GHOST_PROFILE_JSON");
  if (!raw || !*raw) { p.enabled = false; return p; }
  try {
    auto j = nlohmann::json::parse(raw);
    p.profile_id      = j.value("profile_id", "");
    p.profile_seed_hex= j.value("profile_seed_hex", "");
    p.cpu.hardware_concurrency = j.value("cpu_hardware_concurrency", 0u);
    p.memory.total_bytes       = j.value("memory_total_bytes", 0ull);
    p.screen.width             = j.value("screen_width", 0);
    p.screen.height            = j.value("screen_height", 0);
    p.screen.avail_width       = j.value("screen_avail_width", 0);
    p.screen.avail_height      = j.value("screen_avail_height", 0);
    p.screen.device_pixel_ratio= j.value("device_pixel_ratio", 0.0);
    p.screen.color_depth       = j.value("color_depth", 24);
    p.time.timezone_id         = j.value("timezone_id", "");
    p.time.bias_minutes        = j.value("timezone_bias_minutes", 0);
    p.time.locale              = j.value("locale", "");
    p.gpu.vendor               = j.value("gpu_vendor", "");
    p.gpu.renderer             = j.value("gpu_renderer", "");
    p.gpu.gl_version           = j.value("gpu_gl_version", "");
    p.gpu.max_texture_size     = j.value("gpu_max_texture_size", 0);
    p.enabled = true;
  } catch (...) { p.enabled = false; }
  return p;
}

const Profile& Profile::current() {
  std::call_once(g_once, []{ g_profile = load(); });
  return g_profile;
}
} // namespace ghost
```

- [ ] **Step 3: 写探针 exe（尚未 hook，用于对照基线）**

```cpp
// native/tests/probe/src/main.cpp
#include <windows.h>
#include <cstdio>
#include <string>

int main() {
  printf("GHOST_PROFILE_JSON set: %s\n", getenv("GHOST_PROFILE_JSON") ? "yes" : "no");
  printf("GetActiveProcessorCount = %u\n", GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
  MEMORYSTATUSEX ms{}; ms.dwLength = sizeof(ms); GlobalMemoryStatusEx(&ms);
  printf("ullTotalPhys = %llu\n", (unsigned long long)ms.ullTotalPhys);
  printf("SM_CXSCREEN = %d  SM_CYSCREEN = %d\n",
         GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
  TIME_ZONE_INFORMATION tz{}; DWORD r = GetTimeZoneInformation(&tz);
  printf("Bias = %ld  (ret=%lu)\n", tz.Bias, r);
  return 0;
}
```

- [ ] **Step 4: 跑基线并记录真实值**

Run: `build\Release\probe.exe`
Expected: 输出 `GetActiveProcessorCount = 8`、真实内存 ~16GB、真实分辨率。**把这三个值记下来**，后续 hook 验证要靠它对照。

- [ ] **Step 5: Commit**

---

## Task 3: Hook 引擎封装

**Files:**
- Create: `native/ghost_shim/src/hook_engine.h`
- Create: `native/ghost_shim/src/hook_engine.cpp`

**设计要点**：所有 hook 用统一的 RAII 注册器，失败只记日志**不崩溃**——Shim 必须"能 hook 就 hook，hook 不了就放行"，否则浏览器直接打不开。

- [ ] **Step 1: 实现 hook_engine**

```cpp
// native/ghost_shim/src/hook_engine.h
#pragma once
#include <string>
#include <vector>
#include <functional>

namespace ghost {
struct HookStats { int installed = 0; int failed = 0; std::vector<std::string> failures; };
const HookStats& stats();

// 安装一个 inline hook。target 为 null 时计入 failed 而非崩溃。
bool install_hook(const char* name, void* target, void* detour, void** trampoline);
// 便捷：按 DLL+函数名
bool install_hook_export(const char* dll, const char* fn, void* detour, void** trampoline);
// 全部启用（MinHook 需要集中 EnableHook）
void enable_all();
} // namespace ghost
```

```cpp
// native/ghost_shim/src/hook_engine.cpp
#include "hook_engine.h"
#include <windows.h>
#include <cstdio>
#include <mutex>
#include "MinHook.h"

namespace ghost {
static HookStats g_stats;
static std::mutex g_mu;
const HookStats& stats() { return g_stats; }

bool install_hook(const char* name, void* target, void* detour, void** trampoline) {
  std::lock_guard lk(g_mu);
  if (!target) { g_stats.failed++; g_stats.failures.push_back(std::string(name) + ": null target"); return false; }
  MH_STATUS s = MH_CreateHook(target, detour, trampoline);
  if (s != MH_OK) {
    g_stats.failed++;
    g_stats.failures.push_back(std::string(name) + ": " + MH_StatusToString(s));
    return false;
  }
  g_stats.installed++;
  return true;
}

bool install_hook_export(const char* dll, const char* fn, void* detour, void** trampoline) {
  HMODULE m = GetModuleHandleA(dll);
  if (!m) m = LoadLibraryA(dll);
  void* t = m ? (void*)GetProcAddress(m, fn) : nullptr;
  std::string n = std::string(dll) + "!" + fn;
  return install_hook(n.c_str(), t, detour, trampoline);
}

void enable_all() { MH_EnableHook(MH_ALL_HOOKS); }
} // namespace ghost
```

- [ ] **Step 2: Commit**

---

## Task 4: CPU / 内存 Hook（第一个端到端验证点）

**Files:**
- Create: `native/ghost_shim/src/hooks_sysinfo.cpp`
- Modify: `native/ghost_shim/src/dllmain.cpp`

- [ ] **Step 1: 实现 sysinfo hooks**

```cpp
// native/ghost_shim/src/hooks_sysinfo.cpp
#include <windows.h>
#include "../include/ghost_profile.h"
#include "hook_engine.h"

namespace ghost {
static DWORD (WINAPI *real_GetActiveProcessorCount)(WORD) = nullptr;
static BOOL  (WINAPI *real_GlobalMemoryStatusEx)(LPMEMORYSTATUSEX) = nullptr;
static void  (WINAPI *real_GetSystemInfo)(LPSYSTEM_INFO) = nullptr;

static DWORD WINAPI hook_GetActiveProcessorCount(WORD group) {
  const Profile& p = Profile::current();
  if (p.enabled && p.cpu.hardware_concurrency) return p.cpu.hardware_concurrency;
  return real_GetActiveProcessorCount(group);
}

static BOOL WINAPI hook_GlobalMemoryStatusEx(LPMEMORYSTATUSEX s) {
  BOOL ok = real_GlobalMemoryStatusEx(s);
  const Profile& p = Profile::current();
  if (ok && p.enabled && p.memory.total_bytes) {
    // 保持 ullAvailPhys 与 ullTotalPhys 的比例合理
    double ratio = s->ullTotalPhys ? (double)s->ullAvailPhys / (double)s->ullTotalPhys : 0.5;
    s->ullTotalPhys = p.memory.total_bytes;
    s->ullAvailPhys = (DWORD64)(p.memory.total_bytes * ratio);
    s->dwTotalPhys  = (DWORD)p.memory.total_bytes;
    s->dwAvailPhys  = (DWORD)s->ullAvailPhys;
  }
  return ok;
}

static void WINAPI hook_GetSystemInfo(LPSYSTEM_INFO si) {
  real_GetSystemInfo(si);
  const Profile& p = Profile::current();
  if (p.enabled && p.cpu.hardware_concurrency) {
    si->dwNumberOfProcessors = p.cpu.hardware_concurrency;
  }
}

void install_sysinfo_hooks() {
  install_hook_export("kernel32.dll", "GetActiveProcessorCount",
      (void*)hook_GetActiveProcessorCount, (void**)&real_GetActiveProcessorCount);
  install_hook_export("kernel32.dll", "GlobalMemoryStatusEx",
      (void*)hook_GlobalMemoryStatusEx, (void**)&real_GlobalMemoryStatusEx);
  install_hook_export("kernel32.dll", "GetSystemInfo",
      (void*)hook_GetSystemInfo, (void**)&real_GetSystemInfo);
}
} // namespace ghost
```

- [ ] **Step 2: dllmain 串起来**

```cpp
// native/ghost_shim/src/dllmain.cpp
#include <windows.h>
#include <cstdio>
#include "../include/ghost_profile.h"
#include "hook_engine.h"

namespace ghost { void install_sysinfo_hooks(); void install_display_hooks();
                  void install_time_hooks(); void install_gpu_hooks(); void install_proc_hooks(); }

static void logf(const char* fmt, ...) {
  // 输出到 %TEMP%\ghost_shim.log，方便验证期排查
  char path[MAX_PATH]; GetTempPathA(MAX_PATH, path); strcat_s(path, "ghost_shim.log");
  FILE* f = nullptr; fopen_s(&f, path, "a"); if (!f) return;
  va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
  fputc('\n', f); fclose(f);
}

static DWORD WINAPI init_thread(LPVOID) {
  MH_Initialize();
  const ghost::Profile& p = ghost::Profile::current();
  logf("[ghost] pid=%lu enabled=%d profile=%s", GetCurrentProcessId(), (int)p.enabled, p.profile_id.c_str());
  if (!p.enabled) return 0;
  ghost::install_sysinfo_hooks();
  ghost::install_display_hooks();
  ghost::install_time_hooks();
  ghost::install_gpu_hooks();
  ghost::install_proc_hooks();
  ghost::enable_all();
  const auto& s = ghost::stats();
  logf("[ghost] hooks installed=%d failed=%d", s.installed, s.failed);
  for (auto& f : s.failures) logf("[ghost]   FAIL %s", f.c_str());
  return 0;
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(nullptr);
    // 不能在 DllMain 里做重活（loader lock），另起线程
    HANDLE h = CreateThread(nullptr, 0, init_thread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
  }
  return TRUE;
}
```

> ⚠️ **关键**：绝不能在 `DllMain` 里做 hook 安装——loader lock 下 `LoadLibrary` 会死锁。必须另起线程。

- [ ] **Step 3: 先用最小可验证集构建（display/time/gpu/proc 先留空实现）**

- [ ] **Step 4: 写注入器做验证**

```powershell
# 手工验证：把 shim 注入 probe.exe
$env:GHOST_PROFILE_JSON='{"profile_id":"p1","cpu_hardware_concurrency":4,"memory_total_bytes":8589934592}'
E:\projects\unknowbrowser\native\build\Release\ghost_launch.exe --inject ghost_shim.dll -- probe.exe
```

Expected: 探针输出 `GetActiveProcessorCount = 4`、`ullTotalPhys = 8589934592`（8GB），而非真实 8 / 16GB。

- [ ] **Step 5: Commit**

---

## Task 5: 启动器 ghost_launch

**Files:**
- Create: `native/ghost_launch/src/main.cpp`

- [ ] **Step 1: 实现 SUSPENDED 注入**

```cpp
// native/ghost_launch/src/main.cpp  (核心逻辑)
#include <windows.h>
#include <string>
#include <vector>
#include <cstdio>

static bool inject_dll(HANDLE proc, const char* dll_path) {
  SIZE_T len = strlen(dll_path) + 1;
  void* remote = VirtualAllocEx(proc, nullptr, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!remote) return false;
  if (!WriteProcessMemory(proc, remote, dll_path, len, nullptr)) return false;
  HMODULE k32 = GetModuleHandleA("kernel32.dll");
  void* load_lib = (void*)GetProcAddress(k32, "LoadLibraryA");
  HANDLE th = CreateRemoteThread(proc, nullptr, 0,
      (LPTHREAD_START_ROUTINE)load_lib, remote, 0, nullptr);
  if (!th) return false;
  WaitForSingleObject(th, 15000);
  DWORD code = 0; GetExitCodeThread(th, &code);
  CloseHandle(th);
  VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
  return code != 0;   // LoadLibrary 返回模块基址
}

int main(int argc, char** argv) {
  // 用法: ghost_launch --dll <shim.dll> --exe <chrome.exe> [-- args...]
  std::string dll, exe; std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--dll") dll = argv[++i];
    else if (a == "--exe") exe = argv[++i];
    else if (a == "--") { for (int j = i + 1; j < argc; ++j) args.push_back(argv[j]); break; }
    else args.push_back(a);
  }

  std::string cmd = "\"" + exe + "\"";
  for (auto& a : args) cmd += " \"" + a + "\"";

  STARTUPINFOA si{}; si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                      CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
    fprintf(stderr, "CreateProcess failed: %lu\n", GetLastError());
    return 1;
  }
  if (!inject_dll(pi.hProcess, dll.c_str())) {
    fprintf(stderr, "inject failed: %lu\n", GetLastError());
    TerminateProcess(pi.hProcess, 1);
    return 2;
  }
  ResumeThread(pi.hThread);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return 0;
}
```

> ⚠️ `bInheritHandles=TRUE` 是必需的，否则 Chromium 子进程拿不到继承的环境变量与句柄。

- [ ] **Step 2: 验证注入 probe.exe**

Run: 同 Task 4 Step 4
Expected: 同上

- [ ] **Step 3: Commit**

---

## Task 6: 子进程传播（hook CreateProcessW）

**Files:**
- Create: `native/ghost_shim/src/hooks_proc.cpp`

**设计要点**：Chromium 渲染进程由浏览器进程创建。给子进程加 `CREATE_SUSPENDED` → 注入 → 恢复。原本就带 `CREATE_SUSPENDED` 的（Chromium 用于分配 job object）不重复恢复。

- [ ] **Step 1: 实现**

```cpp
// native/ghost_shim/src/hooks_proc.cpp
#include <windows.h>
#include <string>
#include "hook_engine.h"

namespace ghost {
static BOOL (WINAPI *real_CreateProcessW)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES,
    LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION) = nullptr;
static BOOL (WINAPI *real_CreateProcessAsUserW)(HANDLE, LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES,
    LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION) = nullptr;
static std::wstring g_shim_path;
static bool g_in_hook = false;   // 防重入

static bool inject(HANDLE proc, const std::wstring& dll);

static BOOL WINAPI hook_CreateProcessW(LPCWSTR app, LPWSTR cmd, LPSECURITY_ATTRIBUTES pa,
    LPSECURITY_ATTRIBUTES ta, BOOL inherit, DWORD flags, LPVOID env, LPCWSTR cwd,
    LPSTARTUPINFOW si, LPPROCESS_INFORMATION pi)
{
  if (g_in_hook || g_shim_path.empty())
    return real_CreateProcessW(app, cmd, pa, ta, inherit, flags, env, cwd, si, pi);
  g_in_hook = true;
  bool was_suspended = (flags & CREATE_SUSPENDED) != 0;
  BOOL ok = real_CreateProcessW(app, cmd, pa, ta, inherit, flags | CREATE_SUSPENDED, env, cwd, si, pi);
  if (ok) {
    inject(pi->hProcess, g_shim_path);
    if (!was_suspended) ResumeThread(pi->hThread);
  }
  g_in_hook = false;
  return ok;
}

// inject() 与 ghost_launch 中的注入逻辑同构（VirtualAllocEx + WriteProcessMemory +
// CreateRemoteThread(LoadLibraryW) + WaitForSingleObject）
// CreateProcessAsUserW 同理包装。

void install_proc_hooks() {
  wchar_t buf[MAX_PATH]; GetModuleFileNameW(nullptr, buf, MAX_PATH);
  g_shim_path = buf;
  install_hook_export("kernel32.dll", "CreateProcessW",
      (void*)hook_CreateProcessW, (void**)&real_CreateProcessW);
  install_hook_export("kernel32.dll", "CreateProcessAsUserW",
      (void*)hook_CreateProcessAsUserW, (void**)&real_CreateProcessAsUserW);
}
} // namespace ghost
```

- [ ] **Step 2: 验证传播**——探针进程再启动一个 probe.exe，检查孙进程也被 hook
- [ ] **Step 3: Commit**

---

## Task 7: 屏幕 / DPI Hook

**Files:** Create `native/ghost_shim/src/hooks_display.cpp`

Hook 点：`EnumDisplayMonitors`（重写 `MONITORINFO`）、`GetMonitorInfoW`、`GetSystemMetrics`（`SM_CXSCREEN/SM_CYSCREEN/SM_CXMAXIMIZED/...`）、`GetDpiForMonitor`、`GetDpiForWindow`、`GetDeviceCaps`。

**要点**：`screen.width` 与窗口尺寸必须自洽——档案里 `screen_width >= window_width`，且 DPR 与屏幕分辨率的组合要落在真实存在的档位（如 1920×1080@1.0、2560×1440@1.25、3840×2160@1.5）。

- [ ] **Step 1–4**: 实现 → 探针验证 → 检测页验证 → Commit

---

## Task 8: 时区 / Locale Hook

**Files:** Create `native/ghost_shim/src/hooks_time.cpp`

Hook 点：`GetTimeZoneInformation`、`GetDynamicTimeZoneInformation`、`GetUserDefaultLocaleName`、`GetLocaleInfoEx`。
另加启动参数：`--lang=<locale>` 与 `Accept-Language`。

- [ ] **Step 1–4**: 实现 → 探针验证 `Bias` 被改写 → 检测页验证 `Intl.DateTimeFormat().resolvedOptions().timeZone` → Commit

---

## Task 9: GPU / WebGL Hook（通道 B）

**Files:** Create `native/ghost_shim/src/hooks_gpu.cpp`

两条路径，按优先级：
1. **ANGLE 导出**：hook `libGLESv2.dll` 的 `glGetString`、`glGetIntegerv`、`glGetFloatv`、`glReadPixels`。ANGLE 是延迟加载的 → 需 hook `LoadLibraryExW`，在加载后补装。
2. **DXGI 回退**：hook `dxgi.dll!CreateDXGIFactory1`，包装返回的 factory，重写 `IDXGIAdapter::GetDesc1` 的 `Description`/`VendorId`/`DeviceId`。

- [ ] **Step 1–4**: 实现 → 检测页验证 `WEBGL_debug_renderer_info` 的 UNMASKED_VENDOR/RENDERER 与档案一致 → Commit

---

## Task 10: 检测页 + 端到端验证器

**Files:**
- Create: `harness/detect.html`
- Create: `harness/run_detect.py`

**设计要点**：零 CDP 意味着不能从外部读页面结果。检测页把指纹 JSON **POST 到本地 HTTP 服务器**，验证器据此断言。

- [ ] **Step 1: detect.html** 采集并上报：
`navigator.webdriver`、`navigator.hardwareConcurrency`、`navigator.deviceMemory`、`navigator.platform`、`navigator.userAgent`、`navigator.userAgentData`、`screen.*`、`window.devicePixelRatio`、`Intl.DateTimeFormat().resolvedOptions()`、`WebGL UNMASKED_*`、`Object.getOwnPropertyNames(window)` 差集、`Function.prototype.toString` 原生性抽查、`performance.now()` 精度、CDP 探测（尝试 fetch 127.0.0.1:9222）

- [ ] **Step 2: run_detect.py** 启动本地服务器 → `ghost_launch` 启动 Chromium → 等待上报 → 与档案逐项断言 → 输出评分表

- [ ] **Step 3: 跑通**——期望：`webdriver=undefined`、CPU/内存/屏幕/DPR/时区/WebGL 全部等于档案值、无 CDP 端口

- [ ] **Step 4: Commit**

---

## Task 11: ghostprof 指纹档案引擎（Rust）

**Files:** Create `ghostprof/` (cargo)

- [ ] 档案 schema + 相干性约束校验器（GPU↔WebGL、UA↔UA-CH↔字体集、屏幕↔DPR、时区↔locale↔geoip）
- [ ] 相干档案生成器（从真实设备样本库加权采样，而非随机拼凑）
- [ ] 种子派生：`noise_seed = HMAC-SHA256(profile_seed, origin + ":" + surface)`
- [ ] `ghostprof export --id p1 --format env` → 输出 `GHOST_PROFILE_JSON`
- [ ] 单测：1000 次生成全部通过相干性校验

---

## Task 12: ghostd 零 CDP 控制面（Rust）

**Files:** Create `ghostd/`

- [ ] 会话管理 + JSON-RPC over 本地 socket
- [ ] OS 输入后端（Windows `SendInput`；接口预留 XTEST/CGEventPost）
- [ ] 无障碍树读取（Windows UIA；`--force-renderer-accessibility`）
- [ ] OS 级截屏（`PrintWindow`/`BitBlt`）
- [ ] profile SQLite 直读（Cookies / Local Storage）
- [ ] Python + Node SDK

---

## Task 13: ghostcaptcha 三级降级

**Files:** Create `captcha/`

- [ ] CF 挑战识别 + 静默通过等待 + `cf_clearance` 从 SQLite 收割/复用
- [ ] hCaptcha 音频挑战：定位 iframe → OS 输入点击音频按钮 → 抓取音频 URL → 本地 Whisper 转写 → 填入 → 提交
- [ ] 第三方兜底 provider 接口（2Captcha / CapSolver）
- [ ] 集成测试：Turnstile demo 页 + hCaptcha demo 页

---

## Task 14: Linux / macOS 分支

- [ ] Linux：`ghost_shim.so`（`LD_PRELOAD`）+ `sched_getaffinity`/`sysinfo`/X11/fontconfig hook；WSL 内验证
- [ ] macOS：`ghost_shim.dylib`（`DYLD_INSERT_LIBRARIES` + ad-hoc 重签名去 library validation）+ `sysctlbyname`/CoreGraphics/CoreText hook
- [ ] 构建矩阵与 CI

---

## Self-Review

**Spec coverage：** 用户四项需求 → 底座（Task 1/5）、零 CDP（Task 10/12）、验证码三级（Task 13）、垂直切片（Task 2–10）、跨平台（Task 14）。**已覆盖。**

**已知缺口（不在本切片内，但已记录）：** Canvas / Audio 指纹——见 `docs/ARCHITECTURE.md` §3.6，需 Track B 源码级 patch。

**风险：**
1. Chromium 沙箱可能阻止注入 → 缓解：在子进程 resume 前注入；退路是 `--no-sandbox`（页面不可探测，但降低安全性）
2. MinHook 在 CFG 下失效 → 缓解：改用 VEH/硬件断点方案
3. `GetActiveProcessorCount` 在部分版本被 `base::SysInfo` 提前缓存 → 缓解：Shim 在 `DllMain` 阶段（早于 Chromium main）就装 hook
