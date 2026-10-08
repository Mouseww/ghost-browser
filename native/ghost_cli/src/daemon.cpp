#include "daemon.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "audio_capture.h"
#include "captcha.h"
#include "capture.h"
#include "cdp.h"
#include "chrome.h"
#include "embed.h"
#include "input.h"
#include "json.h"
#include "launch.h"
#include "pipe.h"
#include "solve_api.h"
#include "speech.h"
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
  // Which channel the last interaction used: "synthesized" for real OS input,
  // "accessibility" for a UI Automation invocation. They are not equivalent --
  // only the first reaches the page as a trusted event -- so every response that
  // clicked something says which one it was.
  std::string last_input;
  // The solving service's key, when the profile carries one. Kept on the session
  // because the tier that needs it is reached from a request, and a request has no
  // business carrying a secret the profile already states.
  std::string captcha_api_key;
  // The DevTools session, when this session was started with the pipe transport.
  // It exists for the parts of a challenge that no amount of synthesized input can
  // reach -- a hidden form field, or an audio element's own URL -- and it is not
  // connected at all when the browser was started with --no-cdp.
  CdpClient cdp;
  bool cdp_enabled = false;
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

bool arg_bool(const Json& request, const char* key, bool fallback) {
  const Json* value = request.find(key);
  if (value == nullptr) return fallback;
  if (value->type() == Json::Type::kBool) return value->as_bool(fallback);
  // Numbers are accepted too, because a JSON encoder that has only numbers to hand
  // will send 1/0 rather than true/false.
  if (value->is_number()) return value->as_number(fallback ? 1.0 : 0.0) != 0.0;
  return fallback;
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

// Where every scratch file this process writes goes: %TEMP%\<stem>-<pid>.<ext>.
// The pid is in the name because two browsers can be driven at once, and the
// second one must not overwrite the first one's recording mid-listen.
std::string temp_path(const char* stem, const char* extension) {
  char temp[MAX_PATH] = {0};
  std::string dir = ".";
  if (::GetTempPathA(MAX_PATH, temp) != 0) dir = temp;
  char name[64];
  std::snprintf(name, sizeof(name), "%s-%lu.%s", stem,
                static_cast<unsigned long>(::GetCurrentProcessId()), extension);
  return dir + name;
}

// Reads the human-verification challenge on the page, clicks its checkbox when
// that is what it is waiting for, and reports what happened.
//
// Most of the value is in the reading rather than the clicking: the reply names the
// provider, the sitekey and the frame the answer came from, which is what a caller
// needs to decide between waiting, solving the audio challenge, and handing the
// sitekey to a solving service. For Cloudflare Turnstile the click alone is usually
// the whole task, so this command can finish a challenge outright.
// Reads the challenge as it stands now. A widget that has disappeared is reported as
// solved rather than absent, because after a click the only sensible reading of "it
// is gone" is that it was answered — the caller asked about a challenge it had
// already been told exists.
bool read_captcha(Session& session, CaptchaInfo* out, std::string* error) {
  std::vector<Element> nodes = dump_tree(session.window, kTreeDepth, kTreeNodes, false, error);
  if (!error->empty()) return false;
  session.last_tree = nodes;
  *out = analyze_captcha(nodes);
  if (out->provider == CaptchaProvider::kNone) {
    out->state = CaptchaState::kSolved;
    out->detail = "the widget is gone";
  }
  return true;
}

// Wait for a challenge to name itself, and say how long that took.
//
// A single look is not a detection. The widget animates in, and measured on this
// machine the gap is not subtle: Cloudflare's interstitial exposes no challenge
// at all for its first ~1.2 s, and hCaptcha's checkbox for ~1.2 s, so during that
// window a page that is about to challenge you and a page that never will are the
// same picture. Reporting "no challenge" from one sample sends an agent into a
// blocked page with nothing to solve, which is precisely the failure the first
// tier exists to prevent.
//
// `appeared_ms` comes back as -1 when nothing appeared before the deadline, which
// is a different answer from "it appeared instantly" and is reported as such.
bool wait_for_challenge(Session& session, CaptchaInfo* out, int wait_ms,
                        long long* appeared_ms, std::string* error) {
  const ULONGLONG start = GetTickCount64();
  for (;;) {
    error->clear();
    std::vector<Element> nodes = dump_tree(session.window, kTreeDepth, kTreeNodes, false, error);
    if (!error->empty()) return false;
    session.last_tree = nodes;
    *out = analyze_captcha(nodes);
    if (out->provider != CaptchaProvider::kNone) {
      *appeared_ms = static_cast<long long>(GetTickCount64() - start);
      return true;
    }
    if (static_cast<long long>(GetTickCount64() - start) >= wait_ms) {
      *appeared_ms = -1;
      return true;
    }
    Sleep(200);
  }
}

// Click whatever the tree says is at `index`. The index is only valid for the
// most recent tree, so every caller re-reads before asking.
//
// Real synthesized input comes first: it is what a person produces, and only it
// reaches the page as a trusted event. UI Automation's own invocation is the
// fallback for sessions that cannot deliver input at all -- a disconnected RDP
// session, a service, anything headless -- and it is recorded as such, because a
// page can tell the two apart.
bool click_node(Session& session, int index, std::string* error) {
  const Element* target = nullptr;
  for (const Element& element : session.last_tree) {
    if (element.index == index) {
      target = &element;
      break;
    }
  }
  if (target == nullptr) {
    *error = "the control is no longer on the page";
    return false;
  }

  // Identity for re-finding the same control after the tree is re-read: an
  // index is a position in one walk, and anything that reflows the page
  // invalidates it. The automation id is Chromium's own element id and
  // survives a reflow; role plus accessible name is the fallback when the id
  // is empty.
  const std::string wanted_id = target->automation_id;
  const std::string wanted_role = target->role;
  const std::string wanted_name = target->name;

  int x = 0;
  int y = 0;

  // A click only lands when its coordinates sit inside the window's client
  // area: outside that, the desktop click hits whatever is beside the
  // browser, never the control.
  const auto centre_in_client = [&]() {
    if (target == nullptr || !element_center(*target, &x, &y)) return false;
    RECT client{0, 0, 0, 0};
    if (!GetClientRect(session.window, &client)) return false;
    POINT top_left{client.left, client.top};
    POINT bottom_right{client.right, client.bottom};
    if (!ClientToScreen(session.window, &top_left) ||
        !ClientToScreen(session.window, &bottom_right)) {
      return false;
    }
    return x >= top_left.x && x < bottom_right.x && y >= top_left.y &&
           y < bottom_right.y;
  };

  bool usable = centre_in_client();

  // A low-resolution screen or a small restored window can clip the challenge
  // dialog: the control's centre then falls outside the client area. Do what
  // a person does: scroll the control into view while the walked tree is
  // still the one on screen and the index is honest, make the window as
  // large as the screen allows, then re-read the tree -- both actions move
  // everything below them.
  if (!usable && element_center(*target, &x, &y)) {
    std::string fit_error;
    scroll_element_into_view(session.window, index, &fit_error);
    maximize_window(session.window, &fit_error);
    std::vector<Element> fresh =
        dump_tree(session.window, kTreeDepth, kTreeNodes, false, &fit_error);
    if (fit_error.empty()) {
      session.last_tree = fresh;
      int moved = -1;
      long best_distance = -1;
      for (const Element& element : session.last_tree) {
        const bool matches =
            !wanted_id.empty()
                ? element.automation_id == wanted_id &&
                      element.role == wanted_role
                : element.role == wanted_role && element.name == wanted_name;
        if (!matches) continue;
        // Layout order tends to survive a reflow, so among duplicate matches
        // the walk position closest to the old one is the best guess.
        const long distance =
            element.index > index ? element.index - index : index - element.index;
        if (best_distance < 0 || distance < best_distance) {
          best_distance = distance;
          moved = element.index;
        }
      }
      if (moved >= 0) {
        index = moved;
        for (const Element& element : session.last_tree) {
          if (element.index == index) {
            target = &element;
            break;
          }
        }
        usable = centre_in_client();
      }
    }
  }

  std::string activation_error;
  if (usable && activate_window(session.window, &activation_error)) {
    click_at(x, y, "left", 1);
    session.last_input = "synthesized";
    return true;
  }

  std::string invoke_error;
  if (invoke_element(session.window, index, &invoke_error)) {
    session.last_input = "accessibility";
    return true;
  }

  if (!usable) {
    *error = "the control has no visible area, and " + invoke_error;
  } else {
    *error = activation_error +
             "; the accessibility fallback also failed: " + invoke_error;
  }
  return false;
}

// Put text into the field at `index`.
//
// Typing is the honest channel -- the page sees a person at a keyboard -- but
// keystrokes have nowhere to land without a foreground window, and Windows drops
// them silently rather than reporting a failure. So a session that cannot deliver
// input writes the value through the accessibility interface instead, and the
// caller reports which happened, because a page can tell the two apart.
bool answer_field(Session& session, int index, const std::string& text,
                  std::string* error) {
  std::string activation_error;
  if (activate_window(session.window, &activation_error)) {
    if (!click_node(session, index, error)) return false;
    type_text(text);
    session.last_input = "synthesized";
    return true;
  }
  if (!set_element_value(session.window, index, text, error)) return false;
  session.last_input = "accessibility";
  return true;
}

// Who is holding a stream on the render endpoint, if anyone.
//
// A silent capture is ambiguous: the page may have played nothing, or nothing may
// have opened a stream at all. Windows already knows which -- it is the same list
// the volume mixer draws -- so the answer belongs in the report instead of in the
// next debugging session.
std::string stream_summary() {
  std::string error;
  const std::vector<AudioSession> sessions = list_audio_sessions(&error);
  if (sessions.empty()) {
    return error.empty() ? std::string("nothing held a stream") : error;
  }
  std::string out;
  for (const AudioSession& session : sessions) {
    if (session.system_sounds) continue;
    if (!out.empty()) out += "; ";
    char line[160];
    std::snprintf(line, sizeof(line), "pid %lu %s peak %.4f",
                  static_cast<unsigned long>(session.pid),
                  session.state == 1 ? "active" : "inactive",
                  static_cast<double>(session.peak));
    out += line;
  }
  return out.empty() ? std::string("only system sounds held a stream") : out;
}

// What the widget says about itself, read out of the document.
//
// None of this is in the accessibility tree, and not by accident: a site key is an
// attribute, the response field is hidden from sight and from the tree alike, and
// the callback that tells the page a token arrived is a JavaScript property. A tree
// of controls cannot express any of them, which is why the token route needs a
// transport that reads the document itself.
const char kCaptchaFactsJs[] = R"JS(
(function () {
  var out = {sitekey: "", page_url: location.href, response_field: false,
             recaptcha: false, hcaptcha: false, turnstile: false};
  var holder = document.querySelector("[data-sitekey]");
  if (holder) out.sitekey = holder.getAttribute("data-sitekey") || "";
  if (!out.sitekey) {
    var frames = document.querySelectorAll("iframe[src]");
    for (var i = 0; i < frames.length; i++) {
      var src = frames[i].getAttribute("src") || "";
      var match = src.match(/[?&](?:k|sitekey)=([^&]+)/);
      if (match) { out.sitekey = decodeURIComponent(match[1]); break; }
    }
  }
  out.recaptcha = !!document.querySelector(".g-recaptcha, [data-sitekey]");
  out.hcaptcha = !!document.querySelector(".h-captcha, [data-hcaptcha-sitekey]");
  out.turnstile = !!document.querySelector(".cf-turnstile, [data-turnstile-sitekey]");
  out.response_field = !!document.querySelector(
      "textarea[name=g-recaptcha-response], #g-recaptcha-response, " +
      "textarea[name=h-captcha-response], input[name=cf-turnstile-response]");
  return JSON.stringify(out);
})()
)JS";

// Hands the token to the page the way the widget itself would: into the hidden
// field, and then through the widget's own callback when it has one. The callback
// matters because a page that registered one never looks at the field.
std::string captcha_token_js(const std::string& token) {
  return std::string(
             "(function (token) {"
             "  var names = ['g-recaptcha-response', 'h-captcha-response',"
             "               'cf-turnstile-response'];"
             "  var filled = 0;"
             "  for (var n = 0; n < names.length; n++) {"
             "    var fields = document.querySelectorAll("
             "        '[name=\"' + names[n] + '\"], #' + names[n]);"
             "    for (var i = 0; i < fields.length; i++) {"
             "      fields[i].value = token;"
             "      fields[i].innerHTML = token;"
             "      filled++;"
             "    }"
             "  }"
             "  var called = false;"
             "  try {"
             "    var cfg = window.___grecaptcha_cfg;"
             "    if (cfg && cfg.clients) {"
             "      for (var k in cfg.clients) {"
             "        var client = cfg.clients[k];"
             "        for (var c in client) {"
             "          var holder = client[c];"
             "          if (holder && typeof holder.callback === 'function') {"
             "            holder.callback(token);"
             "            called = true;"
             "          }"
             "        }"
             "      }"
             "    }"
             "  } catch (e) {}"
             "  return JSON.stringify({filled: filled, callback: called});"
             "})(") +
         json_quote(token) + ")";
}

// Submits whatever form the response field belongs to. Doing it through the field
// rather than through a button keeps this working on pages that have no button at
// all, which is the common case for a widget inside someone else's form.
const char kCaptchaSubmitJs[] = R"JS(
(function () {
  var field = document.querySelector(
      "[name=g-recaptcha-response], #g-recaptcha-response, " +
      "[name=h-captcha-response], [name=cf-turnstile-response]");
  if (field) {
    var form = field.form || (field.closest ? field.closest("form") : null);
    if (form) { form.submit(); return "form"; }
  }
  var button = document.querySelector(
      "#recaptcha-verify-button, button[type=submit], input[type=submit]");
  if (button) { button.click(); return "button"; }
  return "nothing";
})()
)JS";

// The token route: ask a service for the string the widget would have produced,
// and give it to the page directly.
//
// This is the only route that solves an image challenge, and the reason is not
// cleverness. The service reproduces the challenge itself from the site key and
// the page URL, so nothing has to be photographed or described. What the previous
// tiers could not do was the last step -- put the answer where the page looks --
// and that step is a write into a hidden field, which is a DOM operation.
Json captcha_solve_token(Session& session, const Json& request, const CaptchaInfo& info) {
  if (!session.cdp_enabled || !session.cdp.connected()) {
    return failure(
        "the token route needs the DevTools pipe; this session was started with --no-cdp");
  }

  std::string error;
  std::string facts_text;
  if (!session.cdp.evaluate(kCaptchaFactsJs, &facts_text, &error)) return failure(error);
  std::string parse_error;
  const Json facts = Json::parse(facts_text, &parse_error);
  if (!facts.is_object()) {
    return failure("the page did not describe its challenge: " + parse_error);
  }

  const Json* sitekey = facts.find("sitekey");
  const Json* page_url = facts.find("page_url");
  std::string key = sitekey != nullptr ? sitekey->as_string() : std::string();
  const std::string url = page_url != nullptr ? page_url->as_string() : std::string();
  // The tree's own reading is the fallback: it saw the widget before the DOM was
  // asked, and on some pages it is the one that found the key.
  if (key.empty()) key = info.site_key;

  Json out = success();
  out.set("page_url", Json::string(url));
  if (!key.empty()) out.set("site_key", Json::string(key));
  if (facts.find("recaptcha") != nullptr && facts.find("recaptcha")->as_bool(false)) {
    out.set("kind", Json::string("recaptcha"));
  } else if (facts.find("hcaptcha") != nullptr && facts.find("hcaptcha")->as_bool(false)) {
    out.set("kind", Json::string("hcaptcha"));
  } else if (facts.find("turnstile") != nullptr && facts.find("turnstile")->as_bool(false)) {
    out.set("kind", Json::string("turnstile"));
  }
  out.set("response_field",
          Json::boolean(facts.find("response_field") != nullptr &&
                        facts.find("response_field")->as_bool(false)));

  if (key.empty()) {
    out.set("ok", Json::boolean(false));
    out.set("error", Json::string("the page carries no site key to solve for"));
    return out;
  }

  const SolveApi api = resolve_solve_api(arg_string(request, "key"), session.captcha_api_key);
  if (api.provider.empty()) {
    out.set("ok", Json::boolean(false));
    out.set("error", Json::string(
                         "no solving service is configured: set GHOST_CAPTCHA_KEY, or add "
                         "\"captcha_api_key\" to the profile"));
    return out;
  }
  out.set("provider", Json::string(api.provider));

  std::string token;
  if (!solve_recaptcha_api(api, key, url, &token, &error)) {
    out.set("ok", Json::boolean(false));
    out.set("error", Json::string(error));
    return out;
  }
  out.set("token_length", Json::integer(static_cast<long long>(token.size())));

  std::string injected;
  if (!session.cdp.evaluate(captcha_token_js(token), &injected, &error)) {
    out.set("ok", Json::boolean(false));
    out.set("error", Json::string(error));
    return out;
  }
  const Json placed = Json::parse(injected, &parse_error);
  long long filled = 0;
  if (placed.is_object()) {
    const Json* count = placed.find("filled");
    const Json* callback = placed.find("callback");
    if (count != nullptr) filled = static_cast<long long>(count->as_number(0));
    out.set("fields_filled", Json::integer(filled));
    if (callback != nullptr) out.set("callback_called", Json::boolean(callback->as_bool(false)));
  }
  if (filled == 0) {
    // A token that reached no field is not a solve, and saying so here is the
    // difference between a caller retrying and a caller believing it worked.
    out.set("ok", Json::boolean(false));
    out.set("error", Json::string(
                         "the page has no response field to receive the token; the widget "
                         "may not have finished loading"));
    return out;
  }

  std::string submitted;
  if (!session.cdp.evaluate(kCaptchaSubmitJs, &submitted, &error)) {
    out.set("ok", Json::boolean(false));
    out.set("error", Json::string(error));
    return out;
  }
  out.set("submitted", Json::string(submitted));
  out.set("solved_by", Json::string("api"));
  return out;
}

// Defined further down, next to the DevTools command it is built on. The audio
// tier reaches for it before any of the tree reading below, because a clip is a
// property of the page rather than of a widget the tree has recognised.
bool read_audio_url(Session& session, std::string* url, std::string* error);

Json cmd_captcha(Session& session, const Json& request) {
  std::string error;
  if (!refresh(session, &error)) return failure(error);

  const std::string action = arg_string(request, "action");
  const int timeout_ms = static_cast<int>(arg_number(request, "timeout", 30000));

  // How long the *first* look is allowed to keep looking. A caller that genuinely
  // wants one sample can pass `wait_ms: 0` and get the old behaviour.
  const int wait_ms = static_cast<int>(arg_number(request, "wait_ms", 3000));
  const ULONGLONG command_start = GetTickCount64();

  // The clip behind an audio challenge, taken from the page rather than from the
  // sound card.
  //
  // This is its own action because it is the capability the audio tier is built
  // on, and a caller that wants to know whether the clip is reachable should not
  // have to make the browser play something first. It runs before any tree
  // reading: an audio element is a property of the document, not of a widget the
  // accessibility tree has recognised, so requiring a challenge to be visible
  // first would refuse exactly the pages this is for.
  if (action == "audio-url") {
    std::string url;
    std::string url_error;
    if (!read_audio_url(session, &url, &url_error)) {
      Json missed = success();
      missed.set("found", Json::boolean(false));
      missed.set("detail", Json::string(url_error));
      return missed;
    }

    Json out = success();
    out.set("found", Json::boolean(true));
    out.set("url", Json::string(url));
    if (url.rfind("blob:", 0) == 0 || url.rfind("data:", 0) == 0) {
      out.set("fetched", Json::boolean(false));
      out.set("detail", Json::string(
                            "the clip lives in the renderer as a " +
                            url.substr(0, url.find(':')) +
                            " URL, which nothing outside the page can fetch"));
      return out;
    }

    const std::string given = arg_string(request, "path");
    const std::string path = given.empty() ? temp_path("ghost-clip", "bin") : given;
    long long bytes = 0;
    std::string download_error;
    if (!download_to_file(url, path, &bytes, &download_error)) {
      out.set("fetched", Json::boolean(false));
      out.set("detail", Json::string(download_error));
      return out;
    }
    out.set("fetched", Json::boolean(true));
    out.set("bytes", Json::integer(bytes));
    // The clip is a recording of a person's voice, so it is deleted unless the
    // caller asked to keep it -- the same rule the loopback recording follows.
    if (arg_bool(request, "keep", false)) {
      out.set("path", Json::string(path));
    } else {
      ::DeleteFileA(path.c_str());
    }
    return out;
  }

  std::vector<Element> nodes = dump_tree(session.window, kTreeDepth, kTreeNodes, false, &error);
  if (!error.empty()) return failure(error);
  session.last_tree = nodes;

  CaptchaInfo initial = analyze_captcha(nodes);
  long long appeared_ms = initial.provider == CaptchaProvider::kNone ? -1 : 0;

  // `wait` is the tier that exists to not act, so it gets the whole timeout to
  // watch a challenge arrive; every other action just needs the loading gap
  // covered before it decides there is nothing here. The token route is the one
  // exception in the other direction: it reads the document rather than the tree,
  // so waiting for the tree to notice something would only be a delay.
  const int appear_budget = (action == "wait")     ? timeout_ms
                            : (action == "solve-token") ? 0
                                                        : wait_ms;
  if (initial.provider == CaptchaProvider::kNone && appear_budget > 0) {
    if (!wait_for_challenge(session, &initial, appear_budget, &appeared_ms, &error)) {
      return failure(error);
    }
  }

  Json out = success();
  out.set("provider", Json::string(captcha_provider_name(initial.provider)));
  out.set("state", Json::string(captcha_state_name(initial.state)));
  out.set("detail", Json::string(initial.detail));
  if (appeared_ms >= 0) out.set("appeared_ms", Json::integer(appeared_ms));
  if (!initial.site_key.empty()) out.set("site_key", Json::string(initial.site_key));
  if (!initial.page_url.empty()) out.set("page_url", Json::string(initial.page_url));
  if (!initial.frame_url.empty()) out.set("frame_url", Json::string(initial.frame_url));
  if (!initial.challenge_token.empty()) {
    out.set("challenge_token", Json::string(initial.challenge_token));
  }

  // Wait for the challenge to arrive, and then for it to go away on its own.
  // Nothing is clicked: a challenge that is merely being verified needs time
  // rather than input, and clicking into that window is guessing.
  if (action == "wait") {
    out.set("clicked", Json::boolean(false));
    out.set("appeared", Json::boolean(initial.provider != CaptchaProvider::kNone));

    CaptchaInfo watched = initial;
    bool cleared = false;
    if (watched.provider != CaptchaProvider::kNone) {
      while (GetTickCount64() - command_start < static_cast<ULONGLONG>(timeout_ms)) {
        Sleep(250);
        if (!read_captcha(session, &watched, &error)) return failure(error);
        // Gone entirely, or reported solved: either way it passed without us.
        if (watched.provider == CaptchaProvider::kNone ||
            watched.state == CaptchaState::kSolved) {
          cleared = true;
          break;
        }
        // An image or audio challenge is waiting for a person. Waiting longer
        // will not change that, and spending the caller's whole deadline on a
        // foregone conclusion is not patience, it is waste.
        if (watched.state == CaptchaState::kVisual ||
            watched.state == CaptchaState::kAudio) {
          break;
        }
      }
    }
    out.set("cleared", Json::boolean(cleared));
    out.set("waited_ms", Json::integer(static_cast<long long>(GetTickCount64() - command_start)));
    out.set("state", Json::string(captcha_state_name(watched.state)));
    out.set("detail", Json::string(watched.detail));
    return out;
  }

  // The token route answers the challenge without touching the widget, and it does
  // not need the tree to have recognised one: it asks the document, which is where
  // the site key and the response field actually live. Running it before the
  // `detect` return is what lets it work on a page whose widget the tree cannot
  // describe.
  if (action == "solve-token") {
    return captcha_solve_token(session, request, initial);
  }

  if (action == "detect" || initial.provider == CaptchaProvider::kNone) {
    out.set("clicked", Json::boolean(false));
    return out;
  }

  // The one step worth taking automatically is the click that starts the challenge.
  // It is unambiguous — there is exactly one checkbox and one thing it means — and
  // for Turnstile it is often the entire task.
  //
  // The widget animates in, so the rectangle read on the first look can be a frame or
  // two stale by the time a click lands; a click delivered into that gap does nothing,
  // which is indistinguishable from a page refusing the click. So the tree is re-read
  // after a short settle, and a click that produces no change is retried once against
  // fresh coordinates.
  CaptchaInfo latest = initial;
  bool clicked = false;
  const ULONGLONG start = GetTickCount64();

  if (initial.state == CaptchaState::kCheckbox && initial.checkbox_index >= 0) {
    for (int attempt = 0; attempt < 2; ++attempt) {
      Sleep(800);
      if (!read_captcha(session, &latest, &error)) return failure(error);
      if (latest.state != CaptchaState::kCheckbox || latest.checkbox_index < 0) break;

      std::string click_error;
      if (!click_node(session, latest.checkbox_index, &click_error)) {
        return failure(click_error);
      }
      clicked = true;

      // Give the click a few seconds to show an effect before deciding it missed.
      const ULONGLONG clicked_at = GetTickCount64();
      while (GetTickCount64() - clicked_at < 3500) {
        Sleep(250);
        if (!read_captcha(session, &latest, &error)) return failure(error);
        if (latest.state != CaptchaState::kCheckbox) break;
      }
      if (latest.state != CaptchaState::kCheckbox) break;
    }
  }

  // The audio tier: answer a spoken challenge.
  //
  // The answer comes from one of two places, and the reply says which. The
  // challenge's own clip is reachable as a URL, and when a solving service is
  // configured that clip is fetched and handed straight over -- no capture, no
  // transcription, and no synthesized input anywhere in the path. When the clip
  // cannot be reached, or nothing would be able to read it, the samples come off
  // the render endpoint, which is where the browser put them, and the local
  // engine or the service reads those instead. Either way the only thing that
  // goes back into the page is a typed answer.
  if (action == "solve-audio") {
    // No upfront foreground requirement: the samples come off the render
    // endpoint and the answer can go in through accessibility, so this tier runs
    // in a session that cannot deliver synthesized input at all.

    // An image challenge is one click away from its audio sibling.
    if (latest.state == CaptchaState::kVisual && latest.audio_button_index >= 0) {
      std::string click_error;
      if (!click_node(session, latest.audio_button_index, &click_error)) {
        return failure(click_error);
      }
      clicked = true;
      const ULONGLONG asked = GetTickCount64();
      while (GetTickCount64() - asked < 10000) {
        Sleep(300);
        if (!read_captcha(session, &latest, &error)) return failure(error);
        if (latest.state == CaptchaState::kAudio) break;
      }
    }

    out.set("clicked", Json::boolean(clicked));
    out.set("state", Json::string(captcha_state_name(latest.state)));
    out.set("detail", Json::string(latest.detail));
    if (latest.state != CaptchaState::kAudio || latest.answer_field_index < 0) {
      out.set("solved", Json::boolean(false));
      out.set("detail", Json::string(
                            "there is no open audio challenge to answer: " +
                            std::string(captcha_state_name(latest.state))));
      return out;
    }

    double seconds = arg_number(request, "seconds", 10.0);
    if (seconds < 2.0) seconds = 2.0;
    if (seconds > 30.0) seconds = 30.0;

    // reCAPTCHA keeps the conversation going: a clip answered incorrectly is
    // commonly followed by another clip in the same panel, and a caller that
    // expects that wants the whole exchange handled here instead of one
    // re-invocation per clip. `rounds` bounds the exchange; the default of 1
    // leaves the reply exactly what a single listen has always produced.
    int rounds = static_cast<int>(arg_number(request, "rounds", 1.0));
    if (rounds < 1) rounds = 1;
    if (rounds > 5) rounds = 5;

    char temp[MAX_PATH] = {0};
    std::string dir = ".";
    if (::GetTempPathA(MAX_PATH, temp) != 0) dir = temp;
    char name[64];
    std::snprintf(name, sizeof(name), "ghost-challenge-%lu.wav",
                  static_cast<unsigned long>(::GetCurrentProcessId()));
    const std::string wav_path = dir + name;

    // The exchange ends from three places -- the challenge resolving itself
    // between rounds, the refresh click moving it somewhere else, and the
    // normal tail below -- so the closing fields live here once. `attempted`
    // below zero keeps `rounds_attempted` out of a reply that never left a
    // single round.
    const auto report_end = [&out, &session](const CaptchaInfo& info,
                                             int attempted) {
      out.set("state", Json::string(captcha_state_name(info.state)));
      out.set("detail", Json::string(info.detail));
      out.set("typed", Json::boolean(true));
      out.set("input", Json::string(session.last_input));
      out.set("solved", Json::boolean(info.state == CaptchaState::kSolved));
      if (attempted >= 0) out.set("rounds_attempted", Json::integer(attempted));
    };

    for (int round = 1;; ++round) {
      if (round > 1) {
        // The watch below can end while the panel is still redrawing, so the
        // indices the last round used are stale. The tree is read again
        // before anything is clicked, and one read is confirmed before the
        // exchange is called over -- a snapshot can land inside a redraw.
        if (!read_captcha(session, &latest, &error)) return failure(error);
        if (latest.state != CaptchaState::kAudio) {
          Sleep(400);
          if (!read_captcha(session, &latest, &error)) return failure(error);
        }
        if (latest.state != CaptchaState::kAudio ||
            latest.answer_field_index < 0) {
          report_end(latest, rounds > 1 ? round - 1 : -1);
          return out;
        }
        // Playing the same clip again repeats the same mishearing; the
        // refresh control is what makes the next round a different question.
        // The click is best effort: a refused refresh still leaves the replay
        // path something to listen to.
        if (latest.refresh_button_index >= 0) {
          std::string refresh_error;
          click_node(session, latest.refresh_button_index, &refresh_error);
          Sleep(800);
          if (!read_captcha(session, &latest, &error)) return failure(error);
          if (latest.state != CaptchaState::kAudio ||
              latest.answer_field_index < 0) {
            report_end(latest, rounds > 1 ? round - 1 : -1);
            return out;
          }
        }
      }

      // Where the answer comes from, tried in this order.
      //
      // First the challenge's own clip: it is reachable as a plain HTTPS GET, it
      // needs no synthesized input and no capture window, and it cannot suffer the
      // "played nothing" ambiguity that a render-endpoint capture can. Only the
      // service tier can use it, because the local engine wants samples and
      // decoding a compressed clip is not something this build does.
      //
      // Then the sound card, unchanged. That is the route the local engine needs,
      // and it is also what runs when no service is configured.
      Transcript transcript;
      std::string solved_by = "local";
      const std::string language = arg_string(request, "language");
      const SolveApi api = resolve_solve_api(arg_string(request, "key"),
                                             session.captcha_api_key);

      bool from_clip = false;
      if (session.cdp_enabled && !api.provider.empty()) {
        std::string clip_url;
        std::string url_error;
        if (read_audio_url(session, &clip_url, &url_error)) {
          out.set("audio_url", Json::string(clip_url));
          const std::string clip_path = wav_path + ".clip";
          long long clip_bytes = 0;
          std::string download_error;
          if (download_to_file(clip_url, clip_path, &clip_bytes, &download_error)) {
            std::string api_digits;
            std::string api_error;
            if (solve_audio_api(api, clip_path,
                                language.empty() ? std::string("en") : language,
                                &api_digits, &api_error)) {
              transcript.digits = api_digits;
              transcript.text = api_digits;
              transcript.confidence = 1.0;
              // The service answered, so the local engine's silence is not the
              // verdict -- the same correction the loopback path makes below.
              transcript.ok = true;
              solved_by = "api";
              from_clip = true;
              out.set("audio_source", Json::string("url"));
              out.set("clip_bytes", Json::integer(clip_bytes));
            } else {
              out.set("api", Json::string(api_error));
            }
            ::DeleteFileA(clip_path.c_str());
          } else {
            out.set("audio_url_error", Json::string(download_error));
          }
        }
      }

      if (!from_clip) {
        // The challenge does not play itself. reCAPTCHA's audio frame keeps the clip
        // behind its own play control, and until that control is pressed the audio
        // element holds an open render session that produces nothing -- which is why a
        // capture taken without this click measures `active peak 0.0000` and is
        // indistinguishable from a page that simply stayed silent.
        //
        // The recorder starts first so that pressing the control lands inside the
        // capture window rather than before it.
        CaptureResult captured;
        std::thread recorder([&captured, seconds, &wav_path]() {
          captured = capture_loopback(seconds, std::string(), wav_path);
        });
        Sleep(300);

        const int play_index = latest.play_button_index;
        if (play_index >= 0) {
          std::string play_error;
          if (click_node(session, play_index, &play_error)) {
            out.set("play", Json::string("pressed the challenge's play control"));
          } else {
            out.set("play", Json::string("the play control could not be pressed: " + play_error));
          }
        } else {
          // A challenge shaped differently, or one that autoplays. Asking for a replay is
          // the best available way to put the start of the clip inside the window.
          out.set("play", Json::string("the challenge exposes no play control"));
          if (latest.refresh_button_index >= 0) {
            std::string click_error;
            click_node(session, latest.refresh_button_index, &click_error);
          }
        }
        recorder.join();

        out.set("device", Json::string(captured.device_name));
        out.set("captured_seconds", Json::number(captured.seconds));
        out.set("peak", Json::number(captured.peak));
        out.set("rms", Json::number(captured.rms));
        // Whether a stream existed at all, which the samples alone cannot say.
        const std::string streams = stream_summary();
        out.set("streams", Json::string(streams));
        if (!captured.ok) {
          out.set("solved", Json::boolean(false));
          out.set("detail", Json::string(captured.error));
          if (rounds > 1) out.set("rounds_attempted", Json::integer(round));
          return out;
        }
        if (captured.peak <= 0.0) {
          out.set("solved", Json::boolean(false));
          out.set("detail", Json::string("the challenge played nothing: " +
                                         captured.device_name + " stayed silent (" +
                                         streams + ")"));
          if (rounds > 1) out.set("rounds_attempted", Json::integer(round));
          return out;
        }

        transcript = recognize_wav(wav_path, language, true);
        // Which tier produced the answer. The local engine ships with the machine and
        // costs nothing, so it always goes first; a service is consulted only when it
        // had nothing to say, because on a machine with no recognizer for the
        // challenge's language that is exactly what it will say.
        if (transcript.digits.empty() && !api.provider.empty()) {
          // The service wants a small upload, and the render endpoint's 44.1 kHz stereo
          // is neither small nor necessary for speech.
          const std::string speech_path = wav_path + ".16k.wav";
          std::string convert_error;
          std::string api_digits;
          std::string api_error;
          if (!write_speech_wav(speech_path, captured, 16000, &convert_error)) {
            out.set("api", Json::string(convert_error));
          } else if (solve_audio_api(api, speech_path,
                                     language.empty() ? std::string("en") : language,
                                     &api_digits, &api_error)) {
            transcript.digits = api_digits;
            transcript.text = api_digits;
            transcript.confidence = 1.0;
            // The local engine's failure has been answered, so it is no longer the
            // verdict. Leaving `ok` false here would report the local error and never
            // use the answer the service just gave.
            transcript.ok = true;
            transcript.error.clear();
            solved_by = "api";
          } else {
            out.set("api", Json::string(api_error));
          }
          ::DeleteFileA(speech_path.c_str());
        }
      }
      out.set("solved_by", Json::string(solved_by));
      // A failed transcription is the interesting case, and it cannot be diagnosed from
      // the samples alone -- too quiet, too short and wrong-language all look alike in
      // the numbers. `keep` leaves the recording behind so the caller can measure it.
      // Only the capture route has a recording to keep.
      if (!from_clip) {
        if (arg_bool(request, "keep", false)) {
          out.set("wav", Json::string(wav_path));
        } else {
          ::DeleteFileA(wav_path.c_str());
        }
      }
      if (!transcript.text.empty()) out.set("transcript", Json::string(transcript.text));
      if (!transcript.digits.empty()) out.set("heard", Json::string(transcript.digits));
      out.set("confidence", Json::number(transcript.confidence));
      // Which engine answered. Silence from the wrong language and silence from a bad
      // recording are the same number, and the culture is what tells them apart.
      if (!transcript.recognizer.empty()) {
        out.set("recognizer", Json::string(transcript.recognizer));
      }

      // A listen that produced no answer is the case another round exists
      // for: the engine misheard, or the clip was clipped. The remaining
      // rounds are spent before the failure is reported, and only the last
      // round reports it.
      if (!transcript.ok || transcript.digits.empty()) {
        if (round < rounds) continue;
        if (!transcript.ok) {
          out.set("solved", Json::boolean(false));
          out.set("detail", Json::string(transcript.error));
          if (rounds > 1) out.set("rounds_attempted", Json::integer(round));
          return out;
        }
        out.set("solved", Json::boolean(false));
        // The capture already proved that sound was there, so an empty transcript is
        // about the engine, not the audio. Saying which engine makes the difference
        // between "retry" and "this machine cannot hear this challenge".
        std::string detail = "the audio held nothing that sounded like digits";
        if (!transcript.recognizer.empty()) {
          detail += " (the local recognizer is " + transcript.recognizer +
                    "; a recognizer for the challenge's language may be needed)";
        }
        out.set("detail", Json::string(detail));
        if (rounds > 1) out.set("rounds_attempted", Json::integer(round));
        return out;
      }

      std::string answer_error;
      if (!answer_field(session, latest.answer_field_index, transcript.digits,
                        &answer_error)) {
        return failure(answer_error);
      }
      Sleep(200);
      std::string click_error;
      if (latest.verify_button_index >= 0) {
        if (!click_node(session, latest.verify_button_index, &click_error)) {
          return failure(click_error);
        }
      }

      CaptchaInfo after = latest;
      const ULONGLONG answered = GetTickCount64();
      while (GetTickCount64() - answered < 8000) {
        Sleep(400);
        if (!read_captcha(session, &after, &error)) break;
        if (after.state != CaptchaState::kAudio) break;
      }
      out.set("typed", Json::boolean(true));
      out.set("input", Json::string(session.last_input));
      out.set("state", Json::string(captcha_state_name(after.state)));
      out.set("detail", Json::string(after.detail));
      out.set("solved", Json::boolean(after.state == CaptchaState::kSolved));
      if (rounds > 1) out.set("rounds_attempted", Json::integer(round));
      // Solved, or the widget changed shape, or the allowed listens are spent:
      // the reply is final. Still audio means the answer did not take, and
      // another round has been asked for.
      if (after.state != CaptchaState::kAudio || round >= rounds) return out;
    }
  }

  // Then watch the rest of the way. The widget disappearing means it was answered;
  // changing shape means it escalated to something a person still has to solve, which
  // is the caller's business rather than ours.
  //
  // `absent` is watched too, and that is not the same as "there is no challenge": it
  // is what Cloudflare looks like while it verifies, because the checkbox is gone but
  // the widget is still there. Stopping at that point reported a challenge that was
  // busy passing as one that had failed.
  while (clicked && (latest.state == CaptchaState::kCheckbox ||
                     latest.state == CaptchaState::kAbsent) &&
         GetTickCount64() - start < static_cast<ULONGLONG>(timeout_ms)) {
    Sleep(250);
    if (!read_captcha(session, &latest, &error)) return failure(error);
  }

  out.set("clicked", Json::boolean(clicked));
  // Which channel carried the click matters to anyone reading this: a challenge
  // answered by an accessibility invocation is not the same as one answered by a
  // trusted click, even when the visible state ends up identical.
  if (clicked && !session.last_input.empty()) {
    out.set("input", Json::string(session.last_input));
  }
  out.set("state", Json::string(captcha_state_name(latest.state)));
  out.set("detail", Json::string(latest.detail));
  if (latest.provider != initial.provider) {
    out.set("provider", Json::string(captcha_provider_name(latest.provider)));
  }
  out.set("elapsed_ms", Json::integer(static_cast<long long>(GetTickCount64() - start)));
  return out;
}

Json cmd_click(Session& session, const Json& request) {
  std::string error;
  if (!refresh(session, &error)) return failure(error);

  const std::string button = arg_string(request, "button");
  const int count = static_cast<int>(arg_number(request, "count", 1));
  const bool plain = (button.empty() || button == "left") && count == 1;

  // A pointer click needs a session Windows will accept input in. When this one
  // cannot deliver input, a named control can still be activated through the
  // accessibility interface -- but only a plain left click, because that is the
  // only gesture that interface expresses. A right click or a double click has
  // no equivalent, and is refused rather than silently downgraded to something
  // that is not what the caller asked for.
  std::string activation_error;
  if (!activate_window(session.window, &activation_error)) {
    const Json* index = request.find("index");
    if (index == nullptr || !plain) return failure(activation_error);
    const int node = static_cast<int>(index->as_number());
    std::string click_error;
    if (!click_node(session, node, &click_error)) return failure(click_error);
    Json out = success();
    out.set("index", Json::integer(node));
    out.set("input", Json::string(session.last_input));
    return out;
  }

  int x = 0;
  int y = 0;
  if (!resolve_point(request, session, &x, &y, &error)) return failure(error);

  click_at(x, y, button.empty() ? "left" : button, count);
  session.last_input = "synthesized";

  Json out = success();
  out.set("x", Json::integer(x));
  out.set("y", Json::integer(y));
  out.set("input", Json::string(session.last_input));
  return out;
}

Json cmd_type(Session& session, const Json& request) {
  std::string error;
  if (!refresh(session, &error)) return failure(error);
  const std::string text = arg_string(request, "text");
  if (text.empty()) return failure("type needs text");

  // Keystrokes need a foreground window; a named field does not. Same rule as
  // clicking: use the honest channel when it is available, say so when it is not.
  std::string activation_error;
  if (!activate_window(session.window, &activation_error)) {
    const Json* index = request.find("index");
    if (index == nullptr) return failure(activation_error);
    const int node = static_cast<int>(index->as_number());
    if (!set_element_value(session.window, node, text, &error)) return failure(error);
    session.last_input = "accessibility";
    Json out = success();
    out.set("typed", Json::integer(static_cast<long long>(text.size())));
    out.set("input", Json::string(session.last_input));
    return out;
  }

  type_text(text);
  session.last_input = "synthesized";
  Json out = success();
  out.set("typed", Json::integer(static_cast<long long>(text.size())));
  out.set("input", Json::string(session.last_input));
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

// Turns a reference into an absolute URL against the document it was written in.
//
// This is not optional politeness. The DevTools tree hands back the attribute
// exactly as it was authored, so a challenge that writes src="/payload" -- which
// is what a plain <audio src> in an iframe looks like -- arrives as a path, and
// nothing outside the page can fetch a path. The document node carries the base
// to resolve it against, so this costs no page script.
std::string resolve_url(const std::string& base, const std::string& reference) {
  if (reference.empty() || base.empty()) return reference;
  std::string head;
  head.reserve(8);
  for (const char c : reference) {
    if (head.size() >= 8) break;
    head.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c);
  }
  // Already absolute, or not something a downloader could ever fetch. The caller
  // reports the latter as a clip it cannot reach.
  if (head.rfind("http://", 0) == 0 || head.rfind("https://", 0) == 0 ||
      head.rfind("data:", 0) == 0 || head.rfind("blob:", 0) == 0) {
    return reference;
  }
  const std::size_t scheme_end = base.find("://");
  if (scheme_end == std::string::npos) return reference;
  const std::size_t authority_start = scheme_end + 3;
  const std::size_t authority_end = base.find('/', authority_start);
  const std::string origin =
      base.substr(0, authority_end == std::string::npos ? base.size() : authority_end);
  if (reference.rfind("//", 0) == 0) return base.substr(0, scheme_end + 1) + reference;
  if (reference[0] == '/') return origin + reference;
  // A path relative to the document, so its own last segment is replaced. The
  // query and fragment of the document are not part of the directory.
  std::string directory = base;
  const std::size_t cut = directory.find_first_of("?#");
  if (cut != std::string::npos) directory.erase(cut);
  const std::size_t slash = directory.rfind('/');
  directory = (slash == std::string::npos || slash < authority_start)
                  ? origin + "/"
                  : directory.substr(0, slash + 1);
  return directory + reference;
}

// Walks a DOM.getDocument tree for the element a challenge keeps its clip in.
// Both spellings are accepted: a source on the <audio> element itself, and a
// nested <source> child, which is what a challenge uses when it offers the same
// clip in more than one container.
bool find_audio_src(const Json& node, const std::string& base, std::string* url) {
  if (!node.is_object()) return false;
  // A document node names the base every reference inside it resolves against,
  // which is also how the walk learns the base of a frame it just descended into.
  std::string here = base;
  const Json* node_base = node.find("baseURL");
  if (node_base != nullptr && node_base->is_string() && !node_base->as_string().empty()) {
    here = node_base->as_string();
  }
  const Json* name = node.find("nodeName");
  const std::string tag = name != nullptr ? name->as_string() : std::string();
  if (tag == "AUDIO" || tag == "SOURCE") {
    const Json* attributes = node.find("attributes");
    if (attributes != nullptr && attributes->is_array()) {
      // CDP flattens attributes into [name, value, name, value, ...].
      const std::vector<Json>& pairs = attributes->items();
      for (size_t i = 0; i + 1 < pairs.size(); i += 2) {
        if (pairs[i].as_string() != "src") continue;
        const std::string value = pairs[i + 1].as_string();
        if (!value.empty()) {
          *url = resolve_url(here, value);
          return true;
        }
      }
    }
  }
  const Json* children = node.find("children");
  if (children != nullptr && children->is_array()) {
    for (const Json& child : children->items()) {
      if (find_audio_src(child, here, url)) return true;
    }
  }
  // A same-site frame arrives as a contentDocument on the frame element, so a
  // challenge served from the page's own domain is reached without attaching to
  // anything at all.
  const Json* content = node.find("contentDocument");
  if (content != nullptr && find_audio_src(*content, here, url)) return true;
  const Json* shadows = node.find("shadowRoots");
  if (shadows != nullptr && shadows->is_array()) {
    for (const Json& shadow : shadows->items()) {
      if (find_audio_src(shadow, here, url)) return true;
    }
  }
  return false;
}

// Reads the clip's URL out of the page.
//
// Two shapes have to be handled, and which one appears depends on the site rather
// than on the challenge. When the challenge's frame is same-site with the page --
// which is what a vendor's own demo page looks like -- Chromium keeps it in the
// page's renderer and the frame's document arrives inside DOM.getDocument as a
// contentDocument, so one call on the page session is enough. DOM.getDocument
// needs no DOM.enable and no Runtime.enable, so nothing about the page's own
// view of itself changes.
//
// On a real third-party site the frame is cross-site, so Chromium gives it a
// renderer of its own and the page's tree cannot contain it. There the frame is a
// target in its own right and the tree has to be asked for on a session attached
// to it. Both paths are tried, in that order, because guessing wrong is silent.
bool read_audio_url(Session& session, std::string* url, std::string* error) {
  if (!session.cdp_enabled || !session.cdp.connected()) {
    if (error != nullptr) {
      *error = "this session has no DevTools pipe, so the page's DOM cannot be read";
    }
    return false;
  }
  std::string attach_error;
  if (!session.cdp.attach_to_page(&attach_error)) {
    if (error != nullptr) *error = attach_error;
    return false;
  }

  Json params = Json::object();
  params.set("depth", Json::integer(-1));
  params.set("pierce", Json::boolean(true));

  std::string call_error;
  Json document;
  if (session.cdp.call("DOM.getDocument", params, session.cdp.page_session(), &document,
                       &call_error)) {
    const Json* root = document.find("root");
    if (root != nullptr && find_audio_src(*root, std::string(), url) && !url->empty()) {
      return true;
    }
  }

  // Out-of-process frame: find it by name and ask it directly. Only challenge
  // frames are attached to, so a page full of advertising iframes does not turn
  // one DOM read into a dozen sessions.
  Json targets;
  if (!session.cdp.call("Target.getTargets", Json::object(), std::string(), &targets,
                        &call_error)) {
    if (error != nullptr) *error = call_error;
    return false;
  }
  const Json* infos = targets.find("targetInfos");
  if (infos == nullptr || !infos->is_array()) {
    if (error != nullptr) *error = "the browser listed no targets";
    return false;
  }
  // Frames named after a known challenge vendor go first, because that is what the
  // three providers this control plane knows about look like and it is the cheap
  // common case. The rest are tried too, in the order the browser listed them,
  // because the set of vendors is not something this code gets to decide: a
  // provider can rename its path, and a frame named after nothing at all can still
  // be the one holding the clip. Filtering to the known names alone silently
  // refuses those, which is how this was first written and how it was caught.
  std::vector<std::string> named;
  std::vector<std::string> rest;
  for (const Json& info : infos->items()) {
    const Json* type = info.find("type");
    const Json* frame_url = info.find("url");
    const Json* id = info.find("targetId");
    if (type == nullptr || frame_url == nullptr || id == nullptr) continue;
    if (type->as_string() != "iframe") continue;
    const std::string address = frame_url->as_string();
    if (address.empty() || address == "about:blank") continue;
    if (address.find("recaptcha") != std::string::npos ||
        address.find("hcaptcha") != std::string::npos ||
        address.find("challenges.cloudflare.com") != std::string::npos) {
      named.push_back(id->as_string());
    } else {
      rest.push_back(id->as_string());
    }
  }
  named.insert(named.end(), rest.begin(), rest.end());
  // Every frame costs a tree read, so the search stops at a dozen. A challenge
  // frame is not the thirteenth iframe on a page, and if it somehow were, the
  // sound card is still there to fall back on.
  const size_t limit = named.size() < 12 ? named.size() : 12;
  for (size_t i = 0; i < limit; ++i) {
    Json attach = Json::object();
    attach.set("targetId", Json::string(named[i]));
    attach.set("flatten", Json::boolean(true));
    Json attached;
    if (!session.cdp.call("Target.attachToTarget", attach, std::string(), &attached,
                          &call_error)) {
      continue;
    }
    const Json* frame_session = attached.find("sessionId");
    if (frame_session == nullptr) continue;
    Json frame_document;
    if (session.cdp.call("DOM.getDocument", params, frame_session->as_string(),
                         &frame_document, &call_error)) {
      const Json* frame_root = frame_document.find("root");
      if (frame_root != nullptr && find_audio_src(*frame_root, std::string(), url) &&
          !url->empty()) {
        return true;
      }
    }
  }

  if (error != nullptr) *error = "the page holds no audio element with a source";
  return false;
}

// The escape hatch: one DevTools command, straight through.
//
// The rest of this control plane exists because the accessibility tree cannot
// reach certain things -- a hidden form field, an element's own URL, the text of
// an attribute no control exposes. This command is what those callers are built
// on, and it is deliberately raw: the caller names the method and the parameters
// and gets the browser's result back unchanged.
//
// It never sends Runtime.enable. That command is the one with a detectable side
// effect (it changes what Error.stack and the console object look like from
// inside the page); Runtime.evaluate needs no such setup.
Json cmd_cdp(Session& session, const Json& request) {
  if (!session.cdp_enabled) {
    return failure("this session was started with --no-cdp, so there is no DevTools pipe");
  }
  if (!session.cdp.connected()) {
    return failure("no DevTools pipe is attached to this session");
  }

  const std::string method = arg_string(request, "method");
  if (method.empty()) {
    Json out = success();
    out.set("connected", Json::boolean(true));
    out.set("page_session", Json::boolean(!session.cdp.page_session().empty()));
    return out;
  }

  // "browser" addresses the browser itself (Target.*, Browser.*), "page" -- or
  // nothing -- means the page this session is attached to, and anything else is
  // taken as a session id the caller already holds. That last form is the only
  // way to reach a target the page's own tree does not contain, such as a
  // cross-site challenge frame reached through Target.attachToTarget: its
  // reply carries the sessionId, and this is where it goes back in.
  const std::string scope = arg_string(request, "session");
  std::string session_id;
  if (scope == "browser") {
    // The browser scope is addressed without a session id.
  } else if (scope.empty() || scope == "page") {
    std::string attach_error;
    if (!session.cdp.attach_to_page(&attach_error)) return failure(attach_error);
    session_id = session.cdp.page_session();
  } else {
    session_id = scope;
  }

  const Json* params = request.find("params");
  Json result;
  std::string error;
  if (!session.cdp.call(method, params != nullptr ? *params : Json::object(), session_id,
                        &result, &error)) {
    return failure(error);
  }

  Json out = success();
  out.set("method", Json::string(method));
  out.set("result", result);
  return out;
}

// Chromium marks a renderer hidden when its window cannot become the foreground
// window, and a hidden renderer does two things that look like unrelated bugs: it
// does not build an accessibility tree, and it silently discards every CDP input
// event. So `tree`, `find`, `click` and `type` all go dead at once, with no error
// reported anywhere -- the raw Input replies are a cheerful `{}`. One DevTools call
// brings the renderer back, and it has to be made before the first `tree` is asked
// for: the tree appears when the renderer learns it is visible, not when a client
// asks for it.
//
// It is applied only when the page really does report itself hidden *and* this
// session has no foreground window at all. On a machine where somebody is looking
// at the browser, forcing focus would keep the page from ever seeing a blur, which
// is an anomaly of its own -- so the anomalous state is repaired and the normal one
// is left alone.
void repair_hidden_renderer(Session& session) {
  if (!session.cdp_enabled || !session.cdp.connected()) return;
  if (GetForegroundWindow() != nullptr) return;

  // The first document may not have committed yet, so give the page a few seconds
  // to say what it is rather than reading about:blank and concluding all is well.
  for (int attempt = 0; attempt < 20; ++attempt) {
    std::string attach_error;
    std::string value;
    std::string error;
    if (session.cdp.attach_to_page(&attach_error) &&
        session.cdp.evaluate("document.visibilityState", &value, &error)) {
      if (value.find("hidden") == std::string::npos) return;
      Json params = Json::object();
      params.set("enabled", Json::boolean(true));
      Json result;
      if (session.cdp.call("Emulation.setFocusEmulationEnabled", params,
                           session.cdp.page_session(), &result, &error)) {
        std::printf("ghost serve: renderer was hidden (no foreground window); focus emulation on\n");
      } else {
        std::printf("ghost serve: renderer is hidden and could not be woken: %s\n", error.c_str());
      }
      std::fflush(stdout);
      return;
    }
    Sleep(250);
  }
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
  if (command == "captcha") return cmd_captcha(session, request);
  if (command == "cdp") return cmd_cdp(session, request);
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
  if (!options.profile_json.empty()) {
    std::string parse_error;
    const Json profile = Json::parse(options.profile_json, &parse_error);
    const Json* key = profile.find("captcha_api_key");
    if (key != nullptr) session.captcha_api_key = key->as_string();
  }

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
    lo.cdp = options.cdp;
    session.cdp_enabled = options.cdp;

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

    HANDLE cdp_read = nullptr;
    HANDLE cdp_write = nullptr;
    if (options.cdp) {
      lo.cdp_read_out = &cdp_read;
      lo.cdp_write_out = &cdp_write;
    }

    const int launch_code = launch_under_shim(lo);
    if (launch_code != 0) {
      std::fprintf(stderr, "ghost: the browser did not start (code %d)\n", launch_code);
      return launch_code;
    }
    if (options.cdp) session.cdp.adopt(cdp_read, cdp_write);
    wait_for_window(session.pid, 20000);
  }

  // Wake a hidden renderer before asking for its accessibility tree. This has to
  // come first: while the renderer believes it is hidden it builds no tree at all,
  // so priming accessibility before this would only confirm that there is nothing
  // to prime.
  repair_hidden_renderer(session);

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
