// hooks_gpu.cpp — channel B: WebGL vendor/renderer strings and the GPU descriptor.
//
// JS surface covered:
//   WEBGL_debug_renderer_info.UNMASKED_VENDOR_WEBGL
//   WEBGL_debug_renderer_info.UNMASKED_RENDERER_WEBGL
//   gl.getParameter(gl.VERSION) / gl.getParameter(gl.MAX_TEXTURE_SIZE)
//
// Why this works without any symbol reverse engineering: ANGLE ships as two ordinary
// DLLs (libGLESv2.dll / libEGL.dll) that export the entire GL entry point surface.
// Chromium's GPU process drives ANGLE through those exports, so an inline hook on
// glGetString intercepts the value before it ever enters the command buffer that the
// renderer forwards to the page. No Blink or V8 symbols are involved.
//
// MinHook patches the function *code*, not the export table, so ANGLE resolving its
// own entry points internally — or Chromium fetching them via eglGetProcAddress —
// still lands on the hooked address.
#include <windows.h>

#include <cwchar>
#include <string>

#include "../include/ghost_profile.h"
#include "hook_engine.h"

// Minimal GL typedefs so the shim does not need the OpenGL SDK headers.
using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLubyte = unsigned char;

namespace ghost {
namespace {

constexpr GLenum kGLVendor = 0x1F00;
constexpr GLenum kGLRenderer = 0x1F01;
constexpr GLenum kGLVersion = 0x1F02;
constexpr GLenum kGLShadingLanguageVersion = 0x8B8C;
constexpr GLenum kGLMaxTextureSize = 0x0D33;

const GLubyte* (WINAPI* real_glGetString)(GLenum) = nullptr;
const GLubyte* (WINAPI* real_glGetStringi)(GLenum, GLuint) = nullptr;
void(WINAPI* real_glGetIntegerv)(GLenum, GLint*) = nullptr;

bool g_gl_hooks_installed = false;

// Static storage is mandatory: GL string pointers must stay valid for the lifetime
// of the context, and callers never free them.
const GLubyte* cached(const std::string& s) {
  static std::string storage[4];
  static int next = 0;
  storage[next % 4] = s;
  return reinterpret_cast<const GLubyte*>(storage[next++ % 4].c_str());
}

const GLubyte* WINAPI hook_glGetString(GLenum name) {
  const Profile& p = Profile::current();
  if (p.enabled) {
    switch (name) {
      case kGLVendor: return p.gpu.vendor.empty() ? real_glGetString(name) : cached(p.gpu.vendor);
      case kGLRenderer: return p.gpu.renderer.empty() ? real_glGetString(name) : cached(p.gpu.renderer);
      case kGLVersion: return p.gpu.gl_version.empty() ? real_glGetString(name) : cached(p.gpu.gl_version);
      default: break;
    }
  }
  return real_glGetString ? real_glGetString(name) : nullptr;
}

const GLubyte* WINAPI hook_glGetStringi(GLenum name, GLuint index) {
  const Profile& p = Profile::current();
  if (p.enabled && index == 0) {
    switch (name) {
      case kGLVendor: return p.gpu.vendor.empty() ? real_glGetStringi(name, index) : cached(p.gpu.vendor);
      case kGLRenderer: return p.gpu.renderer.empty() ? real_glGetStringi(name, index) : cached(p.gpu.renderer);
      case kGLVersion: return p.gpu.gl_version.empty() ? real_glGetStringi(name, index) : cached(p.gpu.gl_version);
      default: break;
    }
  }
  return real_glGetStringi ? real_glGetStringi(name, index) : nullptr;
}

void WINAPI hook_glGetIntegerv(GLenum pname, GLint* params) {
  if (real_glGetIntegerv == nullptr) return;
  real_glGetIntegerv(pname, params);
  const Profile& p = Profile::current();
  if (params != nullptr && p.enabled && pname == kGLMaxTextureSize &&
      p.gpu.max_texture_size > 0) {
    *params = p.gpu.max_texture_size;
  }
}

// ---------------------------------------------------------------------------
// DXGI adapter description
//
// IDXGIAdapter::GetDesc1 feeds Chromium's GPU info collector, which decides WebGL
// blocklisting and populates chrome://gpu. It must agree with the ANGLE strings
// above, otherwise the profile is internally inconsistent.
//
// The vtable is shared by every adapter instance of the same class and lives in
// dxgi.dll's read-only data, so patching one slot in place fixes all adapters at
// once. That is simpler and less fragile than constructing wrapper COM objects,
// which would have to forward every method and survive refcounting.
// ---------------------------------------------------------------------------
struct DXGI_ADAPTER_DESC {
  wchar_t Description[128];
  UINT VendorId;
  UINT DeviceId;
  UINT SubSysId;
  UINT Revision;
  SIZE_T DedicatedVideoMemory;
  SIZE_T DedicatedSystemMemory;
  SIZE_T SharedSystemMemory;
  LUID AdapterLuid;
};

struct DXGI_ADAPTER_DESC1 {
  DXGI_ADAPTER_DESC desc;
  UINT Flags;
};

HRESULT(STDMETHODCALLTYPE* real_GetDesc)(void*, DXGI_ADAPTER_DESC*) = nullptr;
HRESULT(STDMETHODCALLTYPE* real_GetDesc1)(void*, DXGI_ADAPTER_DESC1*) = nullptr;
bool g_vtable_patched = false;

// Rewrites the adapter identity ANGLE reads. Description and DeviceId must move
// together: ANGLE appends "(0x%08X)" built from DeviceId, so spoofing only the
// description leaves the real device id visible right next to the fake name.
void write_adapter_fields(DXGI_ADAPTER_DESC& d) {
  const Profile& p = Profile::current();
  if (!p.enabled) return;

  if (!p.gpu.adapter_description.empty()) {
    const std::wstring wide(p.gpu.adapter_description.begin(),
                            p.gpu.adapter_description.end());
    const size_t n = wide.size() < 127 ? wide.size() : 127;
    std::wmemcpy(d.Description, wide.c_str(), n);
    d.Description[n] = L'\0';
  }
  if (p.gpu.adapter_vendor_id != 0) d.VendorId = p.gpu.adapter_vendor_id;
  if (p.gpu.adapter_device_id != 0) d.DeviceId = p.gpu.adapter_device_id;
  if (p.gpu.adapter_video_memory != 0) {
    d.DedicatedVideoMemory = static_cast<SIZE_T>(p.gpu.adapter_video_memory);
  }
}

HRESULT STDMETHODCALLTYPE hook_GetDesc(void* self, DXGI_ADAPTER_DESC* out) {
  if (real_GetDesc == nullptr) return E_FAIL;
  const HRESULT hr = real_GetDesc(self, out);
  if (SUCCEEDED(hr) && out != nullptr) write_adapter_fields(*out);
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_GetDesc1(void* self, DXGI_ADAPTER_DESC1* out) {
  if (real_GetDesc1 == nullptr) return E_FAIL;
  const HRESULT hr = real_GetDesc1(self, out);
  if (SUCCEEDED(hr) && out != nullptr) write_adapter_fields(out->desc);
  return hr;
}

void patch_adapter_vtable(void* adapter) {
  if (adapter == nullptr || g_vtable_patched) return;
  void** vtable = *reinterpret_cast<void***>(adapter);

  DWORD old_protect = 0;
  if (!VirtualProtect(vtable, sizeof(void*) * 12, PAGE_EXECUTE_READWRITE, &old_protect)) {
    ghost_log("dxgi: VirtualProtect on adapter vtable failed: %lu", GetLastError());
    return;
  }
  real_GetDesc = reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*, DXGI_ADAPTER_DESC*)>(vtable[8]);
  real_GetDesc1 = reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*, DXGI_ADAPTER_DESC1*)>(vtable[10]);
  vtable[8] = reinterpret_cast<void*>(hook_GetDesc);
  vtable[10] = reinterpret_cast<void*>(hook_GetDesc1);
  VirtualProtect(vtable, sizeof(void*) * 12, old_protect, &old_protect);
  g_vtable_patched = true;
  ghost_log("dxgi: adapter vtable patched (GetDesc=%p GetDesc1=%p)", vtable[8], vtable[10]);
}

// Any of the three factory entry points is enough to reach an adapter instance and
// therefore its vtable. Chromium and ANGLE do not agree on which one they use, and
// it varies by Windows version, so all three that dxgi.dll actually exports get a
// detour. A missing export is recorded as a normal, ignorable hook failure.
void patch_factory(void** factory) {
  if (factory == nullptr || *factory == nullptr) return;
  // IDXGIFactory1::EnumAdapters1 is vtable slot 12.
  void** vtable = *reinterpret_cast<void***>(*factory);
  using EnumAdapters1Fn = HRESULT(STDMETHODCALLTYPE*)(void*, UINT, void**);
  auto enum_adapters1 = reinterpret_cast<EnumAdapters1Fn>(vtable[12]);
  void* adapter = nullptr;
  if (SUCCEEDED(enum_adapters1(*factory, 0, &adapter)) && adapter != nullptr) {
    patch_adapter_vtable(adapter);
  }
}

using CreateDXGIFactory1Fn = HRESULT(WINAPI*)(const GUID&, void**);
using CreateDXGIFactory2Fn = HRESULT(WINAPI*)(UINT, const GUID&, void**);
CreateDXGIFactory1Fn real_CreateDXGIFactory = nullptr;
CreateDXGIFactory1Fn real_CreateDXGIFactory1 = nullptr;
CreateDXGIFactory2Fn real_CreateDXGIFactory2 = nullptr;

HRESULT WINAPI hook_CreateDXGIFactory(const GUID& iid, void** factory) {
  if (real_CreateDXGIFactory == nullptr) return E_FAIL;
  const HRESULT hr = real_CreateDXGIFactory(iid, factory);
  if (SUCCEEDED(hr)) patch_factory(factory);
  return hr;
}

HRESULT WINAPI hook_CreateDXGIFactory1(const GUID& iid, void** factory) {
  if (real_CreateDXGIFactory1 == nullptr) return E_FAIL;
  const HRESULT hr = real_CreateDXGIFactory1(iid, factory);
  if (SUCCEEDED(hr)) patch_factory(factory);
  return hr;
}

HRESULT WINAPI hook_CreateDXGIFactory2(UINT flags, const GUID& iid, void** factory) {
  if (real_CreateDXGIFactory2 == nullptr) return E_FAIL;
  const HRESULT hr = real_CreateDXGIFactory2(flags, iid, factory);
  if (SUCCEEDED(hr)) patch_factory(factory);
  return hr;
}

// dxgi.dll is loaded lazily and only in the GPU process, long after the shim
// attaches, so this cannot be a one-shot at attach time. It is called both from
// install_gpu_hooks (in case it is somehow already resident) and from the
// LdrLoadDll detour when dxgi.dll actually appears.
bool g_dxgi_hooks_installed = false;

void install_dxgi_hooks() {
  if (g_dxgi_hooks_installed) return;
  if (GetModuleHandleA("dxgi.dll") == nullptr) return;
  g_dxgi_hooks_installed = true;

  const bool a = install_hook_export("dxgi.dll", "CreateDXGIFactory",
                                     reinterpret_cast<void*>(hook_CreateDXGIFactory),
                                     reinterpret_cast<void**>(&real_CreateDXGIFactory));
  const bool b = install_hook_export("dxgi.dll", "CreateDXGIFactory1",
                                     reinterpret_cast<void*>(hook_CreateDXGIFactory1),
                                     reinterpret_cast<void**>(&real_CreateDXGIFactory1));
  const bool c = install_hook_export("dxgi.dll", "CreateDXGIFactory2",
                                     reinterpret_cast<void*>(hook_CreateDXGIFactory2),
                                     reinterpret_cast<void**>(&real_CreateDXGIFactory2));
  ghost_log("dxgi: factory hooks installed (f=%d f1=%d f2=%d)", a ? 1 : 0, b ? 1 : 0,
            c ? 1 : 0);
  hook_engine_enable_all();
}

void install_gl_hooks() {
  if (g_gl_hooks_installed) return;
  g_gl_hooks_installed = true;
  install_hook_export("libGLESv2.dll", "glGetString",
                      reinterpret_cast<void*>(hook_glGetString),
                      reinterpret_cast<void**>(&real_glGetString));
  install_hook_export("libGLESv2.dll", "glGetStringi",
                      reinterpret_cast<void*>(hook_glGetStringi),
                      reinterpret_cast<void**>(&real_glGetStringi));
  install_hook_export("libGLESv2.dll", "glGetIntegerv",
                      reinterpret_cast<void*>(hook_glGetIntegerv),
                      reinterpret_cast<void**>(&real_glGetIntegerv));
  hook_engine_enable_all();
}

// ---------------------------------------------------------------------------
// Load-time hook
//
// libGLESv2.dll does not exist in the browser process; it appears later in the GPU
// process. Hooking LdrLoadDll rather than LoadLibraryW matters because Chromium
// frequently calls the ntdll entry point directly, bypassing the kernel32 wrapper.
// ---------------------------------------------------------------------------
struct UNICODE_STRING_T {
  USHORT Length;
  USHORT MaximumLength;
  PWSTR Buffer;
};

using LdrLoadDllFn = LONG(NTAPI*)(PWSTR, PULONG, UNICODE_STRING_T*, PHANDLE);
LdrLoadDllFn real_LdrLoadDll = nullptr;
thread_local bool t_in_ldr_hook = false;

const wchar_t* basename_of(const UNICODE_STRING_T* name) {
  if (name == nullptr || name->Buffer == nullptr) return nullptr;
  const wchar_t* base = name->Buffer;
  for (const wchar_t* q = name->Buffer; *q != L'\0'; ++q) {
    if (*q == L'\\' || *q == L'/') base = q + 1;
  }
  return base;
}

bool is_module_named(const UNICODE_STRING_T* name, const wchar_t* wanted) {
  const wchar_t* base = basename_of(name);
  return base != nullptr && _wcsicmp(base, wanted) == 0;
}

bool is_angle_module(const UNICODE_STRING_T* name) {
  return is_module_named(name, L"libGLESv2.dll") || is_module_named(name, L"libEGL.dll");
}

LONG NTAPI hook_LdrLoadDll(PWSTR search_path, PULONG flags, UNICODE_STRING_T* name,
                           PHANDLE module) {
  if (real_LdrLoadDll == nullptr) return -1;
  const LONG status = real_LdrLoadDll(search_path, flags, name, module);
  if (t_in_ldr_hook || status < 0) return status;

  t_in_ldr_hook = true;
  if (is_angle_module(name)) {
    install_gl_hooks();
  } else if (is_module_named(name, L"dxgi.dll")) {
    install_dxgi_hooks();
  }
  t_in_ldr_hook = false;
  return status;
}

}  // namespace

void install_gpu_hooks() {
  // ANGLE may already be present (rare, but possible when the shim is injected into
  // an already-running process).
  if (GetModuleHandleA("libGLESv2.dll") != nullptr) install_gl_hooks();

  // The load-time hook is what makes the DXGI path work at all: dxgi.dll is not
  // resident yet when the shim attaches to the GPU process, so install_dxgi_hooks
  // below is a no-op at this point and the real installation happens from inside
  // this detour, the moment dxgi.dll appears.
  install_hook_export("ntdll.dll", "LdrLoadDll",
                      reinterpret_cast<void*>(hook_LdrLoadDll),
                      reinterpret_cast<void**>(&real_LdrLoadDll));

  install_dxgi_hooks();
}

}  // namespace ghost
