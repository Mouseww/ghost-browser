// Finding and describing the browser's top-level window.
//
// Everything the control plane does to a page goes through the OS: synthesized
// input needs a foreground window, and a screenshot needs a window handle. None
// of it touches the browser from the inside, which is the whole point.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace ghost {

struct WindowInfo {
  HWND handle = nullptr;
  DWORD pid = 0;
  std::string title;
  std::string class_name;
  bool visible = false;
  bool minimized = false;
  RECT frame{};   // screen coordinates, the whole window
  RECT client{};  // screen coordinates, the client (page) area
  int client_width = 0;
  int client_height = 0;
};

WindowInfo describe_window(HWND handle);

// Every top-level window owned by `pid`, in Z-order. Browsers own several
// hidden helper windows, so this is a starting point, not an answer.
std::vector<WindowInfo> windows_for_pid(DWORD pid);

// The visible, non-minimized top-level window with the largest client area.
// Returns a zeroed WindowInfo when the process has no such window.
WindowInfo main_window(DWORD pid);

// Brings a window to the foreground so synthesized input lands in it. Chromium
// ignores keystrokes sent to a window it does not consider active.
bool activate_window(HWND handle, std::string* error);

// Maximizes the window (no-op when already maximized) so a challenge dialog
// taller or wider than the client area still fits on screen.
bool maximize_window(HWND handle, std::string* error);

// Pixels per CSS pixel for this window (96 dpi == 1.0). UIA rectangles are in
// physical screen pixels, so this is what converts them to page coordinates.
double window_scale(HWND handle);

}  // namespace ghost
