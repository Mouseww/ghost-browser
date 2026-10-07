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

  int x = 0;
  int y = 0;
  const bool reachable = element_center(*target, &x, &y);
  std::string activation_error;
  if (reachable && activate_window(session.window, &activation_error)) {
    click_at(x, y, "left", 1);
    session.last_input = "synthesized";
    return true;
  }

  std::string invoke_error;
  if (invoke_element(session.window, index, &invoke_error)) {
    session.last_input = "accessibility";
    return true;
  }

  if (!reachable) {
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

Json cmd_captcha(Session& session, const Json& request) {
  std::string error;
  if (!refresh(session, &error)) return failure(error);

  const std::string action = arg_string(request, "action");
  const int timeout_ms = static_cast<int>(arg_number(request, "timeout", 30000));

  std::vector<Element> nodes = dump_tree(session.window, kTreeDepth, kTreeNodes, false, &error);
  if (!error.empty()) return failure(error);
  session.last_tree = nodes;

  const CaptchaInfo initial = analyze_captcha(nodes);

  Json out = success();
  out.set("provider", Json::string(captcha_provider_name(initial.provider)));
  out.set("state", Json::string(captcha_state_name(initial.state)));
  out.set("detail", Json::string(initial.detail));
  if (!initial.site_key.empty()) out.set("site_key", Json::string(initial.site_key));
  if (!initial.page_url.empty()) out.set("page_url", Json::string(initial.page_url));
  if (!initial.frame_url.empty()) out.set("frame_url", Json::string(initial.frame_url));
  if (!initial.challenge_token.empty()) {
    out.set("challenge_token", Json::string(initial.challenge_token));
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

  // The audio tier: answer a spoken challenge with what the sound card heard.
  //
  // Nothing below reads the audio URL or reaches into the page. The samples come
  // off the render endpoint, which is where the browser put them, and the only
  // thing that goes back into the page is a typed answer -- so this runs under
  // exactly the same zero-CDP rule as the click does.
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

    char temp[MAX_PATH] = {0};
    std::string dir = ".";
    if (::GetTempPathA(MAX_PATH, temp) != 0) dir = temp;
    char name[64];
    std::snprintf(name, sizeof(name), "ghost-challenge-%lu.wav",
                  static_cast<unsigned long>(::GetCurrentProcessId()));
    const std::string wav_path = dir + name;

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
      return out;
    }
    if (captured.peak <= 0.0) {
      out.set("solved", Json::boolean(false));
      out.set("detail", Json::string("the challenge played nothing: " +
                                     captured.device_name + " stayed silent (" +
                                     streams + ")"));
      return out;
    }

    const std::string language = arg_string(request, "language");
    Transcript transcript = recognize_wav(wav_path, language, true);
    // Which tier produced the answer. The local engine ships with the machine and
    // costs nothing, so it always goes first; a service is consulted only when it
    // had nothing to say, because on a machine with no recognizer for the
    // challenge's language that is exactly what it will say.
    std::string solved_by = "local";
    const SolveApi api = resolve_solve_api(arg_string(request, "key"),
                                           session.captcha_api_key);
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
    out.set("solved_by", Json::string(solved_by));
    // A failed transcription is the interesting case, and it cannot be diagnosed from
    // the samples alone -- too quiet, too short and wrong-language all look alike in
    // the numbers. `keep` leaves the recording behind so the caller can measure it.
    if (arg_bool(request, "keep", false)) {
      out.set("wav", Json::string(wav_path));
    } else {
      ::DeleteFileA(wav_path.c_str());
    }
    if (!transcript.text.empty()) out.set("transcript", Json::string(transcript.text));
    if (!transcript.digits.empty()) out.set("heard", Json::string(transcript.digits));
    out.set("confidence", Json::number(transcript.confidence));
    // Which engine answered. Silence from the wrong language and silence from a bad
    // recording are the same number, and the culture is what tells them apart.
    if (!transcript.recognizer.empty()) {
      out.set("recognizer", Json::string(transcript.recognizer));
    }
    if (!transcript.ok) {
      out.set("solved", Json::boolean(false));
      out.set("detail", Json::string(transcript.error));
      return out;
    }
    if (transcript.digits.empty()) {
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
    return out;
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
