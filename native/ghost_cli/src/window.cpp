#include "window.h"

#include <algorithm>

#include "embed.h"

namespace ghost {
namespace {

BOOL CALLBACK collect(HWND handle, LPARAM param) {
  auto* out = reinterpret_cast<std::vector<WindowInfo>*>(param);
  DWORD pid = 0;
  GetWindowThreadProcessId(handle, &pid);
  WindowInfo info = describe_window(handle);
  info.pid = pid;
  out->push_back(info);
  return TRUE;
}

}  // namespace

WindowInfo describe_window(HWND handle) {
  WindowInfo info;
  if (handle == nullptr || !IsWindow(handle)) return info;

  info.handle = handle;
  GetWindowThreadProcessId(handle, &info.pid);
  info.visible = IsWindowVisible(handle) != FALSE;
  info.minimized = IsIconic(handle) != FALSE;

  wchar_t buffer[512] = {0};
  if (GetWindowTextW(handle, buffer, 511) > 0) info.title = narrow(buffer);
  wchar_t klass[256] = {0};
  if (GetClassNameW(handle, klass, 255) > 0) info.class_name = narrow(klass);

  if (GetWindowRect(handle, &info.frame)) {
    RECT client{0, 0, 0, 0};
    if (GetClientRect(handle, &client)) {
      POINT top_left{client.left, client.top};
      POINT bottom_right{client.right, client.bottom};
      ClientToScreen(handle, &top_left);
      ClientToScreen(handle, &bottom_right);
      info.client = {top_left.x, top_left.y, bottom_right.x, bottom_right.y};
      info.client_width = bottom_right.x - top_left.x;
      info.client_height = bottom_right.y - top_left.y;
    }
  }
  return info;
}

std::vector<WindowInfo> windows_for_pid(DWORD pid) {
  std::vector<WindowInfo> all;
  EnumWindows(collect, reinterpret_cast<LPARAM>(&all));

  std::vector<WindowInfo> mine;
  for (WindowInfo& info : all) {
    if (info.pid == pid) mine.push_back(info);
  }
  return mine;
}

WindowInfo main_window(DWORD pid) {
  std::vector<WindowInfo> candidates;
  for (const WindowInfo& info : windows_for_pid(pid)) {
    // A browser owns several invisible helper windows. The page is the one with
    // a title and a real client area.
    if (!info.visible || info.minimized) continue;
    if (info.client_width <= 0 || info.client_height <= 0) continue;
    if (info.title.empty()) continue;
    candidates.push_back(info);
  }
  if (candidates.empty()) return WindowInfo{};

  std::sort(candidates.begin(), candidates.end(),
            [](const WindowInfo& a, const WindowInfo& b) {
              return static_cast<long long>(a.client_width) * a.client_height >
                     static_cast<long long>(b.client_width) * b.client_height;
            });
  return candidates.front();
}

bool activate_window(HWND handle, std::string* error) {
  if (handle == nullptr || !IsWindow(handle)) {
    *error = "no such window";
    return false;
  }
  if (IsIconic(handle)) {
    ShowWindow(handle, SW_RESTORE);
    Sleep(150);
  }
  if (GetForegroundWindow() == handle) return true;

  // Windows discards synthesized input while the session is disconnected: there
  // is no foreground window for SendInput to deliver to, and it is a silent
  // no-op rather than an error. Every later command would then time out with no
  // hint of the cause, so say what is actually wrong instead.
  if (GetForegroundWindow() == nullptr) {
    if (error != nullptr) {
      *error =
          "this session has no foreground window, so Windows discards synthesized "
          "input (a disconnected or headless session); connect the session and retry";
    }
    return false;
  }

  const DWORD target_thread = GetWindowThreadProcessId(handle, nullptr);
  const DWORD our_thread = GetCurrentThreadId();
  const HWND foreground = GetForegroundWindow();
  const DWORD foreground_thread =
      foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;

  // Windows refuses SetForegroundWindow to a process the user is not
  // interacting with, so several documented conditions have to hold at once:
  // sharing the target's input queue, and being the thread that last saw input.
  // Attaching to *both* the target and whatever is currently in front is what
  // makes the attach effective when another window owns the foreground; a
  // synthetic ALT press satisfies the last-input condition.
  const auto attach = [&](DWORD thread, BOOL on) {
    if (thread != 0 && thread != our_thread) AttachThreadInput(our_thread, thread, on);
  };
  attach(target_thread, TRUE);
  attach(foreground_thread, TRUE);

  // The topmost toggle is the reliable part: raising and immediately lowering a
  // window brings it to the foreground in cases where SetForegroundWindow alone
  // is silently ignored. Retrying the whole sequence matters because the
  // foreground lock can be held by a window that is itself still settling.
  bool activated = false;
  for (int round = 0; round < 4 && !activated; round++) {
    INPUT alt[2] = {};
    alt[0].type = INPUT_KEYBOARD;
    alt[0].ki.wVk = VK_MENU;
    alt[1].type = INPUT_KEYBOARD;
    alt[1].ki.wVk = VK_MENU;
    alt[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, alt, sizeof(INPUT));

    ShowWindow(handle, SW_SHOW);
    BringWindowToTop(handle);
    SetWindowPos(handle, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetWindowPos(handle, HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(handle);
    SetActiveWindow(handle);
    SetFocus(handle);

    for (int attempt = 0; attempt < 20 && !activated; attempt++) {
      if (GetForegroundWindow() == handle) activated = true;
      else Sleep(25);
    }
  }

  // SwitchToThisWindow is undocumented but has been stable for decades and
  // ignores the foreground lock entirely. It is a last resort because it can
  // steal focus from whatever the user is doing.
  if (!activated) {
    using SwitchToThisWindowFn = void(__stdcall*)(HWND, BOOL);
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
      auto switch_to = reinterpret_cast<SwitchToThisWindowFn>(
          reinterpret_cast<void*>(GetProcAddress(user32, "SwitchToThisWindow")));
      if (switch_to != nullptr) {
        switch_to(handle, TRUE);
        for (int attempt = 0; attempt < 20 && !activated; attempt++) {
          if (GetForegroundWindow() == handle) activated = true;
          else Sleep(25);
        }
      }
    }
  }

  attach(foreground_thread, FALSE);
  attach(target_thread, FALSE);

  if (!activated) {
    *error = "the window could not be brought to the foreground";
    return false;
  }
  return true;
}

double window_scale(HWND handle) {
  if (handle == nullptr) return 1.0;
  const UINT dpi = GetDpiForWindow(handle);
  if (dpi == 0) return 1.0;
  return static_cast<double>(dpi) / 96.0;
}

}  // namespace ghost
