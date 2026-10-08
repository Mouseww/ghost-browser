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

// The traversal the control plane uses for every tree it hands out. An element
// index is a position in this walk, so anything that reads an index and anything
// that acts on one have to agree on it -- hence one definition, not two.
constexpr int kTreeDepth = 30;
constexpr int kTreeNodes = 4000;

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

// Every descendant, asked for in one query rather than walked. Same contents as
// dump_tree when the control view is behaving, which is what makes it useful
// when it is not: it answers "is the element there at all" without a walker.
std::vector<Element> dump_descendants(HWND window, int max_nodes, std::string* error);

// Holds a UI Automation client open for `seconds` and reports what the platform
// thinks of that. Chromium decides whether to build the renderer's tree from a
// flag that only a *live* client sets, so the question "is anyone listening"
// cannot be asked from inside a client that is about to exit -- it has to be
// asked while one is held open.
bool hold_accessibility_client(int seconds, bool* clients_listening);

// Centre of an element's bounds, in physical screen pixels — the coordinates
// SendInput wants.
bool element_center(const Element& element, int* x, int* y);

// Asks the control to scroll itself into view through UI Automation's
// ScrollItemPattern. For controls that live inside a scrollable region (the
// common case: a page taller than the window), this is exactly what a person's
// wheel input does, and it reaches the page as a programmatic scroll, not as
// synthesized input.
//
// Returns false when the control does not advertise the pattern -- a control
// already fully visible has nothing to scroll, and top-level chrome never
// carries it -- which the caller treats as "no scroll needed or possible".
bool scroll_element_into_view(HWND window, int index, std::string* error);

// Maximizes the window so a low-resolution screen or a small restored window
// cannot clip a challenge dialog that is taller or wider than the client area.
// Safe to call repeatedly: an already-maximized window stays maximized.
bool maximize_window(HWND handle, std::string* error);

// Act on a control through UI Automation itself, without synthesizing input.
//
// This exists because synthesized input needs a foreground window, and a session
// that has none -- a disconnected RDP session, a service, a headless machine --
// has Windows silently drop every SendInput. The accessibility tree is still
// there in exactly those sessions, and the control can still be asked to do the
// one thing it advertises. Tries Invoke, then Toggle, then Select, then the
// legacy default action, and reports which of them was used.
//
// The trade is real and worth stating: an accessibility invocation reaches the
// page as a *synthetic* action, so `event.isTrusted` is false, where a real
// synthesized click produces a trusted one. Callers that care should prefer
// SendInput and fall back to this only when the session cannot deliver input.
bool invoke_element(HWND window, int index, std::string* error);

// Put text into an edit control through UI Automation's value pattern. Same
// trade as above: no keystrokes, no foreground window, but the page sees a
// programmatic value change rather than typing.
bool set_element_value(HWND window, int index, const std::string& text,
                       std::string* error);

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
