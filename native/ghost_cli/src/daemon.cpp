#include "daemon.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "capture.h"
#include "chrome.h"
#include "embed.h"
#include "input.h"
#include "json.h"
#include "launch.h"
#include "pipe.h"
#include "uia.h"
#include "window.h"

namespace ghost {
namespace {

struct Session {
  DWORD pid = 0;
  HWND window = nullptr;
  std::string pipe_name;
  std::string profile_id;
  std::string data_dir;
  std::vector<Element> last_tree;
  bool verbose = false;
};

Json failure(const std::string& message) {
  Json out = Json::object();
  out.set("ok", Json::boolean(false));
  out.set("error", Json::string(message));
  return out;
}

Json success() {
  Json out = Json::object();
  out.set("ok", Json::boolean(true));
  return out;
}

bool has(const Json& request, const char* key) { return request.find(key) != nullptr; }

std::string arg_string(const Json& request, const char* key) {
  const Json* value = request.find(key);
  return value == nullptr ? std::string() : value->as_string();
}

double arg_number(const Json& request, const char* key, double fallback) {
  const Json* value = request.find(key);
  return value == nullptr ? fallback : value->as_number(fallback);
}

Json element_json(const Element& element) {
  Json out = Json::object();
  out.set("index", Json::integer(element.index));
  out.set("depth", Json::integer(element.depth));
  out.set("role", Json::string(element.role));
  out.set("name", Json::string(element.name));
  if (!element.value.empty()) out.set("value", Json::string(element.value));
  if (!element.automation_id.empty()) out.set("id", Json::string(element.automation_id));
  out.set("enabled", Json::boolean(element.enabled));
  out.set("offscreen", Json::boolean(element.offscreen));
  out.set("focused", Json::boolean(element.focused));

  Json bounds = Json::object();
  bounds.set("x", Json::integer(element.bounds.left));
  bounds.set("y", Json::integer(element.bounds.top));
  bounds.set("width", Json::integer(element.bounds.right - element.bounds.left));
  bounds.set("height", Json::integer(element.bounds.bottom - element.bounds.top));
  out.set("bounds", bounds);

  int centre_x = 0;
  int centre_y = 0;
  if (element_center(element, &centre_x, &centre_y)) {
    out.set("center_x", Json::integer(centre_x));
    out.set("center_y", Json::integer(centre_y));
  }
  return out;
}

// A browser re-creates its window on some navigations, so the handle is
// re-resolved whenever it has gone stale rather than cached forever.
bool refresh(Session& session, std::string* error) {
  if (session.window != nullptr && IsWindow(session.window)) return true;
  session.window = nullptr;
  if (session.pid == 0) {
    *error = "no browser is attached";
    return false;
  }
  // The window does not exist the instant the browser starts, and Chrome can
  // take a moment to show it. Waiting here rather than failing immediately means
  // a client can send its first command right after `start()` without having to
  // guess how long the browser needs — the race is the control plane's problem,
  // not the caller's.
  const ULONGLONG deadline = GetTickCount64() + 30000;
  for (;;) {
    const WindowInfo info = main_window(session.pid);
    if (info.handle != nullptr) {
      session.window = info.handle;
      // Some navigations replace the window, and a replacement starts with its
      // accessibility tree switched off again, so prime whenever the handle is new
      // rather than only once at startup.
      prime_accessibility(session.window, 3000);
      return true;
    }
    if (GetTickCount64() >= deadline) break;
    Sleep(200);
  }
  *error = "the browser has no visible window yet";
  return false;
}

// Three ways to say where: absolute screen pixels (what `tree` reports), page
// CSS pixels, or an index from the last tree.
bool resolve_point(const Json& request, Session& session, int* x, int* y,
                   std::string* error) {
  if (has(request, "x") && has(request, "y")) {
    *x = static_cast<int>(std::lround(arg_number(request, "x", 0)));
    *y = static_cast<int>(std::lround(arg_number(request, "y", 0)));
    return true;
  }

  if (has(request, "css_x") && has(request, "css_y")) {
    if (!refresh(session, error)) return false;
    const WindowInfo info = describe_window(session.window);
    const double scale = window_scale(session.window);
    *x = info.client.left + static_cast<int>(std::lround(arg_number(request, "css_x", 0) * scale));
    *y = info.client.top + static_cast<int>(std::lround(arg_number(request, "css_y", 0) * scale));
    return true;
  }

  if (has(request, "index")) {
    const int index = static_cast<int>(arg_number(request, "index", -1));
    for (const Element& element : session.last_tree) {
      if (element.index != index) continue;
      if (!element_center(element, x, y)) {
        *error = "that element has no visible area";
        return false;
      }
      return true;
    }
    *error = "no element with index " + std::to_string(index) + " in the last tree";
    return false;
  }

  const std::string role = arg_string(request, "role");
  const std::string name = arg_string(request, "name");
  if (!role.empty() || !name.empty()) {
    if (!refresh(session, error)) return false;
    std::string find_error;
    const std::vector<Element> found =
        find_elements(session.window, role, name, 12, 2000, &find_error);
    if (found.empty()) {
      *error = "nothing matched role=" + role + " name contains \"" + name + "\"";
      return false;
    }
    if (!element_center(found.front(), x, y)) {
      *error = "the match has no visible area";
      return false;
    }
    return true;
  }

  *error = "give x/y (screen), css_x/css_y (page), index (from tree), or role/name";
  return false;
}

// --- commands ----------------------------------------------------------------

Json cmd_status(Session& session) {
  Json out = success();
  out.set("pid", Json::integer(session.pid));
  out.set("pipe", Json::string(session.pipe_name));
  out.set("profile", Json::string(session.profile_id));
  out.set("data_dir", Json::string(session.data_dir));
  out.set("attached", Json::boolean(session.pid != 0));

  std::string error;
  if (refresh(session, &error)) {
    const WindowInfo info = describe_window(session.window);
    Json window = Json::object();
    window.set("title", Json::string(info.title));
    window.set("class", Json::string(info.class_name));
    window.set("width", Json::integer(info.client_width));
    window.set("height", Json::integer(info.client_height));
    window.set("scale", Json::number(window_scale(session.window)));
    window.set("focused", Json::boolean(GetForegroundWindow() == session.window));
    out.set("window", window);
  } else {
    out.set("window", Json());
    out.set("window_error", Json::string(error));
  }
  out.set("tree_nodes", Json::integer(static_cast<long long>(session.last_tree.size())));
  return out;
}

Json cmd_windows(Session& session) {
  if (session.pid == 0) return failure("no browser is attached");
  Json out = success();
  Json list = Json::array();
  for (const WindowInfo& info : windows_for_pid(session.pid)) {
    Json item = Json::object();
    item.set("handle", Json::integer(static_cast<long long>(reinterpret_cast<uintptr_t>(info.handle))));
    item.set("title", Json::string(info.title));
    item.set("class", Json::string(info.class_name));
    item.set("visible", Json::boolean(info.visible));
    item.set("minimized", Json::boolean(info.minimized));
    item.set("width", Json::integer(info.client_width));
    item.set("height", Json::integer(info.client_height));
    list.push(item);
  }
  out.set("windows", list);
  return out;
}

Json cmd_focus(Session& session) {
  std::string error;
  if (!refresh(session, &error)) return failure(error);
  if (!activate_window(session.window, &error)) return failure(error);
  return success();
}

Json cmd_navigate(Session& session, const Json& request) {
  std::string error;
  if (!refresh(session, &error)) return failure(error);
  const std::string url = arg_string(request, "url");
  if (url.empty()) return failure("navigate needs a url");
  if (!activate_window(session.window, &error)) return failure(error);
  Sleep(150);

  wchar_t before[512] = {0};
  GetWindowTextW(session.window, before, 511);

  // Ctrl+L, type, Enter — the same thing a person does. There is no other way to
  // navigate without a protocol, and this one cannot be distinguished from a
  // user doing it.
  if (!key_combo({"ctrl", "l"}, &error)) return failure(error);
  Sleep(200);
  type_text(url);
  Sleep(150);
  if (!key_combo({"enter"}, &error)) return failure(error);

  // The window title is the only completion signal available from outside. It
  // changes when the new document commits.
  const DWORD deadline = GetTickCount() + 20000;
  bool changed = false;
  while (GetTickCount() < deadline) {
    Sleep(150);
    wchar_t now[512] = {0};
    GetWindowTextW(session.window, now, 511);
    if (now[0] != L'\0' && std::wstring(now) != std::wstring(before)) {
      changed = true;
      break;
    }
  }

  Json out = success();
  out.set("navigated", Json::boolean(changed));
  wchar_t title[512] = {0};
  GetWindowTextW(session.window, title, 511);
  out.set("title", Json::string(narrow(title)));
  return out;
}

Json cmd_tree(Session& session, const Json& request) {
  std::string error;
  if (!refresh(session, &error)) return failure(error);
  const int max_depth = static_cast<int>(arg_number(request, "max_depth", 12));
  const int max_nodes = static_cast<int>(arg_number(request, "max_nodes", 600));
  const bool keep_anonymous = has(request, "anonymous") ? request.find("anonymous")->as_bool()
                                                        : false;
  std::vector<Element> elements =
      dump_tree(session.window, max_depth, max_nodes, keep_anonymous, &error);
  if (!error.empty()) return failure(error);
  session.last_tree = elements;

  Json out = success();
  Json list = Json::array();
  for (const Element& element : elements) list.push(element_json(element));
  out.set("nodes", list);
  out.set("count", Json::integer(static_cast<long long>(elements.size())));
  return out;
}

Json cmd_find(Session& session, const Json& request) {
  std::string error;
  if (!refresh(session, &error)) return failure(error);
  const std::string role = arg_string(request, "role");
  const std::string name = arg_string(request, "name");
  if (role.empty() && name.empty()) return failure("find needs a role, a name, or both");
  const int max_depth = static_cast<int>(arg_number(request, "max_depth", 12));
  const int max_nodes = static_cast<int>(arg_number(request, "max_nodes", 2000));

  std::vector<Element> found = find_elements(session.window, role, name, max_depth, max_nodes, &error);
  if (!error.empty()) return failure(error);
  session.last_tree = found;

  Json out = success();
  Json list = Json::array();
  for (const Element& element : found) list.push(element_json(element));
  out.set("nodes", list);
  out.set("count", Json::integer(static_cast<long long>(found.size())));
  return out;
}

Json cmd_click(Session& session, const Json& request) {
  std::string error;
  if (!refresh(session, &error)) return failure(error);
  if (!activate_window(session.window, &error)) return failure(error);

  int x = 0;
  int y = 0;
  if (!resolve_point(request, session, &x, &y, &error)) return failure(error);

  const std::string button = arg_string(request, "button");
  const int count = static_cast<int>(arg_number(request, "count", 1));
  click_at(x, y, button.empty() ? "left" : button, count);

  Json out = success();
  out.set("x", Json::integer(x));
  out.set("y", Json::integer(y));
  return out;
}

Json cmd_type(Session& session, const Json& request) {
  std::string error;
  if (!refresh(session, &error)) return failure(error);
  if (!activate_window(session.window, &error)) return failure(error);
  const std::string text = arg_string(request, "text");
  if (text.empty()) return failure("type needs text");
  type_text(text);
  Json out = success();
  out.set("typed", Json::integer(static_cast<long long>(text.size())));
  return out;
}

Json cmd_key(Session& session, const Json& request) {
  std::string error;
  if (!refresh(session, &error)) return failure(error);
  if (!activate_window(session.window, &error)) return failure(error);

  const Json* keys = request.find("keys");
  std::vector<std::string> names;
  if (keys != nullptr && keys->is_array()) {
    for (const Json& item : keys->items()) names.push_back(item.as_string());
  } else {
    const std::string single = arg_string(request, "key");
    if (!single.empty()) names.push_back(single);
  }
  if (names.empty()) return failure("key needs a key name or a keys array");
  if (!key_combo(names, &error)) return failure(error);
  return success();
}

Json cmd_scroll(Session& session, const Json& request) {
  std::string error;
  if (!refresh(session, &error)) return failure(error);
  if (!activate_window(session.window, &error)) return failure(error);

  int x = 0;
  int y = 0;
  if (!resolve_point(request, session, &x, &y, &error)) {
    // Scrolling has a sensible default position when none was given: the middle
    // of the page.
    const WindowInfo info = describe_window(session.window);
    x = info.client.left + info.client_width / 2;
    y = info.client.top + info.client_height / 2;
  }
  const int delta = static_cast<int>(arg_number(request, "delta", -360));
  scroll_at(x, y, delta);

  Json out = success();
  out.set("delta", Json::integer(delta));
  return out;
}

Json cmd_screenshot(Session& session, const Json& request) {
  std::string error;
  if (!refresh(session, &error)) return failure(error);

  std::string path = arg_string(request, "path");
  if (path.empty()) {
    path = join_path(cache_root(), "screenshot.bmp");
  }
  if (!capture_window(session.window, path, &error)) return failure(error);

  Json out = success();
  out.set("path", Json::string(path));
  const WindowInfo info = describe_window(session.window);
  out.set("width", Json::integer(info.client_width));
  out.set("height", Json::integer(info.client_height));
  return out;
}

// There is deliberately no "call" command here. One existed until 0.5.0 and it
// forwarded the incoming request back down the same pipe, so {"cmd":"call"}
// re-entered itself and hung until the client's timeout. A nested request has no
// meaning anyway: this process *is* the server, and a client that wants to relay
// should open its own connection. `ghost call` is the CLI-side one-shot client,
// which is a different thing and is where that name belongs.

Json dispatch(Session& session, const Json& request, bool* stop) {
  const std::string command = arg_string(request, "cmd");
  if (command.empty()) return failure("every request needs a \"cmd\"");

  if (session.verbose) std::printf("serve: %s\n", request.dump().c_str());

  if (command == "status") return cmd_status(session);
  if (command == "windows") return cmd_windows(session);
  if (command == "focus") return cmd_focus(session);
  if (command == "navigate") return cmd_navigate(session, request);
  if (command == "tree") return cmd_tree(session, request);
  if (command == "find") return cmd_find(session, request);
  if (command == "click") return cmd_click(session, request);
  if (command == "type") return cmd_type(session, request);
  if (command == "key") return cmd_key(session, request);
  if (command == "scroll") return cmd_scroll(session, request);
  if (command == "screenshot") return cmd_screenshot(session, request);
  if (command == "shutdown") {
    *stop = true;
    return success();
  }
  return failure("unknown command: " + command);
}

// Waits for the browser's first window to appear after a launch.
DWORD wait_for_window(DWORD pid, DWORD timeout_ms) {
  const DWORD deadline = GetTickCount() + timeout_ms;
  while (GetTickCount() < deadline) {
    if (main_window(pid).handle != nullptr) return pid;
    Sleep(200);
  }
  return pid;
}

// Shuts down a browser this process started.
//
// This is not tidiness. Chromium takes an exclusive lock on its user-data-dir:
// leave the browser behind and the next run's browser finds the profile in use,
// exits immediately, and presents no window at all -- which surfaces as a
// confusing "the browser has no visible window yet" several layers away from the
// cause. A polite WM_CLOSE first, so the profile is flushed and cookies are
// written, then termination if it will not go.
void close_browser(DWORD pid) {
  const WindowInfo info = main_window(pid);
  if (info.handle != nullptr) {
    SendMessageTimeoutW(info.handle, WM_CLOSE, 0, 0, SMTO_ABORTIFHUNG, 2000, nullptr);
  }

  HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid);
  if (process == nullptr) return;
  if (WaitForSingleObject(process, 8000) == WAIT_TIMEOUT) {
    std::fprintf(stderr, "ghost: the browser did not close; terminating pid %lu\n", pid);
    TerminateProcess(process, 0);
    WaitForSingleObject(process, 5000);
  }
  CloseHandle(process);
}

}  // namespace

std::string default_pipe_name(const std::string& profile_id) {
  return "ghost-" + (profile_id.empty() ? std::string("default") : profile_id);
}

int run_serve(const ServeOptions& options) {
  Session session;
  session.pipe_name =
      options.pipe_name.empty() ? default_pipe_name(options.profile_id) : options.pipe_name;
  session.profile_id = options.profile_id;
  session.data_dir = options.data_dir;
  session.verbose = options.verbose;

  if (options.attach_pid != 0) {
    session.pid = options.attach_pid;
    std::string error;
    if (!refresh(session, &error)) {
      std::fprintf(stderr, "ghost: %s\n", error.c_str());
      return 1;
    }
    std::printf("attached to pid %lu\n", session.pid);
  } else {
    const BrowserInstall browser = find_browser(options.browser);
    if (browser.path.empty()) {
      std::fprintf(stderr,
                   "ghost: no Chrome or Edge found. Install one, or pass\n"
                   "       --browser <path to a Chromium executable>.\n");
      return 1;
    }
    const std::string shim = extract_resource(kResShimDll, "ghost_shim.dll");
    if (shim.empty()) {
      std::fprintf(stderr, "ghost: this executable has no embedded shim (bad build)\n");
      return 1;
    }

    LaunchOptions lo;
    lo.shim_dll = shim;
    lo.profile_json = options.profile_json;
    lo.timezone = options.timezone;
    lo.language = options.locale;
    lo.exe = browser.path;
    lo.wait = false;
    lo.verbose = options.verbose;
    lo.forward_stdio = false;
    lo.pid_out = &session.pid;

    make_dirs(options.data_dir);
    lo.args = {
        "--user-data-dir=" + options.data_dir,
        "--no-first-run",
        "--no-default-browser-check",
    };
    if (!options.sandbox) {
      lo.args.push_back("--disable-gpu-sandbox");
      lo.args.push_back("--no-sandbox");
      // Suppresses the "unsupported command-line flag" infobar, which is both a
      // visible automation tell and a 56 px bite out of the viewport.
      lo.args.push_back("--test-type");
    }
    for (const std::string& arg : options.chrome_args) lo.args.push_back(arg);
    for (const std::string& url : options.urls) lo.args.push_back(url);

    const int launch_code = launch_under_shim(lo);
    if (launch_code != 0) {
      std::fprintf(stderr, "ghost: the browser did not start (code %d)\n", launch_code);
      return launch_code;
    }
    wait_for_window(session.pid, 20000);
  }

  // Switch the renderer's accessibility tree on before any client gets to ask, so
  // the first `tree` or `find` sees the document rather than a tree made entirely
  // of browser chrome. Doing it here rather than on demand keeps the cost off the
  // first request and needs no extra command-line flag.
  {
    std::string prime_error;
    if (refresh(session, &prime_error) && session.window != nullptr) {
      const bool ready = prime_accessibility(session.window, 8000);
      std::printf("ghost serve: accessibility %s\n", ready ? "ready" : "not ready");
      std::fflush(stdout);
    }
  }

  std::printf("ghost serve: pid %lu, pipe \\\\.\\pipe\\%s\n", session.pid,
              session.pipe_name.c_str());
  std::printf("profile: %s\n", session.profile_id.c_str());
  std::fflush(stdout);

  const PipeHandler handler = [&session](const std::string& request, bool* stop) -> std::string {
    std::string parse_error;
    const Json parsed = Json::parse(request, &parse_error);
    if (!parse_error.empty()) return failure("invalid JSON: " + parse_error).dump();
    return dispatch(session, parsed, stop).dump();
  };

  const int code = serve_pipe(session.pipe_name, handler);
  std::printf("ghost serve: stopped\n");
  std::fflush(stdout);

  // The browser belongs to this control plane when we started it, so it goes
  // down with it. An --attach session owns nothing and is left running.
  if (options.attach_pid == 0 && session.pid != 0) close_browser(session.pid);

  return code;
}

}  // namespace ghost
