// Synthesized input, at the OS level.
//
// The point of not using CDP is that the page must see input it cannot
// distinguish from a person's: `isTrusted` true, hardware timestamps, a pointer
// that moves along a path instead of teleporting, and real key events. SendInput
// gives all of that because the input goes through the same kernel path as a
// physical device.
#pragma once

#include <string>
#include <vector>

namespace ghost {

// Move the pointer to an absolute screen position along a jittered path. A
// single jump would produce a `mousemove` with no preceding positions, which is
// its own fingerprint.
void move_to(int screen_x, int screen_y);

// Press and release a button at a position, moving there first.
void click_at(int screen_x, int screen_y, const std::string& button, int count);

void scroll_at(int screen_x, int screen_y, int delta);

// Type text as Unicode key events, one code unit at a time. This bypasses the
// keyboard layout entirely, so non-ASCII text works without switching layouts.
void type_text(const std::string& utf8);

// Press keys together, e.g. {"ctrl","l"}. Returns false with a reason when a
// name is unknown, rather than silently typing nothing.
bool key_combo(const std::vector<std::string>& keys, std::string* error);

}  // namespace ghost
