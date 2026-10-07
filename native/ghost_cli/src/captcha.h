// captcha.h — recognizing a human-verification challenge from outside the renderer.
//
// There is no CDP here and no script injection, so a challenge has to be
// recognized the way a person recognizes one: by looking. What makes that
// practical is a detail of Chrome's accessibility implementation — a frame's
// document node carries the frame's *URL* in its `value`. A challenge widget is
// an iframe, so its document node hands us the widget's identity directly:
//
//   hCaptcha    .../hcaptcha.html#frame=checkbox&...&sitekey=a5f74b19-...&hl=zh
//   reCAPTCHA   .../recaptcha/api2/anchor?...&k=6Le-wvkSAAAAAPBMRTvw0Q4Muexq9...&hl=zh-CN
//   reCAPTCHA   .../recaptcha/api2/bframe?...&bft=0dAFcWeA40KW7GRveS7EUwhQVZgAWq34...&ca=true
//
// That is the sitekey, the frame kind, and the challenge token, without opening a
// debugging port or evaluating a line of JavaScript. It is also what a solving
// service needs, which is why the third tier can be built on the same reading.
#pragma once

#include <string>
#include <vector>

#include "uia.h"

namespace ghost {

enum class CaptchaProvider { kNone, kHCaptcha, kReCaptcha, kTurnstile };

enum class CaptchaState {
  kAbsent,    // nothing on screen
  kCheckbox,  // the widget is showing its checkbox and has not been answered
  kVisual,    // an image challenge is up
  kAudio,     // the audio challenge is up
  kSolved,    // a challenge was seen earlier and is now gone
};

struct CaptchaInfo {
  CaptchaProvider provider = CaptchaProvider::kNone;
  CaptchaState state = CaptchaState::kAbsent;

  // The widget frame's URL, which is where everything below is parsed from.
  std::string frame_url;
  std::string site_key;         // hCaptcha `sitekey=`, reCAPTCHA `k=`
  std::string challenge_token;  // reCAPTCHA `bft=`
  std::string page_url;         // the top-level document's URL

  // Indices into the node vector that was analyzed, or -1. They are handed
  // straight back to the click resolver, which is what keeps this file free of
  // any opinion about how input is delivered.
  int checkbox_index = -1;
  int audio_button_index = -1;
  int verify_button_index = -1;
  int answer_field_index = -1;
  int refresh_button_index = -1;
  int play_button_index = -1;    // the audio challenge's own play control
  int accessibility_index = -1;  // hCaptcha's accessibility menu

  std::string detail;  // why the state was chosen, for the log
};

// Classifies a node vector as produced by dump_tree. Pure: it reads the tree and
// returns a verdict, so it can be tested without a browser.
CaptchaInfo analyze_captcha(const std::vector<Element>& nodes);

const char* captcha_provider_name(CaptchaProvider provider);
const char* captcha_state_name(CaptchaState state);

// True when the state means a person would still have to do something.
bool captcha_needs_action(const CaptchaInfo& info);

}  // namespace ghost
