// ghost.exe — the whole product in one file.
//
// The native shim is embedded as a resource, so a single downloaded executable
// is a complete fingerprint browser: it finds the installed Chromium, invents a
// coherent machine identity, extracts the shim next to the user's local app
// data, and starts the browser with the shim already inside it.
//
// Subcommands exist because "just double-click it" is not a workflow an agent
// can drive. Everything the product can do is reachable from argv.
#include <windows.h>
#include <shellapi.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../../common/probe_report.h"
#include "audio_capture.h"
#include "speech.h"
#include "chrome.h"
#include "daemon.h"
#include "embed.h"
#include "json.h"
#include "launch.h"
#include "pipe.h"
#include "profile_gen.h"
#include "uia.h"
#include "window.h"

namespace {

constexpr const char* kVersion = "0.12.0";
constexpr const char* kDefaultId = "default";

void print_usage() {
  std::printf(
      "ghost %s - a fingerprint browser for agents\n"
      "\n"
      "usage:\n"
      "  ghost browse [options] [url ...]      launch the browser under the shim\n"
      "  ghost serve [options] [url ...]       launch the browser and drive it over a pipe\n"
      "  ghost call [--pipe <name>] <json>     send one request to a running server\n"
      "  ghost run [options] -- <exe> [args]   run any program under the shim\n"
      "  ghost profile new [options]           create a profile\n"
      "  ghost profile show [options]          print a profile\n"
      "  ghost profile list                    list stored profiles\n"
      "  ghost selftest                        verify injection end to end\n"
      "  ghost doctor                          report what ghost can see here\n"
      "  ghost install [--uninstall]           put ghost on the user PATH\n"
      "  ghost version\n"
      "  ghost help\n"
      "\n"
      "options:\n"
      "  --id <name>          profile name (default: %s)\n"
      "  --profile <file>     use this file instead of the profile store\n"
      "  --seed <hex>         identity seed; the same seed always yields the same machine\n"
      "  --tz <IANA>          override the timezone, e.g. Europe/London\n"
      "  --locale <tag>       override the locale, e.g. en-GB (--lang is accepted too)\n"
      "  --browser <which>    chrome (default), edge, or a path to an executable\n"
      "  --chrome-arg <arg>   extra browser argument (repeatable)\n"
      "  --user-data-dir <d>  where the browser keeps cookies and storage\n"
      "  --pipe <name>        control pipe name (default: ghost-<profile id>)\n"
      "  --attach <pid>       drive an already-running browser instead of launching one\n"
      "  --sandbox            keep Chromium's sandbox; renderer surfaces stay real\n"
      "  --no-cdp             do not open a DevTools channel at all\n"
      "  --no-wait            return as soon as the browser is running\n"
      "  --allow-unspoofed    start the target even if injection fails\n"
      "  --force              overwrite an existing profile\n"
      "  --verbose            print each step\n"
      "\n"
      "Note on --sandbox: spoofing navigator.hardwareConcurrency and\n"
      "navigator.deviceMemory requires injecting the renderer, and Chromium's\n"
      "renderer sandbox refuses that. ghost therefore passes --no-sandbox by\n"
      "default. Pass --sandbox to keep the sandbox and accept that those two\n"
      "values, and the WebGL adapter, stay real.\n",
      kVersion, kDefaultId);
}

struct Options {
  std::string command;
  std::string sub;  // second word, for "profile new"
  std::vector<std::string> rest;
  std::string id = kDefaultId;
  std::string seed;
  std::string tz;
  std::string locale;
  std::string browser;
  std::string profile_file;
  std::string user_data_dir;
  std::vector<std::string> chrome_args;
  std::string pipe_name;
  std::string attach;
  bool verbose = false;
  bool wait = true;
  bool allow_unspoofed = false;
  bool sandbox = false;
  bool no_cdp = false;
  bool force = false;
  bool digits = false;
  bool subtree = false;
  int hold = 0;
};

bool has_prefix(const std::string& s, const char* prefix) {
  const size_t n = std::strlen(prefix);
  return s.size() >= n && s.compare(0, n, prefix) == 0;
}

// Returns the value of "--flag=value" or the next argv entry for "--flag value".
bool value_of(const std::vector<std::string>& args, size_t* i, const char* flag,
              std::string* out) {
  const std::string& a = args[*i];
  if (has_prefix(a, (std::string(flag) + "=").c_str())) {
    *out = a.substr(std::strlen(flag) + 1);
    return true;
  }
  if (a == flag) {
    if (*i + 1 >= args.size()) {
      std::fprintf(stderr, "ghost: %s needs a value\n", flag);
      std::exit(64);
    }
    *out = args[++(*i)];
    return true;
  }
  return false;
}

Options parse(const std::vector<std::string>& args) {
  Options o;
  bool literal = false;

  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (literal) {
      o.rest.push_back(a);
      continue;
    }
    if (a == "--") {
      literal = true;
      continue;
    }

    std::string v;
    if (value_of(args, &i, "--id", &v)) {
      o.id = v;
    } else if (value_of(args, &i, "--seed", &v)) {
      o.seed = v;
    } else if (value_of(args, &i, "--tz", &v)) {
      o.tz = v;
    } else if (value_of(args, &i, "--locale", &v) || value_of(args, &i, "--lang", &v)) {
      // --lang is the same switch under the name Chromium uses. It has to be
      // accepted because the launcher appends --lang=<tag> to every target's
      // command line, so anything launched under the shim receives one.
      o.locale = v;
    } else if (value_of(args, &i, "--browser", &v)) {
      o.browser = v;
    } else if (value_of(args, &i, "--profile", &v)) {
      o.profile_file = v;
    } else if (value_of(args, &i, "--user-data-dir", &v)) {
      o.user_data_dir = v;
    } else if (value_of(args, &i, "--chrome-arg", &v)) {
      o.chrome_args.push_back(v);
    } else if (value_of(args, &i, "--pipe", &v)) {
      o.pipe_name = v;
    } else if (value_of(args, &i, "--attach", &v)) {
      o.attach = v;
    } else if (a == "--verbose" || a == "-v") {
      o.verbose = true;
    } else if (a == "--no-wait") {
      o.wait = false;
    } else if (a == "--allow-unspoofed") {
      o.allow_unspoofed = true;
    } else if (a == "--sandbox") {
      o.sandbox = true;
    } else if (a == "--no-cdp") {
      o.no_cdp = true;
    } else if (a == "--force") {
      o.force = true;
    } else if (a == "--digits") {
      o.digits = true;
    } else if (a == "--subtree") {
      o.subtree = true;
    } else if (value_of(args, &i, "--hold", &v)) {
      o.hold = std::atoi(v.c_str());
    } else if (a == "--help" || a == "-h") {
      o.command = "help";
    } else if (a == "--version") {
      o.command = "version";
    } else if (has_prefix(a, "-")) {
      std::fprintf(stderr, "ghost: unknown option %s\n", a.c_str());
      std::exit(64);
    } else if (o.command.empty()) {
      o.command = a;
    } else if (o.sub.empty()) {
      o.sub = a;
    } else {
      o.rest.push_back(a);
    }
  }
  if (o.command.empty()) o.command = "help";
  return o;
}

// --- profile store -----------------------------------------------------------

std::string profiles_dir() { return ghost::join_path(ghost::cache_root(), "profiles"); }
std::string store_path(const std::string& id) {
  return ghost::join_path(profiles_dir(), id + ".json");
}

// Minimal field reader. The profiles this program writes are flat objects of
// strings, numbers and booleans, so a full JSON parser would be dead weight in
// an executable whose whole selling point is being one small file.
std::string json_string(const std::string& json, const std::string& key) {
  const std::string needle = "\"" + key + "\"";
  size_t p = json.find(needle);
  if (p == std::string::npos) return std::string();
  p = json.find(':', p + needle.size());
  if (p == std::string::npos) return std::string();
  p = json.find('"', p);
  if (p == std::string::npos) return std::string();
  const size_t end = json.find('"', p + 1);
  if (end == std::string::npos) return std::string();
  return json.substr(p + 1, end - p - 1);
}

std::string json_number(const std::string& json, const std::string& key) {
  const std::string needle = "\"" + key + "\"";
  size_t p = json.find(needle);
  if (p == std::string::npos) return std::string();
  p = json.find(':', p + needle.size());
  if (p == std::string::npos) return std::string();
  ++p;
  while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) ++p;
  const size_t start = p;
  while (p < json.size() &&
         (std::isdigit(static_cast<unsigned char>(json[p])) || json[p] == '.' ||
          json[p] == '-' || json[p] == '+' || json[p] == 'e' || json[p] == 'E')) {
    ++p;
  }
  return json.substr(start, p - start);
}

// Turn the CLI overrides into the HostZone the generator wants. An unknown
// --tz is fatal: the shim needs a Windows key it can resolve against the system
// timezone database, and silently falling back would produce a profile whose
// timezone_id and actual offsets disagree.
bool zone_for(const Options& o, ghost::HostZone* out) {
  *out = ghost::host_zone();
  if (o.tz.empty()) return true;

  const std::string key = ghost::windows_key_for_iana(o.tz);
  if (key.empty()) {
    std::fprintf(stderr,
                 "ghost: unknown --tz '%s'. Pass an IANA name from the table in\n"
                 "       native/ghost_cli/src/profile_gen.cpp, e.g. Europe/London.\n",
                 o.tz.c_str());
    return false;
  }
  bool found = false;
  out->windows_key = key;
  out->iana = o.tz;
  out->bias_minutes = ghost::bias_for_windows_key(key, &found);
  out->has_daylight = found;
  return true;
}

ghost::HostLocale locale_for(const Options& o) {
  ghost::HostLocale l = ghost::host_locale();
  if (!o.locale.empty()) {
    l.tag = o.locale;
    const uint32_t id = ghost::langid_for_locale_tag(o.locale);
    if (id != 0) l.langid = id;
  }
  return l;
}

// Load the profile, creating it when missing. `chrome_version` only affects the
// user agent, so a missing browser is not a reason to refuse to create one.
std::string resolve_profile(const Options& o, const std::string& chrome_version,
                            std::string* path_out) {
  const std::string path =
      o.profile_file.empty() ? store_path(o.id) : o.profile_file;
  if (path_out != nullptr) *path_out = path;

  std::string json = ghost::read_file(path);
  if (!json.empty() && !o.force) {
    if (!chrome_version.empty()) {
      const std::string refreshed = ghost::refresh_user_agent(json, chrome_version);
      if (refreshed != json) {
        ghost::write_file(path, refreshed);
        if (o.verbose) {
          std::printf("profile     : user agent refreshed to Chrome %s\n",
                      chrome_version.c_str());
        }
        json = refreshed;
      }
    }
    return json;
  }

  ghost::HostZone zone;
  if (!zone_for(o, &zone)) return std::string();

  const std::string seed = o.seed.empty() ? ghost::random_seed_hex() : o.seed;
  json = ghost::generate_profile_json(o.id, seed, zone, locale_for(o), chrome_version);

  ghost::make_dirs(profiles_dir());
  if (!ghost::write_file(path, json)) {
    std::fprintf(stderr, "ghost: cannot write %s\n", path.c_str());
    return std::string();
  }
  if (o.verbose || o.command != "browse") {
    std::printf("created profile %s (seed %s)\n", path.c_str(), seed.c_str());
  }
  return json;
}

// --- commands ----------------------------------------------------------------

int cmd_browse(const Options& o) {
  const ghost::BrowserInstall browser = ghost::find_browser(o.browser);
  if (browser.path.empty()) {
    std::fprintf(stderr,
                 "ghost: no Chrome or Edge found. Install one, or pass\n"
                 "       --browser <path to a Chromium executable>.\n");
    return 1;
  }

  std::string path;
  const std::string json = resolve_profile(o, browser.version, &path);
  if (json.empty()) return 1;

  const std::string shim = ghost::extract_resource(ghost::kResShimDll, "ghost_shim.dll");
  if (shim.empty()) {
    std::fprintf(stderr, "ghost: this executable has no embedded shim (bad build)\n");
    return 1;
  }

  // The timezone and locale the browser is told about must be the ones the
  // profile claims, not the host's, or the two halves of the identity disagree.
  const std::string tz = json_string(json, "timezone_id");
  const std::string lang = json_string(json, "locale");

  ghost::LaunchOptions lo;
  lo.shim_dll = shim;
  lo.profile_json = json;
  lo.timezone = tz;
  lo.language = lang;
  lo.exe = browser.path;
  lo.wait = o.wait;
  lo.verbose = o.verbose;
  lo.allow_unspoofed = o.allow_unspoofed;
  // The browser outlives us; never hand it the caller's stdout pipe.
  lo.forward_stdio = false;

  const std::string data_dir =
      o.user_data_dir.empty() ? ghost::join_path(profiles_dir(), o.id + ".data")
                              : o.user_data_dir;
  ghost::make_dirs(data_dir);

  lo.args = {
      "--user-data-dir=" + data_dir,
      "--no-first-run",
      "--no-default-browser-check",
  };
  if (!o.sandbox) {
    // The GPU process is reachable with only its sandbox off, which is what the
    // WebGL adapter needs. The renderer needs the whole sandbox off.
    lo.args.push_back("--disable-gpu-sandbox");
    lo.args.push_back("--no-sandbox");
    // Chromium puts an "unsupported command-line flag" infobar on screen for the
    // flags above. That banner is a visible automation tell in every screenshot
    // and it steals about 56 px of viewport, which makes the page's reported
    // innerHeight disagree with the profile. --test-type suppresses the banner;
    // measured directly, it takes the infobar from three accessibility nodes to
    // zero.
    lo.args.push_back("--test-type");
  }
  for (const std::string& a : o.chrome_args) lo.args.push_back(a);
  // The parser files the first positional argument as a possible subcommand, so
  // `ghost browse https://example.com` lands in `sub` and the rest in `rest`.
  // For browse they are all just pages to open.
  if (!o.sub.empty()) lo.args.push_back(o.sub);
  for (const std::string& a : o.rest) lo.args.push_back(a);

  if (o.verbose) {
    std::printf("browser     : %s %s\n", browser.name.c_str(), browser.version.c_str());
    std::printf("profile     : %s\n", path.c_str());
    std::printf("data dir    : %s\n", data_dir.c_str());
    std::printf("timezone    : %s\n", tz.c_str());
    std::printf("locale      : %s\n", lang.c_str());
    std::printf("sandbox     : %s\n", o.sandbox ? "on (renderer not spoofed)" : "off");
  }
  return ghost::launch_under_shim(lo);
}

int cmd_run(const Options& o) {
  // Same positional split as browse: the first argument may have been filed as a
  // subcommand.
  std::vector<std::string> targets;
  if (!o.sub.empty()) targets.push_back(o.sub);
  targets.insert(targets.end(), o.rest.begin(), o.rest.end());
  if (targets.empty()) {
    std::fprintf(stderr, "ghost: run needs a target, e.g. ghost run -- notepad.exe\n");
    return 64;
  }

  const std::string shim = ghost::extract_resource(ghost::kResShimDll, "ghost_shim.dll");
  if (shim.empty()) {
    std::fprintf(stderr, "ghost: this executable has no embedded shim (bad build)\n");
    return 1;
  }

  ghost::LaunchOptions lo;
  lo.shim_dll = shim;
  lo.exe = targets[0];
  lo.args.assign(targets.begin() + 1, targets.end());
  lo.wait = o.wait;
  lo.verbose = o.verbose;
  lo.allow_unspoofed = o.allow_unspoofed;

  if (!o.profile_file.empty()) {
    lo.profile_json = ghost::read_file(o.profile_file);
  } else if (o.force || !o.seed.empty()) {
    ghost::HostZone zone;
    if (!zone_for(o, &zone)) return 1;
    const std::string seed = o.seed.empty() ? ghost::random_seed_hex() : o.seed;
    lo.profile_json = ghost::generate_profile_json(o.id, seed, zone, locale_for(o), "");
  }
  if (!lo.profile_json.empty()) {
    lo.timezone = json_string(lo.profile_json, "timezone_id");
    lo.language = json_string(lo.profile_json, "locale");
  }
  return ghost::launch_under_shim(lo);
}

int cmd_profile(const Options& o) {
  const std::string sub = o.sub.empty() ? "show" : o.sub;

  if (sub == "list") {
    ghost::make_dirs(profiles_dir());
    const std::string pattern = ghost::join_path(profiles_dir(), "*.json");
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(ghost::widen(pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
      std::printf("no profiles in %s\n", profiles_dir().c_str());
      return 0;
    }
    int count = 0;
    do {
      std::printf("%s\n", ghost::narrow(fd.cFileName).c_str());
      ++count;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::printf("%d profile(s) in %s\n", count, profiles_dir().c_str());
    return 0;
  }

  if (sub == "show") {
    const std::string path = o.profile_file.empty() ? store_path(o.id) : o.profile_file;
    const std::string json = ghost::read_file(path);
    if (json.empty()) {
      std::fprintf(stderr, "ghost: no profile at %s (try: ghost profile new)\n",
                   path.c_str());
      return 1;
    }
    std::printf("%s\n", json.c_str());
    return 0;
  }

  if (sub == "new") {
    const ghost::BrowserInstall browser = ghost::find_browser(o.browser);
    const std::string path = o.profile_file.empty() ? store_path(o.id) : o.profile_file;
    if (!o.force && ghost::file_exists(path)) {
      std::fprintf(stderr, "ghost: %s exists; pass --force to overwrite\n", path.c_str());
      return 1;
    }
    const std::string json = resolve_profile(o, browser.version, nullptr);
    if (json.empty()) return 1;
    if (!o.verbose) std::printf("%s\n", path.c_str());
    return 0;
  }

  std::fprintf(stderr, "ghost: unknown profile subcommand '%s'\n", sub.c_str());
  return 64;
}

// A profile whose every value the host cannot possibly produce, so a passing
// self-test cannot be a coincidence.
const char* kSelfTestProfile = R"JSON({
  "enabled": true,
  "profile_id": "selftest",
  "profile_seed_hex": "5e1f7e5fselftest",
  "cpu_hardware_concurrency": 7,
  "memory_total_bytes": 12884901888,
  "screen_width": 1920,
  "screen_height": 1080,
  "screen_avail_width": 1920,
  "screen_avail_height": 1040,
  "device_pixel_ratio": 1.5,
  "color_depth": 24,
  "timezone_id": "Europe/London",
  "timezone_windows_key": "GMT Standard Time",
  "timezone_bias_minutes": 0,
  "locale": "en-GB",
  "locale_langid": 2057,
  "gpu_adapter_description": "NVIDIA GeForce RTX 3060",
  "gpu_adapter_vendor_id": 4318,
  "gpu_adapter_device_id": 9476,
  "gpu_adapter_video_memory": 12884901888,
  "window_width": 1280,
  "window_height": 720
})JSON";

int cmd_selftest(const Options& o) {
  const std::string shim = ghost::extract_resource(ghost::kResShimDll, "ghost_shim.dll");
  if (shim.empty()) {
    std::fprintf(stderr, "ghost: this executable is missing its embedded engine\n");
    return 1;
  }
  if (o.verbose) std::printf("shim        : %s\n", shim.c_str());

  wchar_t temp_dir[MAX_PATH] = {0};
  GetTempPathW(MAX_PATH, temp_dir);
  const std::string out =
      ghost::join_path(ghost::narrow(temp_dir), "ghost-selftest.txt");
  DeleteFileW(ghost::widen(out).c_str());

  ghost::LaunchOptions lo;
  lo.shim_dll = shim;
  lo.profile_json = kSelfTestProfile;
  lo.timezone = "Europe/London";
  lo.language = "en-GB";
  // The shipped binary is its own probe: re-running this executable with the
  // hidden __probe switch makes it print the same report probe.exe does. That
  // keeps the payload to a single extracted file and means the process being
  // tested is the one the user actually has.
  lo.exe = ghost::exe_path();
  lo.args.push_back("__probe");
  lo.capture_to = out;
  lo.wait = true;
  lo.verbose = o.verbose;
  lo.allow_unspoofed = true;  // the self-test reports failure itself

  const int code = ghost::launch_under_shim(lo);
  const std::string report = ghost::read_file(out);

  struct Expectation {
    const char* needle;
    const char* why;
  };
  // Values chosen so the host cannot satisfy them by accident.
  const Expectation expected[] = {
      {"ghost_shim.dll loaded   = yes", "the shim was not inside the target process"},
      {"GetActiveProcessorCount = 7", "GetActiveProcessorCount was not spoofed"},
      {"dwNumberOfProcessors    = 7", "GetSystemInfo was not spoofed"},
      {"ullTotalPhys            = 12884901888", "GlobalMemoryStatusEx was not spoofed"},
      {"GetSystemMetrics(CX)    = 2880", "the screen width was not spoofed"},
      {"GetSystemMetrics(CY)    = 1620", "the screen height was not spoofed"},
      {"GetDpiForMonitor        = 144 x 144", "the DPI was not spoofed"},
      {"GetUserDefaultLocaleName= en-GB", "the locale was not spoofed"},
  };

  int failed = 0;
  for (const Expectation& e : expected) {
    const bool ok = report.find(e.needle) != std::string::npos;
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", e.needle);
    if (!ok) {
      std::printf("       %s\n", e.why);
      ++failed;
    }
  }

  if (failed == 0 && report.find("Bias=0") == std::string::npos) {
    std::printf("[FAIL] the timezone bias was not spoofed\n");
    ++failed;
  } else if (failed == 0) {
    std::printf("[PASS] GetTimeZoneInformation Bias=0\n");
  }

  std::printf("\n%d/%zu checks passed (target exit %d)\n",
              static_cast<int>(sizeof(expected) / sizeof(expected[0])) + 1 - failed,
              sizeof(expected) / sizeof(expected[0]) + 1, code);
  if (failed != 0) {
    std::printf("\nfull probe output:\n%s\n", report.c_str());
  }
  DeleteFileW(ghost::widen(out).c_str());
  return failed == 0 ? 0 : 1;
}

int cmd_doctor(const Options& o) {
  (void)o;
  const ghost::BrowserInstall browser = ghost::find_browser({});
  const ghost::HostZone zone = ghost::host_zone();
  const ghost::HostLocale locale = ghost::host_locale();

  std::printf("ghost %s\n", kVersion);
  std::printf("exe             : %s\n", ghost::exe_path().c_str());
  std::printf("cache root      : %s\n", ghost::cache_root().c_str());
  std::printf("profiles        : %s\n", profiles_dir().c_str());
  std::printf("embedded shim   : %zu bytes\n",
              ghost::resource_bytes(ghost::kResShimDll).size());
  if (browser.path.empty()) {
    std::printf("browser         : none found\n");
  } else {
    std::printf("browser         : %s %s (%s)\n", browser.name.c_str(),
                browser.version.c_str(), browser.path.c_str());
  }
  std::printf("host timezone   : %s / %s (bias %d, dst %s)\n", zone.windows_key.c_str(),
              zone.iana.empty() ? "unmapped" : zone.iana.c_str(), zone.bias_minutes,
              zone.has_daylight ? "yes" : "no");
  std::printf("host locale     : %s (langid 0x%04X)\n", locale.tag.c_str(), locale.langid);

  HANDLE token = nullptr;
  if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    if (GetTokenInformation(token, TokenElevation, &elevation, size, &size)) {
      std::printf("elevated        : %s\n", elevation.TokenIsElevated ? "yes" : "no");
    }
    CloseHandle(token);
  }

  const char* note = browser.path.empty()
                         ? "Install Chrome or Edge, then run: ghost browse"
                         : "Ready. Try: ghost selftest";
  std::printf("\n%s\n", note);
  return browser.path.empty() ? 1 : 0;
}

// Hidden on purpose; not listed in usage(). This is the injection target for
// `ghost selftest`. It prints exactly what tests/probe prints, which lets the
// shipped executable verify itself without writing a second executable to disk.
int cmd_probe(const Options& o) {
  (void)o;
  ghost::probe_report();
  return 0;
}

// Hidden on purpose; not listed in usage(). This is how the audio tier is
// measured from a shell, and the only part of it that can be tested without a
// challenge on screen:
//
//   ghost __audio list          what a capture could listen to
//   ghost __audio sessions      who is playing, and how loudly, right now
//   ghost __audio 5             capture five seconds and report the level
//   ghost __audio 5 out.wav     ...and keep it
//   ghost __speech list         which languages this machine can transcribe
//   ghost __speech out.wav en   transcribe it, keeping only the digits
//   ghost __speech out.wav en --digits   ...listening for digits and nothing else
//   ghost __uia 12345           dump the accessibility tree of that process's window
//   ghost __uia "Example"       ...matched by a piece of the window title instead
//
// It reports the level rather than just "ok", because the interesting failure is
// not "the endpoint would not open" -- that comes back as an error -- but "it
// opened and heard nothing", which is what a machine with no speakers, a muted
// endpoint, or a session whose audio is not being rendered all look like.
int cmd_audio(const Options& o) {
  if (o.sub == "list") {
    const std::vector<ghost::AudioDevice> devices = ghost::list_render_devices();
    if (devices.empty()) {
      std::printf("no active audio output device\n");
      return 1;
    }
    for (const ghost::AudioDevice& device : devices) {
      char vol[48] = "";
      if (device.volume >= 0.0f) {
        std::snprintf(vol, sizeof(vol), "  %.0f%%%s", device.volume * 100.0f,
                      device.muted ? " muted" : "");
      }
      std::printf("%s %s%s\n", device.is_default ? "*" : " ", device.name.c_str(), vol);
    }
    return 0;
  }

  if (o.sub == "sessions") {
    // What the volume mixer would show: which processes hold a stream on the
    // default endpoint, and how loud each one is right now. A capture that came
    // back silent is ambiguous without this -- "the page played nothing" and
    // "nothing ever opened a stream" look identical in the samples.
    std::string error;
    const std::vector<ghost::AudioSession> sessions = ghost::list_audio_sessions(&error);
    if (sessions.empty()) {
      std::printf("no audio sessions%s%s\n", error.empty() ? "" : ": ",
                  error.c_str());
      return error.empty() ? 0 : 1;
    }
    static const char* kStates[] = {"inactive", "active", "expired"};
    for (const ghost::AudioSession& session : sessions) {
      const char* state = (session.state >= 0 && session.state <= 2)
                              ? kStates[session.state]
                              : "unknown";
      std::printf("pid %-8lu %-9s peak %.4f%s%s\n",
                  static_cast<unsigned long>(session.pid), state,
                  static_cast<double>(session.peak),
                  session.system_sounds ? "  system sounds" : "",
                  session.name.empty() ? "" : ("  " + session.name).c_str());
    }
    return 0;
  }

  double seconds = 5.0;
  if (!o.sub.empty()) seconds = std::atof(o.sub.c_str());
  if (seconds <= 0.0 || seconds > 600.0) {
    std::printf("ghost: capture length must be between 0 and 600 seconds\n");
    return 64;
  }
  const std::string out = o.rest.empty() ? std::string() : o.rest.front();

  std::printf("capturing %.1fs of loopback audio...\n", seconds);
  const ghost::CaptureResult capture =
      ghost::capture_loopback(seconds, std::string(), out);
  if (!capture.ok) {
    std::printf("capture failed: %s\n", capture.error.c_str());
    return 1;
  }
  std::printf("device    %s\n", ghost::describe_capture(capture).c_str());
  std::printf("frames    %llu\n", static_cast<unsigned long long>(capture.frames));
  std::printf("silent    %llu frames\n",
              static_cast<unsigned long long>(capture.silent_frames));
  std::printf("peak      %.6f\n", capture.peak);
  std::printf("rms       %.6f\n", capture.rms);
  if (!capture.wav_path.empty()) {
    std::printf("wav       %s\n", capture.wav_path.c_str());
  }
  return 0;
}

// Local transcription, so that "what did the challenge say" is answered by the
// machine's own speech engine rather than by a model shipped inside this exe.
// `list` exists because the interesting failure is not a bad transcript but no
// recognizer for the challenge's language at all, and that is worth seeing
// before blaming the audio.
int cmd_speech(const Options& o) {
  if (o.sub == "list") {
    const std::vector<ghost::Recognizer> recognizers = ghost::list_recognizers();
    if (recognizers.empty()) {
      std::printf("no speech recognizer is installed\n");
      return 1;
    }
    for (const ghost::Recognizer& r : recognizers) {
      std::printf("%s  %s  %s\n", r.id.c_str(),
                  r.culture.empty() ? "(unknown)" : r.culture.c_str(),
                  r.description.c_str());
    }
    return 0;
  }
  if (o.sub.empty()) {
    std::printf("ghost: __speech needs a wave file, or \"list\"\n");
    return 64;
  }
  const std::string language = o.rest.empty() ? std::string() : o.rest.front();
  const ghost::Transcript transcript =
      ghost::recognize_wav(o.sub, language, o.digits);
  if (!transcript.ok) {
    std::printf("speech failed: %s\n", transcript.error.c_str());
    return 1;
  }
  std::printf("recognizer %s\n", transcript.recognizer.c_str());
  std::printf("text       %s\n", transcript.text.c_str());
  std::printf("digits     %s\n", transcript.digits.c_str());
  std::printf("confidence %.4f\n", transcript.confidence);
  for (size_t i = 0; i < transcript.alternatives.size(); ++i) {
    std::printf("alt[%zu]     %s\n", i, transcript.alternatives[i].c_str());
  }
  return 0;
}

// A window, a depth, and nothing else.
//
// This exists because "the accessibility tree stops at the browser chrome" has
// two very different causes -- the page really is not exposed to UIA, or the
// walk that reads it is broken -- and from inside the control plane those look
// identical. Pointing the same walker at ANY window, including one this tool
// never launched and never injected, is what tells the two apart.
int cmd_uia(const Options& o) {
  // The arguments are positional, so they are separated from the switches before
  // anything reads them: a window subject, then an optional depth and cap.
  const bool subtree = o.subtree;

  // Held open before anything else, because the flag it sets is read by whatever
  // is starting up, not by whatever is already running. This is the experiment:
  // hold a client across a browser's startup and see whether the renderer's tree
  // appears where it otherwise never does.
  if (o.hold > 0) {
    bool listening = false;
    ghost::hold_accessibility_client(o.hold, &listening);
    std::printf("held      %ds (clients %s)\n", o.hold,
                listening ? "listening" : "not listening");
  }
  std::vector<std::string> positional;
  if (!o.sub.empty()) positional.push_back(o.sub);
  for (const std::string& arg : o.rest) positional.push_back(arg);

  HWND window = nullptr;
  const std::string subject = positional.empty() ? std::string() : positional[0];

  if (!subject.empty()) {
    const bool numeric = subject.find_first_not_of("0123456789") == std::string::npos;
    if (numeric) {
      window = ghost::main_window(
                   static_cast<DWORD>(std::strtoul(subject.c_str(), nullptr, 10)))
                   .handle;
    } else {
      struct Search {
        const std::string* needle;
        HWND found;
      } search{&subject, nullptr};
      EnumWindows(
          [](HWND hwnd, LPARAM param) -> BOOL {
            Search* s = reinterpret_cast<Search*>(param);
            char title[512] = "";
            GetWindowTextA(hwnd, title, sizeof(title));
            if (std::strstr(title, s->needle->c_str()) != nullptr) {
              s->found = hwnd;
              return FALSE;
            }
            return TRUE;
          },
          reinterpret_cast<LPARAM>(&search));
      window = search.found;
    }
  }

  if (window == nullptr) {
    std::printf("ghost: no window matched \"%s\"\n", subject.c_str());
    return 1;
  }

  const ghost::WindowInfo info = ghost::describe_window(window);
  std::printf("window    %p  pid %lu\n", static_cast<void*>(info.handle),
              static_cast<unsigned long>(info.pid));
  std::printf("title     %s\n", info.title.c_str());
  std::printf("class     %s\n", info.class_name.c_str());
  std::printf("client    %dx%d at %ld,%ld\n", info.client_width, info.client_height,
              info.client.left, info.client.top);

  // Chromium only builds the renderer's accessibility tree when it believes a
  // UI Automation client is listening, and that belief is a flag owned by
  // uiautomationcore rather than by Chromium. Printing it next to the walk is
  // what separates "the page is not exposed" from "the walk cannot see it".
  {
    using ListeningFn = BOOL(WINAPI*)();
    const HMODULE core = LoadLibraryW(L"uiautomationcore.dll");
    ListeningFn listening = core == nullptr
                                ? nullptr
                                : reinterpret_cast<ListeningFn>(
                                      GetProcAddress(core, "UiaClientsAreListening"));
    std::printf("clients   %s\n",
                listening == nullptr ? "unknown"
                                     : (listening() ? "listening" : "not listening"));
  }
  std::printf("foreground %p\n", static_cast<void*>(GetForegroundWindow()));

  // Asking is the trigger: Chromium builds the tree in response to the first
  // query, so a false here means it never answered at all.
  const bool primed = ghost::prime_accessibility(window, 8000);
  std::printf("primed    %s\n", primed ? "yes" : "no");

  int depth = 32;
  int max_nodes = 4000;
  if (positional.size() >= 2) depth = std::atoi(positional[1].c_str());
  if (positional.size() >= 3) max_nodes = std::atoi(positional[2].c_str());

  std::string error;
  const std::vector<ghost::Element> nodes =
      subtree ? ghost::dump_descendants(window, max_nodes, &error)
              : ghost::dump_tree(window, depth, max_nodes, true, &error);
  if (nodes.empty()) {
    std::printf("no elements%s%s\n", error.empty() ? "" : ": ", error.c_str());
    return 1;
  }
  for (const ghost::Element& element : nodes) {
    std::printf("%2d %-14s id=%-22s %s\n", element.depth, element.role.c_str(),
                element.automation_id.empty() ? "-" : element.automation_id.c_str(),
                element.name.c_str());
  }
  std::printf("%zu elements%s\n", nodes.size(), subtree ? " (one query, no walk)" : "");
  return 0;
}

// Adding the install directory to the user PATH is the closest thing to an
// installer that a portable executable can honestly offer.
int cmd_install(const Options& o) {
  const bool uninstall = !o.sub.empty() && o.sub == "uninstall";
  const std::string dir = ghost::join_path(ghost::cache_root(), "bin");
  const std::string target = ghost::join_path(dir, "ghost.exe");

  if (uninstall) {
    std::string path = ghost::env_var("PATH");
    const std::string needle = dir + ";";
    const size_t p = path.find(needle);
    if (p != std::string::npos) {
      path.erase(p, needle.size());
      SetEnvironmentVariableA("PATH", path.c_str());
      HKEY key = nullptr;
      if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_SET_VALUE, &key) ==
          ERROR_SUCCESS) {
        RegSetValueExW(key, L"Path", 0, REG_EXPAND_SZ,
                       reinterpret_cast<const BYTE*>(ghost::widen(path).c_str()),
                       static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
      }
    }
    std::printf("removed %s from PATH (open a new shell to see it)\n", dir.c_str());
    return 0;
  }

  if (!ghost::make_dirs(dir)) {
    std::fprintf(stderr, "ghost: cannot create %s\n", dir.c_str());
    return 1;
  }
  if (!CopyFileW(ghost::widen(ghost::exe_path()).c_str(), ghost::widen(target).c_str(),
                 FALSE)) {
    std::fprintf(stderr, "ghost: cannot copy to %s (%lu)\n", target.c_str(),
                 GetLastError());
    return 1;
  }

  std::string path = ghost::env_var("PATH");
  if (path.find(dir) == std::string::npos) {
    path = dir + ";" + path;
    SetEnvironmentVariableA("PATH", path.c_str());
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_SET_VALUE, &key) ==
        ERROR_SUCCESS) {
      const std::wstring wide = ghost::widen(path);
      RegSetValueExW(key, L"Path", 0, REG_EXPAND_SZ,
                     reinterpret_cast<const BYTE*>(wide.c_str()),
                     static_cast<DWORD>((wide.size() + 1) * sizeof(wchar_t)));
      RegCloseKey(key);
      DWORD_PTR result = 0;
      SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                          reinterpret_cast<LPARAM>(L"Environment"), SMTO_ABORTIFHUNG,
                          5000, &result);
    }
  }

  std::printf("installed %s\n", target.c_str());
  std::printf("run 'ghost' from a new shell\n");
  return 0;
}

int cmd_serve(const Options& o) {
  const ghost::BrowserInstall browser = ghost::find_browser(o.browser);

  std::string path;
  // Align the profile's user agent with the browser that is actually installed,
  // so a version bump does not leave the identity claiming an old engine.
  const std::string json = resolve_profile(o, browser.version, &path);
  if (json.empty()) return 1;

  ghost::ServeOptions so;
  so.pipe_name = o.pipe_name;
  so.profile_id = o.id;
  so.profile_json = json;
  so.timezone = json_string(json, "timezone_id");
  so.locale = json_string(json, "locale");
  so.browser = o.browser;
  so.data_dir = o.user_data_dir.empty() ? ghost::join_path(profiles_dir(), o.id + ".data")
                                        : o.user_data_dir;
  so.chrome_args = o.chrome_args;
  so.sandbox = o.sandbox;
  so.cdp = !o.no_cdp;
  so.verbose = o.verbose;

  // The first positional is a possible subcommand, so a URL lands in `sub`.
  if (!o.sub.empty()) so.urls.push_back(o.sub);
  for (const std::string& a : o.rest) so.urls.push_back(a);

  if (!o.attach.empty()) {
    so.attach_pid = static_cast<DWORD>(std::strtoul(o.attach.c_str(), nullptr, 10));
    if (so.attach_pid == 0) {
      std::fprintf(stderr, "ghost: --attach needs a process id\n");
      return 64;
    }
  }
  return ghost::run_serve(so);
}

int cmd_call(const Options& o) {
  // The same pipe the agent client uses, reachable by hand so the control plane
  // can be debugged without a client in the way.
  std::string request = o.sub;
  for (const std::string& a : o.rest) {
    if (!request.empty()) request += " ";
    request += a;
  }
  if (request.empty()) {
    std::fprintf(stderr, "ghost: call needs a JSON request, e.g. '{\"cmd\":\"status\"}'\n");
    return 64;
  }

  const std::string pipe =
      o.pipe_name.empty() ? ghost::default_pipe_name(o.id) : o.pipe_name;
  std::string response;
  std::string error;
  if (!ghost::call_pipe(pipe, request, &response, &error, 30000)) {
    std::fprintf(stderr, "ghost: %s\n", error.c_str());
    return 1;
  }
  std::printf("%s\n", response.c_str());
  return 0;
}

}  // namespace

int main() {
  int argc = 0;
  LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::vector<std::string> args;
  args.reserve(argc > 0 ? static_cast<size_t>(argc) - 1 : 0);
  for (int i = 1; i < argc; ++i) args.push_back(ghost::narrow(wide[i]));
  if (wide != nullptr) LocalFree(wide);

  const Options o = parse(args);

  if (o.command == "help") {
    print_usage();
    return 0;
  }
  if (o.command == "version") {
    std::printf("ghost %s\n", kVersion);
    return 0;
  }
  if (o.command == "browse") return cmd_browse(o);
  if (o.command == "serve") return cmd_serve(o);
  if (o.command == "call") return cmd_call(o);
  if (o.command == "run") return cmd_run(o);
  if (o.command == "profile") return cmd_profile(o);
  if (o.command == "selftest") return cmd_selftest(o);
  if (o.command == "doctor") return cmd_doctor(o);
  if (o.command == "install") return cmd_install(o);
  if (o.command == "__probe") return cmd_probe(o);
  if (o.command == "__audio") return cmd_audio(o);
  if (o.command == "__speech") return cmd_speech(o);
  if (o.command == "__uia") return cmd_uia(o);

  std::fprintf(stderr, "ghost: unknown command '%s'\n\n", o.command.c_str());
  print_usage();
  return 64;
}
