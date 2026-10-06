#include "input.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

namespace ghost {
namespace {

std::mt19937& rng() {
  static std::mt19937 engine{std::random_device{}()};
  return engine;
}

int jitter(int low, int high) {
  return std::uniform_int_distribution<int>(low, high)(rng());
}

void send_absolute_move(int screen_x, int screen_y) {
  const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
  const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
  const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

  INPUT in{};
  in.type = INPUT_MOUSE;
  // SendInput wants the desktop normalized to 0..65535; passing raw pixels
  // would land the pointer somewhere else entirely on a scaled display.
  in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
  in.mi.dx = static_cast<LONG>((screen_x - vx) * 65535.0 / (vw > 1 ? vw - 1 : 1));
  in.mi.dy = static_cast<LONG>((screen_y - vy) * 65535.0 / (vh > 1 ? vh - 1 : 1));
  SendInput(1, &in, sizeof(in));
}

void send_button(DWORD flag) {
  INPUT in{};
  in.type = INPUT_MOUSE;
  in.mi.dwFlags = flag;
  SendInput(1, &in, sizeof(in));
}

void send_key(WORD vk, bool up) {
  INPUT in{};
  in.type = INPUT_KEYBOARD;
  in.ki.wVk = vk;
  in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
  SendInput(1, &in, sizeof(in));
}

void send_unicode(wchar_t unit, bool up) {
  INPUT in{};
  in.type = INPUT_KEYBOARD;
  in.ki.wVk = 0;
  in.ki.wScan = unit;
  in.ki.dwFlags = KEYEVENTF_UNICODE | (up ? KEYEVENTF_KEYUP : 0);
  SendInput(1, &in, sizeof(in));
}

// Minimal UTF-8 decoder: enough for text a caller might type, including
// non-Latin scripts, without pulling in a conversion library.
std::vector<wchar_t> utf8_to_utf16(const std::string& text) {
  std::vector<wchar_t> out;
  size_t i = 0;
  while (i < text.size()) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    unsigned int code = 0;
    size_t extra = 0;
    if (c < 0x80) {
      code = c;
    } else if ((c & 0xE0) == 0xC0) {
      code = c & 0x1F;
      extra = 1;
    } else if ((c & 0xF0) == 0xE0) {
      code = c & 0x0F;
      extra = 2;
    } else if ((c & 0xF8) == 0xF0) {
      code = c & 0x07;
      extra = 3;
    } else {
      i++;
      continue;  // stray continuation byte
    }
    if (i + extra >= text.size()) break;
    bool ok = true;
    for (size_t k = 1; k <= extra; k++) {
      const unsigned char next = static_cast<unsigned char>(text[i + k]);
      if ((next & 0xC0) != 0x80) { ok = false; break; }
      code = (code << 6) | (next & 0x3F);
    }
    if (!ok) { i++; continue; }
    i += extra + 1;

    if (code <= 0xFFFF) {
      out.push_back(static_cast<wchar_t>(code));
    } else {
      code -= 0x10000;
      out.push_back(static_cast<wchar_t>(0xD800 + (code >> 10)));
      out.push_back(static_cast<wchar_t>(0xDC00 + (code & 0x3FF)));
    }
  }
  return out;
}

WORD virtual_key_for(const std::string& raw) {
  std::string name;
  for (const char c : raw) name.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

  if (name == "ctrl" || name == "control") return VK_CONTROL;
  if (name == "shift") return VK_SHIFT;
  if (name == "alt") return VK_MENU;
  if (name == "win" || name == "meta" || name == "super") return VK_LWIN;
  if (name == "enter" || name == "return") return VK_RETURN;
  if (name == "tab") return VK_TAB;
  if (name == "escape" || name == "esc") return VK_ESCAPE;
  if (name == "space") return VK_SPACE;
  if (name == "backspace") return VK_BACK;
  if (name == "delete" || name == "del") return VK_DELETE;
  if (name == "home") return VK_HOME;
  if (name == "end") return VK_END;
  if (name == "pageup") return VK_PRIOR;
  if (name == "pagedown") return VK_NEXT;
  if (name == "up") return VK_UP;
  if (name == "down") return VK_DOWN;
  if (name == "left") return VK_LEFT;
  if (name == "right") return VK_RIGHT;
  if (name.size() >= 2 && name[0] == 'f' && std::isdigit(static_cast<unsigned char>(name[1]))) {
    const int n = std::atoi(name.c_str() + 1);
    if (n >= 1 && n <= 24) return static_cast<WORD>(VK_F1 + n - 1);
  }
  if (name.size() == 1) {
    const short vk = VkKeyScanW(static_cast<wchar_t>(name[0]));
    if (vk != -1) return static_cast<WORD>(vk & 0xFF);
  }
  return 0;
}

bool is_modifier(WORD vk) {
  return vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU || vk == VK_LWIN;
}

DWORD button_flag(const std::string& button) {
  const std::string b = button.empty() ? "left" : button;
  if (b == "right") return MOUSEEVENTF_RIGHTDOWN;
  if (b == "middle") return MOUSEEVENTF_MIDDLEDOWN;
  return MOUSEEVENTF_LEFTDOWN;
}

}  // namespace

void move_to(int screen_x, int screen_y) {
  POINT current{};
  if (!GetCursorPos(&current)) {
    send_absolute_move(screen_x, screen_y);
    return;
  }
  const double dx = screen_x - current.x;
  const double dy = screen_y - current.y;
  const double distance = std::sqrt(dx * dx + dy * dy);
  if (distance < 2.0) {
    send_absolute_move(screen_x, screen_y);
    return;
  }

  // A hand does not teleport. More distance means more intermediate points, and
  // the path bows slightly instead of being a straight line.
  int steps = static_cast<int>(distance / 18.0) + 4;
  if (steps > 40) steps = 40;
  const double bow = std::uniform_real_distribution<double>(-1.0, 1.0)(rng()) *
                     std::min(24.0, distance * 0.08);
  const double nx = -dy / distance;
  const double ny = dx / distance;

  for (int i = 1; i <= steps; i++) {
    const double t = static_cast<double>(i) / steps;
    const double eased = t * t * (3.0 - 2.0 * t);
    const double offset = bow * std::sin(t * 3.14159265358979);
    send_absolute_move(static_cast<int>(std::lround(current.x + dx * eased + nx * offset)),
                       static_cast<int>(std::lround(current.y + dy * eased + ny * offset)));
    Sleep(static_cast<DWORD>(jitter(2, 9)));
  }
  send_absolute_move(screen_x, screen_y);
  Sleep(static_cast<DWORD>(jitter(8, 25)));
}

void click_at(int screen_x, int screen_y, const std::string& button, int count) {
  move_to(screen_x, screen_y);
  const DWORD down = button_flag(button);
  const DWORD up = down == MOUSEEVENTF_RIGHTDOWN    ? MOUSEEVENTF_RIGHTUP
                   : down == MOUSEEVENTF_MIDDLEDOWN ? MOUSEEVENTF_MIDDLEUP
                                                    : MOUSEEVENTF_LEFTUP;
  const int clicks = count < 1 ? 1 : count;
  for (int i = 0; i < clicks; i++) {
    send_button(down);
    Sleep(static_cast<DWORD>(jitter(35, 90)));
    send_button(up);
    if (i + 1 < clicks) Sleep(static_cast<DWORD>(jitter(40, 110)));
  }
}

void scroll_at(int screen_x, int screen_y, int delta) {
  move_to(screen_x, screen_y);

  // Wheel events arrive as many small steps with an easing tail, never as one
  // jump of the whole distance.
  const int total = delta;
  const int steps = std::max(1, std::abs(total) / 120);
  const int per_step = total / steps;
  for (int i = 0; i < steps; i++) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_WHEEL;
    in.mi.mouseData = static_cast<DWORD>(per_step);
    SendInput(1, &in, sizeof(in));
    Sleep(static_cast<DWORD>(jitter(12, 40)));
  }
}

void type_text(const std::string& utf8) {
  for (const wchar_t unit : utf8_to_utf16(utf8)) {
    send_unicode(unit, false);
    Sleep(static_cast<DWORD>(jitter(8, 30)));
    send_unicode(unit, true);
    Sleep(static_cast<DWORD>(jitter(15, 60)));
  }
}

bool key_combo(const std::vector<std::string>& keys, std::string* error) {
  if (keys.empty()) {
    *error = "no keys given";
    return false;
  }

  std::vector<WORD> codes;
  for (const std::string& name : keys) {
    const WORD vk = virtual_key_for(name);
    if (vk == 0) {
      *error = "unknown key name: " + name;
      return false;
    }
    codes.push_back(vk);
  }

  // Hold the modifiers, tap the last key, release in reverse. Chromium reads the
  // modifier state from the keyboard, so the order is not cosmetic.
  for (size_t i = 0; i + 1 < codes.size(); i++) {
    if (is_modifier(codes[i])) send_key(codes[i], false);
  }
  Sleep(static_cast<DWORD>(jitter(20, 50)));
  send_key(codes.back(), false);
  Sleep(static_cast<DWORD>(jitter(30, 70)));
  send_key(codes.back(), true);
  Sleep(static_cast<DWORD>(jitter(20, 50)));
  for (size_t i = codes.size() - 1; i-- > 0;) {
    if (is_modifier(codes[i])) send_key(codes[i], true);
  }
  return true;
}

}  // namespace ghost
