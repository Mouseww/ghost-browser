// Reading the page through the accessibility tree.
//
// This replaces the DOM inspection a debugging protocol would give us. Windows
// exposes every Chromium renderer's accessibility tree through UI Automation,
// the page has no way to tell that it is being read, and nothing about it
// changes the JavaScript environment. It is slower than CDP and gives roles and
// names rather than selectors, which is exactly the trade being made.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace ghost {

struct Element {
  int index = 0;  // position in the flattened walk, for stable addressing
  int depth = 0;
  std::string role;           // "button", "edit", "link", ...
  std::string name;           // accessible name: usually the visible label
  std::string value;          // current text of an edit, when there is one
  std::string automation_id;  // Chromium maps this to the element id
  bool enabled = false;
  bool offscreen = false;
  bool focused = false;
  RECT bounds{};  // physical screen pixels
};

// Depth-first walk of the window's accessibility tree. Elements with neither a
// name nor a value are still returned when `keep_anonymous` is set; without it
// the result is limited to things a caller could actually address.
std::vector<Element> dump_tree(HWND window, int max_depth, int max_nodes,
                               bool keep_anonymous, std::string* error);

// Substring match on the accessible name, optionally restricted to one role.
// `role` empty means any role.
std::vector<Element> find_elements(HWND window, const std::string& role,
                                   const std::string& name_contains, int max_depth,
                                   int max_nodes, std::string* error);

// Centre of an element's bounds, in physical screen pixels — the coordinates
// SendInput wants.
bool element_center(const Element& element, int* x, int* y);

// Waits for the page's document to appear in the accessibility tree, and returns
// true if it did.
//
// Chromium builds that tree lazily: the first UI Automation query is what switches
// accessibility on, and the document shows up in a later query. A client that asks
// exactly once, right after the browser starts, therefore sees a tree made only of
// browser chrome — no headings, no links, no text — and has every reason to
// conclude the page is empty. Priming once at startup makes the first real query
// correct, and unlike --force-renderer-accessibility it leaves no extra flag on the
// command line.
//
// Returns false on timeout, which is not fatal: a browser still on about:blank has
// no document to find, and the caller should carry on regardless.
bool prime_accessibility(HWND window, int timeout_ms);

}  // namespace ghost
