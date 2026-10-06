// Profile generation.
//
// A fingerprint browser with one hard-coded profile is useless: the whole point
// is that each identity is unique. But uniqueness is not enough either -- every
// field has to agree with every other field that describes the same machine, or
// the profile is *more* detectable than no spoofing at all. So the generator
// draws from a table of presets that are internally consistent, and derives the
// choice from a seed so one identity is stable across runs.
#pragma once

#include <cstdint>
#include <string>

namespace ghost {

// The host's timezone. The shim resolves the Windows key against the system
// timezone database and copies the whole entry, so DST rules come along for
// free; the IANA name is what ICU reads from TZ.
struct HostZone {
  std::string windows_key;   // "GMT Standard Time"
  std::string iana;          // "Europe/London"
  int bias_minutes = 0;      // Win32 convention: UTC = local + Bias
  bool has_daylight = false;
};

struct HostLocale {
  std::string tag;       // "en-GB"
  uint32_t langid = 0;   // 2057
};

HostZone host_zone();
HostLocale host_locale();

// Look up the IANA name for a Windows timezone key; empty when unknown.
std::string iana_for_windows_key(const std::string& key);

// Reverse lookup, used when the caller passes --tz. Empty when unknown.
std::string windows_key_for_iana(const std::string& iana);

// Standard bias for a Windows timezone key. `found` reports whether the key is
// in the table; the Win32 convention is UTC = local + bias.
int bias_for_windows_key(const std::string& key, bool* found);

// Windows LCID for a BCP-47 tag; 0 when the tag is not in the table.
uint32_t langid_for_locale_tag(const std::string& tag);

// A fresh 16-hex-digit seed.
std::string random_seed_hex();

// Serialise a coherent profile. `chrome_version` is a full version string such
// as "154.0.8037.98"; only its major component reaches the user agent.
std::string generate_profile_json(const std::string& id, const std::string& seed_hex,
                                  const HostZone& zone, const HostLocale& locale,
                                  const std::string& chrome_version);

// Rewrite the Chrome major version inside an existing profile's user agent, so
// a profile generated before a browser update does not advertise a version that
// is no longer installed.
std::string refresh_user_agent(const std::string& json, const std::string& chrome_version);

}  // namespace ghost
