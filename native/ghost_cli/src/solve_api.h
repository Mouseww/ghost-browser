// The third tier: a paid solving service answers the challenge.
//
// This tier exists for one measured reason. Windows installs speech recognizers per
// language feature, and a machine can carry an English *voice* with no English
// *recognizer* -- this one does, so a reCAPTCHA audio challenge can be captured
// perfectly and still not be transcribed locally. When the local engine cannot hear
// the challenge, the only honest local answer is "no", and a service is what turns
// that answer into a solve.
//
// The user's own key is what makes the tier exist. Nothing is baked in, the tier is
// off by default, and an unconfigured machine reports that it is off rather than
// failing at something the user never asked for.
#pragma once

#include <string>

namespace ghost {

struct SolveApi {
  std::string provider;  // "2captcha"; empty means the tier is not configured
  std::string key;
  std::string base_url;  // https://2captcha.com unless overridden
};

// Where the key comes from, in order: an explicit key (the control plane's `key`
// argument), then GHOST_CAPTCHA_KEY, then the profile's `captcha_api_key`. The URL
// comes from GHOST_CAPTCHA_URL, so a reseller or a local stand-in can be pointed at.
SolveApi resolve_solve_api(const std::string& explicit_key = std::string(),
                           const std::string& profile_key = std::string());

// Sends the recording to the service and returns the digits it heard.
//
// Blocking, because a solve takes seconds and the service is polled until it answers.
// `language` is a hint ("en"); empty lets the service decide.
bool solve_audio_api(const SolveApi& api, const std::string& wav_path,
                     const std::string& language, std::string* digits,
                     std::string* error);

// Standard base64, no line breaks. Exposed because the request body needs it and
// there is no reason for a second copy of it to exist.
std::string base64_encode(const unsigned char* data, size_t size);

}  // namespace ghost
