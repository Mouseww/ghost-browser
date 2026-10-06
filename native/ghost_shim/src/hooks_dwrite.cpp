// hooks_dwrite.cpp — font enumeration, filtered through DirectWrite.
//
// Why this surface and not canvas or audio: canvas pixels and audio samples are
// produced by Skia and Blink inside the renderer and never cross an OS API, so no
// OS-level hook can reach them. Font enumeration does cross one. Blink does not
// call GDI's EnumFontFamiliesExW on Windows; Skia's DirectWrite font manager asks
// IDWriteFactory::GetSystemFontCollection for the installed set and then
// IDWriteFontCollection::FindFamilyName for every family a page names, which is
// what document.fonts.check() and canvas text measurement ultimately hit.
//
// IDWriteFontCollection is a COM object we cannot cheaply subclass, but we do not
// have to: its vtable is writable, so the same trick that spoofs the DXGI adapter
// works here. The real methods are captured as trampolines first and consulted
// for every question, so the filter is expressed as "is the family the real
// collection just resolved on our allow-list?" rather than by string matching.
// That matters because a family has many localized names (MS Gothic is also
// "ＭＳ ゴシック") and matching on the requested string would reject legitimate
// aliases while letting the canonical name through.
#include "hooks_dwrite.h"

#include <windows.h>
#include <dwrite.h>

#include <cstdint>
#include <string>
#include <vector>

#include "ghost_profile.h"
#include "hook_engine.h"

namespace ghost {
namespace {

// Vtable slots, fixed by the COM interfaces since Windows 7.
//   IDWriteFactory         : IUnknown(0-2), GetSystemFontCollection = 3
//   IDWriteFontCollection  : IUnknown(0-2), GetFontFamilyCount = 3,
//                            GetFontFamily = 4, FindFamilyName = 5,
//                            GetFontFromFontFace = 6
constexpr size_t kSlotGetSystemFontCollection = 3;
constexpr size_t kSlotGetFontFamilyCount = 3;
constexpr size_t kSlotGetFontFamily = 4;
constexpr size_t kSlotFindFamilyName = 5;

using GetSystemFontCollectionFn = HRESULT(STDMETHODCALLTYPE*)(IDWriteFactory*,
                                                              IDWriteFontCollection**, BOOL);
using DWriteCreateFactoryFn = HRESULT(WINAPI*)(DWRITE_FACTORY_TYPE, REFIID, IUnknown**);
// GetFontFamilyCount returns UINT32, not HRESULT.
using GetFontFamilyCountFn = UINT32(STDMETHODCALLTYPE*)(IDWriteFontCollection*);
using GetFontFamilyFn = HRESULT(STDMETHODCALLTYPE*)(IDWriteFontCollection*, UINT32,
                                                    IDWriteFontFamily**);
using FindFamilyNameFn = HRESULT(STDMETHODCALLTYPE*)(IDWriteFontCollection*, const WCHAR*,
                                                     UINT32*, BOOL*);

DWriteCreateFactoryFn real_create_factory = nullptr;
GetSystemFontCollectionFn real_get_collection = nullptr;

GetFontFamilyCountFn real_family_count = nullptr;
GetFontFamilyFn real_family = nullptr;
FindFamilyNameFn real_find_family = nullptr;

bool factory_patched = false;
bool collection_patched = false;

std::vector<std::wstring> allow_list;  // lowercased
std::vector<UINT32> visible_to_real;   // our index -> the real collection's index

std::wstring to_wide(const std::string& s) {
  if (s.empty()) return std::wstring();
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                    nullptr, 0);
  if (n <= 0) return std::wstring();
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

std::wstring lowered(const std::wstring& s) {
  if (s.empty()) return std::wstring();
  std::wstring out = s;
  CharLowerBuffW(out.data(), static_cast<DWORD>(out.size()));
  return out;
}

bool allowed(const wchar_t* name) {
  if (name == nullptr) return false;
  const std::wstring key = lowered(name);
  for (const std::wstring& a : allow_list) {
    if (a == key) return true;
  }
  return false;
}

// --- the patched collection methods ----------------------------------------

UINT32 STDMETHODCALLTYPE hook_GetFontFamilyCount(IDWriteFontCollection* self) {
  (void)self;
  return static_cast<UINT32>(visible_to_real.size());
}

HRESULT STDMETHODCALLTYPE hook_GetFontFamily(IDWriteFontCollection* self, UINT32 index,
                                             IDWriteFontFamily** family) {
  if (family == nullptr) return E_INVALIDARG;
  *family = nullptr;
  if (index >= visible_to_real.size()) return E_INVALIDARG;
  return real_family(self, visible_to_real[index], family);
}

HRESULT STDMETHODCALLTYPE hook_FindFamilyName(IDWriteFontCollection* self, const WCHAR* name,
                                              UINT32* index, BOOL* exists) {
  if (index == nullptr || exists == nullptr) return E_INVALIDARG;

  // Let the real collection resolve every localized alias first; the answer is
  // then "is that family one we admit?", which needs no name matching at all.
  UINT32 real_index = UINT32_MAX;
  const HRESULT hr = real_find_family(self, name, &real_index, exists);
  if (FAILED(hr) || !*exists) return hr;

  for (size_t i = 0; i < visible_to_real.size(); ++i) {
    if (visible_to_real[i] == real_index) {
      *index = static_cast<UINT32>(i);
      return hr;
    }
  }

  // Installed on the machine, absent from the profile: report it as not
  // installed, which is exactly what makes document.fonts.check() and the canvas
  // measurement probe agree instead of contradicting each other.
  *exists = FALSE;
  *index = UINT32_MAX;
  return S_OK;
}

// --- installation -----------------------------------------------------------

// Walks the real collection once and records which of its families the profile
// admits. The index mapping has to exist because GetFontFamilyCount and
// GetFontFamily must agree with each other, and FindFamilyName returns an index
// the caller will hand straight back to GetFontFamily.
void build_visible(IDWriteFontCollection* collection) {
  visible_to_real.clear();
  const UINT32 count = real_family_count(collection);
  for (UINT32 i = 0; i < count; ++i) {
    IDWriteFontFamily* family = nullptr;
    if (FAILED(real_family(collection, i, &family)) || family == nullptr) continue;

    IDWriteLocalizedStrings* names = nullptr;
    if (SUCCEEDED(family->GetFamilyNames(&names)) && names != nullptr) {
      UINT32 at = 0;
      BOOL found = FALSE;
      names->FindLocaleName(L"en-us", &at, &found);
      if (!found) at = 0;
      WCHAR buf[256] = {};
      if (SUCCEEDED(names->GetString(at, buf, 256)) && allowed(buf)) {
        visible_to_real.push_back(i);
      }
      names->Release();
    }
    family->Release();
  }
}

void patch_collection(IDWriteFontCollection* collection) {
  if (collection_patched || collection == nullptr) return;

  void** vtable = *reinterpret_cast<void***>(collection);
  real_family_count = reinterpret_cast<GetFontFamilyCountFn>(vtable[kSlotGetFontFamilyCount]);
  real_family = reinterpret_cast<GetFontFamilyFn>(vtable[kSlotGetFontFamily]);
  real_find_family = reinterpret_cast<FindFamilyNameFn>(vtable[kSlotFindFamilyName]);
  if (real_family_count == nullptr || real_family == nullptr || real_find_family == nullptr) {
    ghost_log("dwrite: the font collection vtable is not shaped as expected; skipping");
    return;
  }

  build_visible(collection);

  // An empty result would tell the browser that this machine has no fonts at all,
  // which breaks text rendering far more visibly than an unfiltered list. Refuse
  // to patch rather than ship a browser that cannot draw.
  if (visible_to_real.empty()) {
    ghost_log("dwrite: none of the %zu profile families exist here; leaving fonts alone",
              allow_list.size());
    return;
  }

  DWORD old = 0;
  if (!VirtualProtect(vtable, sizeof(void*) * 8, PAGE_READWRITE, &old)) {
    ghost_log("dwrite: VirtualProtect on the font collection vtable failed: %lu",
              GetLastError());
    return;
  }
  vtable[kSlotGetFontFamilyCount] = reinterpret_cast<void*>(&hook_GetFontFamilyCount);
  vtable[kSlotGetFontFamily] = reinterpret_cast<void*>(&hook_GetFontFamily);
  vtable[kSlotFindFamilyName] = reinterpret_cast<void*>(&hook_FindFamilyName);
  VirtualProtect(vtable, sizeof(void*) * 8, old, &old);

  collection_patched = true;
  ghost_log("dwrite: font collection filtered (%zu of %u families visible)",
            visible_to_real.size(), real_family_count(collection));
}

HRESULT STDMETHODCALLTYPE hook_GetSystemFontCollection(IDWriteFactory* self,
                                                       IDWriteFontCollection** collection,
                                                       BOOL check_for_updates) {
  const HRESULT hr = real_get_collection(self, collection, check_for_updates);
  if (SUCCEEDED(hr) && collection != nullptr && *collection != nullptr) {
    patch_collection(*collection);
  }
  return hr;
}

HRESULT WINAPI hook_DWriteCreateFactory(DWRITE_FACTORY_TYPE type, REFIID iid,
                                        IUnknown** factory) {
  const HRESULT hr = real_create_factory(type, iid, factory);
  if (FAILED(hr) || factory == nullptr || *factory == nullptr) return hr;
  if (!IsEqualIID(iid, __uuidof(IDWriteFactory))) return hr;
  if (factory_patched) return hr;

  void** vtable = *reinterpret_cast<void***>(*factory);
  real_get_collection =
      reinterpret_cast<GetSystemFontCollectionFn>(vtable[kSlotGetSystemFontCollection]);
  if (real_get_collection == nullptr) return hr;

  DWORD old = 0;
  if (!VirtualProtect(vtable, sizeof(void*) * 8, PAGE_READWRITE, &old)) {
    ghost_log("dwrite: VirtualProtect on the factory vtable failed: %lu", GetLastError());
    return hr;
  }
  vtable[kSlotGetSystemFontCollection] =
      reinterpret_cast<void*>(&hook_GetSystemFontCollection);
  VirtualProtect(vtable, sizeof(void*) * 8, old, &old);

  factory_patched = true;
  ghost_log("dwrite: GetSystemFontCollection hooked");
  return hr;
}

}  // namespace

void install_dwrite_hooks() {
  const Profile& profile = Profile::current();
  if (!profile.fonts.has_families) return;

  for (const std::string& name : profile.fonts.families) {
    allow_list.push_back(lowered(to_wide(name)));
  }
  if (allow_list.empty()) return;

  // install_hook_export resolves the export and loads dwrite.dll if it is not
  // resident yet. Chromium loads it in every process that lays out text, so this
  // adds nothing an observer could notice.
  const bool ok = install_hook_export("dwrite.dll", "DWriteCreateFactory",
                                      reinterpret_cast<void*>(hook_DWriteCreateFactory),
                                      reinterpret_cast<void**>(&real_create_factory));
  ghost_log("dwrite: font filtering %s (%zu families in the profile)",
            ok ? "armed" : "UNAVAILABLE", allow_list.size());
}

}  // namespace ghost
