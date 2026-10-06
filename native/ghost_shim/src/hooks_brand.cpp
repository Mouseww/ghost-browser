// hooks_brand.cpp — make the browser present itself as this project.
//
// Track A cannot change the engine's compiled-in product name, but the name a
// user actually sees is decided at runtime, in the browser process, by calls
// this shim can intercept:
//
//   * the window title, which Chromium composes as "<page title> - <product>"
//     and then pushes through SetWindowText;
//   * the window icon, which is set with WM_SETICON and inherited by later
//     windows through the window class;
//   * the AppUserModelID, which is what the taskbar groups by.
//
// The limits are real and are documented rather than hidden: chrome://version,
// the on-disk executable name and the window class name still say Chrome. Those
// live in the binary, not in a runtime call.
#include <windows.h>

#include <string>
#include <vector>

#include "hook_engine.h"

namespace ghost {
namespace {

using SetWindowTextW_t = BOOL(WINAPI*)(HWND, LPCWSTR);
using SetWindowTextA_t = BOOL(WINAPI*)(HWND, LPCSTR);

SetWindowTextW_t real_set_window_text_w = nullptr;
SetWindowTextA_t real_set_window_text_a = nullptr;

// The engine names that can end up in a title. Matched against the *last*
// occurrence so a page titled "Chromium release notes" keeps its own words.
const wchar_t* kEngineNames[] = {
    L"Google Chrome",
    L"Microsoft Edge",
    L"Chromium",
};

std::wstring brand_name() {
  wchar_t buffer[128] = {0};
  const DWORD length = GetEnvironmentVariableW(L"GHOST_BRAND_NAME", buffer, 128);
  if (length > 0 && length < 128) return std::wstring(buffer, length);
  return L"Ghost Browser";
}

bool is_engine_name(const std::wstring& text) {
  for (const wchar_t* name : kEngineNames) {
    if (text == name) return true;
  }
  return false;
}

std::wstring rewrite_title(const std::wstring& title) {
  const std::wstring brand = brand_name();
  for (const wchar_t* name : kEngineNames) {
    const std::wstring engine(name);
    const size_t at = title.rfind(engine);
    if (at == std::wstring::npos) continue;
    if (brand == engine) return title;  // already ours
    return title.substr(0, at) + brand + title.substr(at + engine.size());
  }
  return title;
}

bool owned_by_this_process(HWND window) {
  DWORD pid = 0;
  GetWindowThreadProcessId(window, &pid);
  return pid == GetCurrentProcessId();
}

HICON load_brand_icon(int width, int height) {
  // The shim carries the same icon as ghost.exe, so the browser window can wear
  // it without depending on where the launcher was installed. LR_SHARED makes
  // the system cache and own the handle, so there is nothing to destroy and
  // repeated sweeps cannot leak.
  HMODULE self = static_cast<HMODULE>(shim_module());
  if (self == nullptr) return nullptr;
  return static_cast<HICON>(LoadImageW(self, MAKEINTRESOURCEW(1), IMAGE_ICON, width, height,
                                       LR_DEFAULTCOLOR | LR_SHARED));
}

void apply_icon(HWND window) {
  static HICON big = nullptr;
  static HICON small = nullptr;
  if (big == nullptr) {
    big = load_brand_icon(GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
  }
  if (small == nullptr) {
    small = load_brand_icon(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
  }

  if (big != nullptr) {
    // SendMessage rather than PostMessage: the class icon must be in place
    // before the taskbar reads it, and a hung window must not block us.
    SendMessageTimeoutW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big),
                        SMTO_ABORTIFHUNG, 1000, nullptr);
    SetClassLongPtrW(window, GCLP_HICON, reinterpret_cast<LONG_PTR>(big));
  }
  if (small != nullptr) {
    SendMessageTimeoutW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small),
                        SMTO_ABORTIFHUNG, 1000, nullptr);
    SetClassLongPtrW(window, GCLP_HICONSM, reinterpret_cast<LONG_PTR>(small));
  }
}

// Titles are the one surface that changes constantly, so they are handled both
// eagerly (in the hook) and lazily (by the sweep below), in case a title was set
// before the hook was live.
BOOL WINAPI hook_SetWindowTextW(HWND window, LPCWSTR text) {
  if (text != nullptr && owned_by_this_process(window)) {
    const std::wstring rewritten = rewrite_title(text);
    if (rewritten != text) {
      return real_set_window_text_w(window, rewritten.c_str());
    }
  }
  return real_set_window_text_w(window, text);
}

BOOL WINAPI hook_SetWindowTextA(HWND window, LPCSTR text) {
  if (text != nullptr && owned_by_this_process(window)) {
    const int length = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    if (length > 1) {
      std::wstring wide(static_cast<size_t>(length - 1), L'\0');
      MultiByteToWideChar(CP_ACP, 0, text, -1, wide.data(), length);
      const std::wstring rewritten = rewrite_title(wide);
      if (rewritten != wide) {
        const int narrow_length =
            WideCharToMultiByte(CP_ACP, 0, rewritten.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string narrow(static_cast<size_t>(narrow_length > 0 ? narrow_length - 1 : 0), '\0');
        if (narrow_length > 0) {
          WideCharToMultiByte(CP_ACP, 0, rewritten.c_str(), -1, narrow.data(), narrow_length,
                              nullptr, nullptr);
        }
        return real_set_window_text_a(window, narrow.c_str());
      }
    }
  }
  return real_set_window_text_a(window, text);
}

struct Sweep {
  HWND window;
};

BOOL CALLBACK sweep_window(HWND window, LPARAM param) {
  auto* state = reinterpret_cast<Sweep*>(param);
  if (!owned_by_this_process(window)) return TRUE;
  if (!IsWindowVisible(window)) return TRUE;

  wchar_t title[512] = {0};
  GetWindowTextW(window, title, 511);
  if (title[0] == L'\0') return TRUE;

  state->window = window;

  const std::wstring current(title);
  const std::wstring rewritten = rewrite_title(current);
  if (rewritten != current) {
    real_set_window_text_w(window, rewritten.c_str());
  }
  apply_icon(window);
  return TRUE;
}

DWORD WINAPI brand_thread(LPVOID) {
  // Chromium takes its own AppUserModelID early. Setting ours makes the taskbar
  // group and label the window with the project name.
  if (HMODULE shell = LoadLibraryW(L"shell32.dll")) {
    using SetAumid_t = HRESULT(WINAPI*)(PCWSTR);
    auto set_aumid =
        reinterpret_cast<SetAumid_t>(GetProcAddress(shell, "SetCurrentProcessExplicitAppUserModelID"));
    if (set_aumid != nullptr) {
      const std::wstring aumid = L"unknowbrowser." + brand_name();
      set_aumid(aumid.c_str());
      ghost_log("brand: app user model id = %ls", aumid.c_str());
    }
  }

  // The window does not exist yet when the shim attaches, and a title may be set
  // before the hook is live. Sweeping for a bounded time covers both without
  // leaving a permanent timer in every renderer process. The bound is wall clock
  // rather than a loop count, because the sleep changes once the window appears:
  // a count would silently mean "eight minutes" after the first hit.
  const ULONGLONG deadline = GetTickCount64() + 90000;
  bool applied = false;
  while (GetTickCount64() < deadline) {
    Sweep state{nullptr};
    EnumWindows(sweep_window, reinterpret_cast<LPARAM>(&state));
    if (state.window != nullptr && !applied) {
      applied = true;
      ghost_log("brand: applied '%ls' + icon to hwnd=%p", brand_name().c_str(), state.window);
    }
    // Once the class icon is set, every later window of that class inherits it,
    // so the sweep only has to catch the first window and can then back off.
    Sleep(applied ? 2000 : 250);
  }
  return 0;
}

}  // namespace

void install_brand_hooks() {
  install_hook_export("user32.dll", "SetWindowTextW",
                      reinterpret_cast<void*>(&hook_SetWindowTextW),
                      reinterpret_cast<void**>(&real_set_window_text_w));
  install_hook_export("user32.dll", "SetWindowTextA",
                      reinterpret_cast<void*>(&hook_SetWindowTextA),
                      reinterpret_cast<void**>(&real_set_window_text_a));

  HANDLE thread = CreateThread(nullptr, 0, brand_thread, nullptr, 0, nullptr);
  if (thread != nullptr) CloseHandle(thread);
}

}  // namespace ghost
