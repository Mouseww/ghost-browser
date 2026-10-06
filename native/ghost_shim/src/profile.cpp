#include "ghost_profile.h"

#include <cstdlib>
#include <mutex>
#include <string>

#include "minijson.h"

namespace ghost {
namespace {

Profile load_from_env() {
  Profile p;

  const char* raw = std::getenv("GHOST_PROFILE_JSON");
  if (raw == nullptr || *raw == '\0') return p;

  mj::Value root;
  if (!mj::parse(std::string(raw), root) || !root.find("enabled")) return p;
  if (!root.find("enabled")->as_bool(false)) return p;

  auto num = [&root](const char* key, double fallback) -> double {
    const mj::Value* v = root.find(key);
    return v ? v->as_num(fallback) : fallback;
  };
  auto str = [&root](const char* key) -> std::string {
    const mj::Value* v = root.find(key);
    return v ? v->as_str() : std::string();
  };

  p.profile_id = str("profile_id");
  p.profile_seed_hex = str("profile_seed_hex");

  p.cpu.hardware_concurrency = static_cast<uint32_t>(num("cpu_hardware_concurrency", 0));
  p.memory.total_bytes = static_cast<uint64_t>(num("memory_total_bytes", 0));

  p.screen.width = static_cast<int32_t>(num("screen_width", 0));
  p.screen.height = static_cast<int32_t>(num("screen_height", 0));
  p.screen.avail_width = static_cast<int32_t>(num("screen_avail_width", 0));
  p.screen.avail_height = static_cast<int32_t>(num("screen_avail_height", 0));
  p.screen.device_pixel_ratio = num("device_pixel_ratio", 0.0);
  p.screen.color_depth = static_cast<int32_t>(num("color_depth", 24));

  p.time.timezone_id = str("timezone_id");
  p.time.timezone_windows_key = str("timezone_windows_key");
  p.time.bias_minutes = static_cast<int32_t>(num("timezone_bias_minutes", 0));
  // Presence, not value: bias 0 (UTC / London winter) is a real offset to spoof to.
  p.time.has_bias = root.find("timezone_bias_minutes") != nullptr;
  p.time.locale = str("locale");
  p.time.langid = static_cast<uint32_t>(num("locale_langid", 0));

  p.gpu.vendor = str("gpu_vendor");
  p.gpu.renderer = str("gpu_renderer");
  p.gpu.gl_version = str("gpu_gl_version");
  p.gpu.max_texture_size = static_cast<int32_t>(num("gpu_max_texture_size", 0));

  // The fields that actually work on current Chrome, where ANGLE is statically
  // linked into chrome.dll and there is no libGLESv2.dll export to intercept.
  p.gpu.adapter_description = str("gpu_adapter_description");
  p.gpu.adapter_vendor_id = static_cast<uint32_t>(num("gpu_adapter_vendor_id", 0));
  p.gpu.adapter_device_id = static_cast<uint32_t>(num("gpu_adapter_device_id", 0));
  p.gpu.adapter_video_memory = static_cast<uint64_t>(num("gpu_adapter_video_memory", 0));

  p.user_agent = str("user_agent");
  p.platform = str("platform");
  p.window_width = static_cast<int32_t>(num("window_width", 0));
  p.window_height = static_cast<int32_t>(num("window_height", 0));

  // The font allow-list. Presence of the key is what turns filtering on: an empty
  // array would mean "no fonts installed", which is never what anyone wants, so
  // an absent or empty list leaves DirectWrite untouched.
  if (const mj::Value* fonts = root.find("fonts"); fonts != nullptr &&
                                                   fonts->type == mj::Value::kArr &&
                                                   fonts->arr && !fonts->arr->empty()) {
    for (const mj::Value& item : *fonts->arr) {
      const std::string name = item.as_str();
      if (!name.empty()) p.fonts.families.push_back(name);
    }
    p.fonts.has_families = !p.fonts.families.empty();
  }

  // A profile with no spoofable surface at all is treated as disabled so the
  // shim does not install pointless hooks.
  const bool has_any = p.cpu.hardware_concurrency != 0 || p.memory.total_bytes != 0 ||
                       p.screen.width != 0 || p.gpu.renderer.size() != 0 ||
                       p.time.timezone_id.size() != 0 || p.fonts.has_families;
  p.enabled = has_any;
  return p;
}

}  // namespace

const Profile& Profile::current() {
  static std::once_flag once;
  static Profile cached;
  std::call_once(once, [] { cached = load_from_env(); });
  return cached;
}

}  // namespace ghost
