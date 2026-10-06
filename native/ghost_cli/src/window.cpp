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

  const DWORD target_thread = GetWindowThreadProcessId(handle, nullptr);
  const DWORD our_thread = GetCurrentThreadId();

  // Windows refuses SetForegroundWindow from a process the user is not
  // interacting with. Two documented conditions have to be satisfied at once:
  // sharing the target's input queue, and being the thread that last saw input.
  // A synthetic ALT press satisfies the second, which is why it is here.
  const bool attached =
      (target_thread != 0 && target_thread != our_thread) &&
      AttachThreadInput(our_thread, target_thread, TRUE) != FALSE;

  INPUT alt[2] = {};
  alt[0].type = INPUT_KEYBOARD;
  alt[0].ki.wVk = VK_MENU;
  alt[1].type = INPUT_KEYBOARD;
  alt[1].ki.wVk = VK_MENU;
  alt[1].ki.dwFlags = KEYEVENTF_KEYUP;
  SendInput(2, alt, sizeof(INPUT));

  ShowWindow(handle, SW_SHOW);
  BringWindowToTop(handle);
  SetWindowPos(handle, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
  SetForegroundWindow(handle);
  SetActiveWindow(handle);
  SetFocus(handle);
  if (attached) AttachThreadInput(our_thread, target_thread, FALSE);

  for (int attempt = 0; attempt < 60; attempt++) {
    if (GetForegroundWindow() == handle) return true;
    Sleep(25);
  }
  *error = "the window could not be brought to the foreground";
  return false;
}

double window_scale(HWND handle) {
  if (handle == nullptr) return 1.0;
  const UINT dpi = GetDpiForWindow(handle);
  if (dpi == 0) return 1.0;
  return static_cast<double>(dpi) / 96.0;
}

}  // namespace ghost
