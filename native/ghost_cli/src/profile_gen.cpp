// The tables here encode the coherence rules that make a profile survivable.
// Getting a single value "right" is easy; getting a machine's worth of values to
// agree with each other is the actual problem, so the presets are hand-checked:
//
//   * logical size x device pixel ratio lands on a panel that actually ships
//     (2560x1440 at 1.5 is a 4K display running at 150% scaling)
//   * the WebGL vendor/renderer strings, the adapter description and the PCI
//     device id all describe the same card, because ANGLE composes its renderer
//     string out of the DXGI adapter identity and appends "(0x<device id>)"
//   * the timezone's Windows key and its IANA name are the same zone, and the
//     stored bias is that zone's standard bias
#define _CRT_RAND_S
#include "profile_gen.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>

namespace ghost {
namespace {

std::wstring widen(const std::string& s) {
  if (s.empty()) return std::wstring();
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                    nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

std::string narrow(const std::wstring& w) {
  if (w.empty()) return std::string();
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                    nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n,
                      nullptr, nullptr);
  return s;
}

uint64_t fnv1a(const std::string& s) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return h;
}

struct ZoneEntry {
  const char* windows_key;
  const char* iana;
  int bias_minutes;
};

// Windows timezone key -> IANA name and standard bias. The key is what the shim
// hands to EnumDynamicTimeZoneInformation; the IANA name is what ICU reads.
const ZoneEntry kZones[] = {
    {"Dateline Standard Time", "Etc/GMT+12", 720},
    {"UTC-11", "Etc/GMT+11", 660},
    {"Hawaiian Standard Time", "Pacific/Honolulu", 600},
    {"Alaskan Standard Time", "America/Anchorage", 540},
    {"Pacific Standard Time", "America/Los_Angeles", 480},
    {"Mountain Standard Time", "America/Denver", 420},
    {"US Mountain Standard Time", "America/Phoenix", 420},
    {"Central Standard Time", "America/Chicago", 360},
    {"Eastern Standard Time", "America/New_York", 300},
    {"US Eastern Standard Time", "America/Indianapolis", 300},
    {"Atlantic Standard Time", "America/Halifax", 240},
    {"Newfoundland Standard Time", "America/St_Johns", 210},
    {"E. South America Standard Time", "America/Sao_Paulo", 180},
    {"Argentina Standard Time", "America/Argentina/Buenos_Aires", 180},
    {"Azores Standard Time", "Atlantic/Azores", 60},
    {"Cape Verde Standard Time", "Atlantic/Cape_Verde", 60},
    {"UTC", "Etc/UTC", 0},
    {"GMT Standard Time", "Europe/London", 0},
    {"Greenwich Standard Time", "Atlantic/Reykjavik", 0},
    {"W. Europe Standard Time", "Europe/Berlin", -60},
    {"Central Europe Standard Time", "Europe/Budapest", -60},
    {"Romance Standard Time", "Europe/Paris", -60},
    {"Central European Standard Time", "Europe/Warsaw", -60},
    {"W. Central Africa Standard Time", "Africa/Lagos", -60},
    {"E. Europe Standard Time", "Europe/Chisinau", -120},
    {"Egypt Standard Time", "Africa/Cairo", -120},
    {"South Africa Standard Time", "Africa/Johannesburg", -120},
    {"FLE Standard Time", "Europe/Kiev", -120},
    {"Israel Standard Time", "Asia/Jerusalem", -120},
    {"GTB Standard Time", "Europe/Bucharest", -120},
    {"Russian Standard Time", "Europe/Moscow", -180},
    {"Turkey Standard Time", "Europe/Istanbul", -180},
    {"Arab Standard Time", "Asia/Riyadh", -180},
    {"Arabic Standard Time", "Asia/Baghdad", -180},
    {"Georgian Standard Time", "Asia/Tbilisi", -240},
    {"Iran Standard Time", "Asia/Tehran", -210},
    {"Arabian Standard Time", "Asia/Dubai", -240},
    {"Afghanistan Standard Time", "Asia/Kabul", -270},
    {"West Asia Standard Time", "Asia/Tashkent", -300},
    {"Pakistan Standard Time", "Asia/Karachi", -300},
    {"India Standard Time", "Asia/Kolkata", -330},
    {"Sri Lanka Standard Time", "Asia/Colombo", -330},
    {"Nepal Standard Time", "Asia/Kathmandu", -345},
    {"Bangladesh Standard Time", "Asia/Dhaka", -360},
    {"Myanmar Standard Time", "Asia/Yangon", -390},
    {"SE Asia Standard Time", "Asia/Bangkok", -420},
    {"China Standard Time", "Asia/Shanghai", -480},
    {"Singapore Standard Time", "Asia/Singapore", -480},
    {"Taipei Standard Time", "Asia/Taipei", -480},
    {"W. Australia Standard Time", "Australia/Perth", -480},
    {"Tokyo Standard Time", "Asia/Tokyo", -540},
    {"Korea Standard Time", "Asia/Seoul", -540},
    {"Cen. Australia Standard Time", "Australia/Adelaide", -570},
    {"AUS Central Standard Time", "Australia/Darwin", -570},
    {"E. Australia Standard Time", "Australia/Brisbane", -600},
    {"AUS Eastern Standard Time", "Australia/Sydney", -600},
    {"Tasmania Standard Time", "Australia/Hobart", -600},
    {"Vladivostok Standard Time", "Asia/Vladivostok", -600},
    {"New Zealand Standard Time", "Pacific/Auckland", -720},
    {"Fiji Standard Time", "Pacific/Fiji", -720},
    {"Tonga Standard Time", "Pacific/Tongatapu", -780},
    {"Samoa Standard Time", "Pacific/Apia", -780},
};

struct LocaleEntry {
  const char* tag;
  uint32_t langid;
};

const LocaleEntry kLocales[] = {
    {"en-US", 0x0409}, {"en-GB", 0x0809}, {"en-CA", 0x1009}, {"en-AU", 0x0C09},
    {"de-DE", 0x0407}, {"fr-FR", 0x040C}, {"es-ES", 0x0C0A}, {"it-IT", 0x0410},
    {"nl-NL", 0x0413}, {"pt-BR", 0x0416}, {"ja-JP", 0x0411}, {"ko-KR", 0x0412},
    {"zh-CN", 0x0804}, {"zh-TW", 0x0404}, {"ru-RU", 0x0419}, {"pl-PL", 0x0415},
    {"sv-SE", 0x041D}, {"tr-TR", 0x041F},
};

struct MachinePreset {
  int cores;
  uint64_t memory_bytes;
  int logical_width;
  int logical_height;
  double dpr;
  const char* webgl_vendor;
  const char* webgl_renderer;
  const char* adapter_description;
  uint32_t adapter_vendor_id;
  uint32_t adapter_device_id;
  uint64_t adapter_video_memory;
};

const char* const kGlVersion = "OpenGL ES 3.0 (ANGLE 2.1.0)";

const MachinePreset kMachines[] = {
    {4, 8589934592ull, 1920, 1080, 1.0, "Google Inc. (NVIDIA)",
     "ANGLE (NVIDIA, NVIDIA GeForce GTX 1650 Direct3D11 vs_5_0 ps_5_0, D3D11)",
     "NVIDIA GeForce GTX 1650", 4318, 8066, 4294967296ull},
    {6, 17179869184ull, 1920, 1080, 1.0, "Google Inc. (NVIDIA)",
     "ANGLE (NVIDIA, NVIDIA GeForce GTX 1660 SUPER Direct3D11 vs_5_0 ps_5_0, D3D11)",
     "NVIDIA GeForce GTX 1660 SUPER", 4318, 8644, 6442450944ull},
    {6, 17179869184ull, 2560, 1440, 1.0, "Google Inc. (NVIDIA)",
     "ANGLE (NVIDIA, NVIDIA GeForce RTX 3060 Direct3D11 vs_5_0 ps_5_0, D3D11)",
     "NVIDIA GeForce RTX 3060", 4318, 9476, 12884901888ull},
    {8, 17179869184ull, 1920, 1080, 1.25, "Google Inc. (NVIDIA)",
     "ANGLE (NVIDIA, NVIDIA GeForce RTX 3060 Direct3D11 vs_5_0 ps_5_0, D3D11)",
     "NVIDIA GeForce RTX 3060", 4318, 9476, 12884901888ull},
    {8, 34359738368ull, 2560, 1440, 1.0, "Google Inc. (NVIDIA)",
     "ANGLE (NVIDIA, NVIDIA GeForce RTX 4060 Direct3D11 vs_5_0 ps_5_0, D3D11)",
     "NVIDIA GeForce RTX 4060", 4318, 10370, 8589934592ull},
    {12, 34359738368ull, 2560, 1440, 1.0, "Google Inc. (NVIDIA)",
     "ANGLE (NVIDIA, NVIDIA GeForce RTX 4070 Direct3D11 vs_5_0 ps_5_0, D3D11)",
     "NVIDIA GeForce RTX 4070", 4318, 10118, 12884901888ull},
    {12, 34359738368ull, 2560, 1440, 1.5, "Google Inc. (NVIDIA)",
     "ANGLE (NVIDIA, NVIDIA GeForce RTX 4070 Direct3D11 vs_5_0 ps_5_0, D3D11)",
     "NVIDIA GeForce RTX 4070", 4318, 10118, 12884901888ull},
    {16, 68719476736ull, 2560, 1440, 1.5, "Google Inc. (NVIDIA)",
     "ANGLE (NVIDIA, NVIDIA GeForce RTX 4080 Direct3D11 vs_5_0 ps_5_0, D3D11)",
     "NVIDIA GeForce RTX 4080", 4318, 9988, 17179869184ull},
    {8, 17179869184ull, 1920, 1200, 1.0, "Google Inc. (AMD)",
     "ANGLE (AMD, AMD Radeon RX 6600 Direct3D11 vs_5_0 ps_5_0, D3D11)",
     "AMD Radeon RX 6600", 4098, 29695, 8589934592ull},
    {6, 17179869184ull, 2560, 1440, 1.0, "Google Inc. (AMD)",
     "ANGLE (AMD, AMD Radeon RX 6700 XT Direct3D11 vs_5_0 ps_5_0, D3D11)",
     "AMD Radeon RX 6700 XT", 4098, 29663, 12884901888ull},
    {4, 8589934592ull, 1600, 900, 1.0, "Google Inc. (Intel)",
     "ANGLE (Intel, Intel(R) UHD Graphics 630 Direct3D11 vs_5_0 ps_5_0, D3D11)",
     "Intel(R) UHD Graphics 630", 32902, 16027, 1073741824ull},
    {8, 17179869184ull, 1920, 1080, 1.0, "Google Inc. (Intel)",
     "ANGLE (Intel, Intel(R) Iris(R) Xe Graphics Direct3D11 vs_5_0 ps_5_0, D3D11)",
     "Intel(R) Iris(R) Xe Graphics", 32902, 39497, 1073741824ull},
};

constexpr size_t kZoneCount = sizeof(kZones) / sizeof(kZones[0]);
constexpr size_t kLocaleCount = sizeof(kLocales) / sizeof(kLocales[0]);
constexpr size_t kMachineCount = sizeof(kMachines) / sizeof(kMachines[0]);

// The families a stock Windows 10/11 install ships, using the en-US names
// DirectWrite reports (a family's localized name, "宋体" for SimSun, is resolved
// by the shim before the allow-list is consulted). Anything outside this list is
// a fingerprint: a machine claiming a clean en-GB install that also has Office,
// a Chinese IME pack, or a developer's font collection is trivially singled out,
// and a real one measured here carried 266 families, not 80.
//
// The spoofed set is the intersection of this list with what the host has, so the
// shim can hide fonts but cannot invent them. That is a real limit: a profile
// cannot make a machine look like it has a font it lacks.
const char* const kWindowsFonts[] = {
    "Arial", "Arial Black", "Bahnschrift", "Calibri", "Calibri Light", "Cambria",
    "Cambria Math", "Candara", "Candara Light", "Comic Sans MS", "Consolas",
    "Constantia", "Corbel", "Corbel Light", "Courier New", "Ebrima",
    "Franklin Gothic Medium", "Gabriola", "Gadugi", "Georgia",
    "HoloLens MDL2 Assets", "Impact", "Ink Free", "Javanese Text", "Leelawadee UI",
    "Leelawadee UI Semilight", "Lucida Console", "Lucida Sans Unicode",
    "Malgun Gothic", "Marlett", "Microsoft Himalaya", "Microsoft JhengHei",
    "Microsoft JhengHei UI", "Microsoft JhengHei UI Light", "Microsoft New Tai Lue",
    "Microsoft PhagsPa", "Microsoft Sans Serif", "Microsoft Tai Le",
    "Microsoft Uighur", "Microsoft YaHei", "Microsoft YaHei UI",
    "Microsoft YaHei UI Light", "Microsoft Yi Baiti", "MingLiU-ExtB",
    "MingLiU_HKSCS-ExtB", "Mongolian Baiti", "MS Gothic", "MS PGothic",
    "MS UI Gothic", "MV Boli", "Myanmar Text", "Nirmala UI",
    "Nirmala UI Semilight", "Palatino Linotype", "Segoe MDL2 Assets", "Segoe Print",
    "Segoe Script", "Segoe UI", "Segoe UI Black", "Segoe UI Emoji",
    "Segoe UI Historic", "Segoe UI Light", "Segoe UI Semibold", "Segoe UI Semilight",
    "Segoe UI Symbol", "SimSun", "SimSun-ExtB", "SimSun-ExtG", "Sitka Banner",
    "Sitka Display", "Sitka Heading", "Sitka Small", "Sitka Subheading",
    "Sitka Text", "Sylfaen", "Symbol", "Tahoma", "Times New Roman", "Trebuchet MS",
    "Verdana", "Webdings", "Wingdings", "Wingdings 2", "Wingdings 3", "Yu Gothic",
    "Yu Gothic UI", "Yu Gothic UI Light", "Yu Gothic UI Semibold",
    "Yu Gothic UI Semilight",
};

constexpr size_t kFontCount = sizeof(kWindowsFonts) / sizeof(kWindowsFonts[0]);

}  // namespace

std::string iana_for_windows_key(const std::string& key) {
  for (const ZoneEntry& z : kZones) {
    if (key == z.windows_key) return z.iana;
  }
  return std::string();
}

std::string windows_key_for_iana(const std::string& iana) {
  for (const ZoneEntry& z : kZones) {
    if (iana == z.iana) return z.windows_key;
  }
  return std::string();
}

int bias_for_windows_key(const std::string& key, bool* found) {
  for (const ZoneEntry& z : kZones) {
    if (key == z.windows_key) {
      if (found) *found = true;
      return z.bias_minutes;
    }
  }
  if (found) *found = false;
  return 0;
}

uint32_t langid_for_locale_tag(const std::string& tag) {
  for (const LocaleEntry& l : kLocales) {
    if (tag == l.tag) return l.langid;
  }
  // Accept a bare language subtag, matching host_locale()'s fallback.
  for (const LocaleEntry& l : kLocales) {
    if (tag == std::string(l.tag).substr(0, 2)) return l.langid;
  }
  return 0;
}

HostZone host_zone() {
  DYNAMIC_TIME_ZONE_INFORMATION tz{};
  GetDynamicTimeZoneInformation(&tz);

  HostZone zone;
  zone.windows_key = narrow(tz.TimeZoneKeyName);
  zone.bias_minutes = static_cast<int>(tz.Bias);
  zone.has_daylight = tz.DaylightDate.wMonth != 0;
  zone.iana = iana_for_windows_key(zone.windows_key);
  return zone;
}

HostLocale host_locale() {
  wchar_t name[LOCALE_NAME_MAX_LENGTH] = {0};
  HostLocale locale;
  if (GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) > 0) {
    locale.tag = narrow(name);
  }
  locale.langid = static_cast<uint32_t>(GetUserDefaultLangID());

  // Fall back to the language subtag so the tag always has a region, which is
  // what navigator.language and Intl actually report.
  if (locale.tag.find('-') == std::string::npos) {
    for (const LocaleEntry& l : kLocales) {
      if (locale.tag == std::string(l.tag).substr(0, 2)) {
        locale.tag = l.tag;
        locale.langid = l.langid;
        break;
      }
    }
  }
  if (locale.tag.empty()) {
    locale.tag = "en-US";
    locale.langid = 0x0409;
  }
  return locale;
}

std::string random_seed_hex() {
  char buf[17];
  unsigned int a = 0, b = 0;
  rand_s(&a);
  rand_s(&b);
  const uint64_t v = (static_cast<uint64_t>(a) << 32) | b;
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
  return std::string(buf);
}

namespace {

// ANGLE composes the unmasked renderer string itself and includes the PCI
// device id. A profile that omits it documents a value the page will never see,
// so rebuild the string in the exact shape ANGLE emits.
std::string angle_renderer(const MachinePreset& m) {
  const std::string s = m.webgl_renderer;
  const size_t at = s.find(" Direct3D11");
  if (at == std::string::npos) return s;
  char id[16];
  std::snprintf(id, sizeof(id), " (0x%08X)", m.adapter_device_id);
  return s.substr(0, at) + id + s.substr(at);
}

}  // namespace

std::string generate_profile_json(const std::string& id, const std::string& seed_hex,
                                  const HostZone& zone, const HostLocale& locale,
                                  const std::string& chrome_version) {
  const MachinePreset& m = kMachines[fnv1a(seed_hex) % kMachineCount];
  const std::string renderer = angle_renderer(m);

  // The available desktop is the screen minus a taskbar. 40 px at 100% scaling
  // is the Windows 10/11 default.
  const int taskbar = static_cast<int>(40 * m.dpr + 0.5);
  const int avail_height = m.logical_height - taskbar;
  const int window_width = m.logical_width * 2 / 3;
  const int window_height = m.logical_height * 2 / 3;

  std::string major = chrome_version.substr(0, chrome_version.find('.'));
  if (major.empty()) major = "0";

  std::string fonts_json;
  for (size_t i = 0; i < kFontCount; ++i) {
    if (i != 0) fonts_json += ", ";
    fonts_json += '"';
    fonts_json += kWindowsFonts[i];
    fonts_json += '"';
  }

  char buf[8192];
  std::snprintf(
      buf, sizeof(buf),
      "{\n"
      "  \"enabled\": true,\n"
      "  \"profile_id\": \"%s\",\n"
      "  \"profile_seed_hex\": \"%s\",\n"
      "  \"cpu_hardware_concurrency\": %d,\n"
      "  \"memory_total_bytes\": %llu,\n"
      "  \"screen_width\": %d,\n"
      "  \"screen_height\": %d,\n"
      "  \"screen_avail_width\": %d,\n"
      "  \"screen_avail_height\": %d,\n"
      "  \"device_pixel_ratio\": %s,\n"
      "  \"color_depth\": 24,\n"
      "  \"timezone_id\": \"%s\",\n"
      "  \"timezone_windows_key\": \"%s\",\n"
      "  \"timezone_bias_minutes\": %d,\n"
      "  \"locale\": \"%s\",\n"
      "  \"locale_langid\": %u,\n"
      "  \"gpu_vendor\": \"%s\",\n"
      "  \"gpu_renderer\": \"%s\",\n"
      "  \"gpu_gl_version\": \"%s\",\n"
      "  \"gpu_max_texture_size\": 16384,\n"
      "  \"gpu_adapter_description\": \"%s\",\n"
      "  \"gpu_adapter_vendor_id\": %u,\n"
      "  \"gpu_adapter_device_id\": %u,\n"
      "  \"gpu_adapter_video_memory\": %llu,\n"
      "  \"user_agent\": \"Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
      "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/%s.0.0.0 Safari/537.36\",\n"
      "  \"platform\": \"Win32\",\n"
      "  \"window_width\": %d,\n"
      "  \"window_height\": %d,\n"
      "  \"fonts\": [%s]\n"
      "}\n",
      id.c_str(), seed_hex.c_str(), m.cores,
      static_cast<unsigned long long>(m.memory_bytes), m.logical_width, m.logical_height,
      m.logical_width, avail_height,
      // Device pixel ratios are printed with enough precision to round-trip.
      (m.dpr == 1.0 ? "1.0" : (m.dpr == 1.25 ? "1.25" : "1.5")), zone.iana.c_str(),
      zone.windows_key.c_str(), zone.bias_minutes, locale.tag.c_str(), locale.langid,
      m.webgl_vendor, renderer.c_str(), kGlVersion, m.adapter_description,
      m.adapter_vendor_id, m.adapter_device_id,
      static_cast<unsigned long long>(m.adapter_video_memory), major.c_str(), window_width,
      window_height, fonts_json.c_str());
  return std::string(buf);
}

std::string refresh_user_agent(const std::string& json, const std::string& chrome_version) {
  const size_t dot = chrome_version.find('.');
  const std::string major =
      dot == std::string::npos ? chrome_version : chrome_version.substr(0, dot);
  if (major.empty()) return json;

  const size_t key = json.find("\"user_agent\"");
  if (key == std::string::npos) return json;
  const size_t chrome = json.find("Chrome/", key);
  if (chrome == std::string::npos) return json;

  size_t end = chrome + 7;
  while (end < json.size() && json[end] >= '0' && json[end] <= '9') ++end;
  if (end == chrome + 7) return json;

  std::string out = json;
  out.replace(chrome + 7, end - (chrome + 7), major);
  return out;
}

}  // namespace ghost
