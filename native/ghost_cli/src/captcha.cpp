#include "captcha.h"

#include <cctype>
#include <cstddef>

namespace ghost {
namespace {

std::string lowered(const std::string& text) {
  std::string out = text;
  for (char& c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

bool contains(const std::string& haystack, const char* needle) {
  return haystack.find(needle) != std::string::npos;
}

bool starts_with(const std::string& text, const char* prefix) {
  return text.rfind(prefix, 0) == 0;
}

// Reads one parameter out of a URL, tolerating both the query string and the
// fragment. hCaptcha puts everything that matters in the fragment
// (`...hcaptcha.html#frame=checkbox&...&sitekey=...`), so a parser that only
// looked after '?' would find nothing.
//
// The boundary check is not decoration: reCAPTCHA uses the one-letter key `k`,
// and a naive search for "k=" matches inside "sitekey=" and "bft=" as well.
std::string param_value(const std::string& url, const std::string& key) {
  const std::string needle = key + "=";
  size_t at = 0;
  while ((at = url.find(needle, at)) != std::string::npos) {
    const bool at_boundary =
        at == 0 || url[at - 1] == '?' || url[at - 1] == '&' ||
        url[at - 1] == '#' || url[at - 1] == ';';
    if (at_boundary) {
      const size_t start = at + needle.size();
      const size_t end = url.find_first_of("&#", start);
      return url.substr(start, end == std::string::npos ? std::string::npos : end - start);
    }
    at += needle.size();
  }
  return std::string();
}

// The name a vendor gives its own frame, used only when the URL is unavailable.
// Turnstile is the case that matters: its widget document carries no URL, so the
// automation id is what identifies it.
bool name_says_cloudflare(const std::string& name) {
  const std::string n = lowered(name);
  return contains(n, "cloudflare") || contains(n, "安全质询") ||
         contains(n, "security challenge");
}

bool name_says_hcaptcha(const std::string& name) {
  const std::string n = lowered(name);
  return contains(n, "hcaptcha");
}

bool name_says_recaptcha(const std::string& name) {
  const std::string n = lowered(name);
  return contains(n, "recaptcha");
}

}  // namespace

const char* captcha_provider_name(CaptchaProvider provider) {
  switch (provider) {
    case CaptchaProvider::kHCaptcha: return "hcaptcha";
    case CaptchaProvider::kReCaptcha: return "recaptcha";
    case CaptchaProvider::kTurnstile: return "turnstile";
    default: return "none";
  }
}

const char* captcha_state_name(CaptchaState state) {
  switch (state) {
    case CaptchaState::kCheckbox: return "checkbox";
    case CaptchaState::kVisual: return "visual";
    case CaptchaState::kAudio: return "audio";
    case CaptchaState::kSolved: return "solved";
    default: return "absent";
  }
}

bool captcha_needs_action(const CaptchaInfo& info) {
  return info.state == CaptchaState::kCheckbox || info.state == CaptchaState::kVisual;
}

CaptchaInfo analyze_captcha(const std::vector<Element>& nodes) {
  CaptchaInfo info;

  // The page-level document is the shallowest one; a challenge always lives inside
  // it. Identifying it by depth rather than by "the first document node we happen to
  // see" matters here, because the demo pages sit on the vendor's own domain: the
  // hCaptcha demo is hosted at accounts.hcaptcha.com and the reCAPTCHA demo at
  // google.com/recaptcha/api2/demo. Both page URLs therefore contain the vendor's
  // domain, and a scan that does not exclude the page reports a widget that has not
  // loaded yet — which is exactly how an earlier version of this file reported
  // "hcaptcha, absent" on a page where the checkbox was still two seconds away.
  size_t page_index = nodes.size();
  int page_depth = 0;
  for (size_t i = 0; i < nodes.size(); ++i) {
    const Element& e = nodes[i];
    if (e.role != "document") continue;
    if (page_index == nodes.size() || e.depth < page_depth) {
      page_index = i;
      page_depth = e.depth;
    }
  }
  if (page_index < nodes.size()) info.page_url = nodes[page_index].value;

  // Every frame URL, not just the first one that names a vendor. A widget can have
  // several frames open at once — reCAPTCHA keeps its anchor frame while the
  // challenge frame appears beside it — and hCaptcha switches its single frame from
  // `frame=checkbox` to `frame=challenge`. Deciding the state from whichever frame
  // happened to come first reports an open challenge as unstarted, which is exactly
  // what an earlier version of this function did: the checkbox frame matched, the
  // loop stopped, and the challenge frame was never looked at.
  std::vector<std::string> frame_urls;
  for (size_t i = 0; i < nodes.size(); ++i) {
    if (i == page_index) continue;
    const Element& e = nodes[i];
    if (e.role != "document" && e.role != "group") continue;
    if (!e.value.empty()) frame_urls.push_back(e.value);
  }

  // Turnstile is matched first, and by name or automation id, because its widget
  // document carries no URL at all.
  for (size_t i = 0; i < nodes.size(); ++i) {
    if (info.provider != CaptchaProvider::kNone) break;
    if (i == page_index) continue;
    const Element& e = nodes[i];
    if (e.role != "document" && e.role != "group") continue;
    if (starts_with(e.automation_id, "cf-chl-widget") || name_says_cloudflare(e.name)) {
      info.provider = CaptchaProvider::kTurnstile;
      info.frame_url = e.value;
    }
  }

  // Then the vendor URL, which names the provider and carries the sitekey. Only
  // *frame* nodes are considered: a page that links to the vendor — every demo page
  // does — would otherwise match on some hyperlink's URL and hand back the page's own
  // address as the frame, which is a bug this loop had.
  for (const std::string& url : frame_urls) {
    if (info.provider != CaptchaProvider::kNone) break;
    const std::string lowered_url = lowered(url);
    if (contains(lowered_url, "hcaptcha.com")) {
      info.provider = CaptchaProvider::kHCaptcha;
      info.frame_url = url;
    } else if (contains(lowered_url, "recaptcha/api2/") ||
               contains(lowered_url, "recaptcha/enterprise")) {
      info.provider = CaptchaProvider::kReCaptcha;
      info.frame_url = url;
    }
  }

  if (info.provider == CaptchaProvider::kNone) {
    info.state = CaptchaState::kAbsent;
    info.detail = "no challenge widget in the tree";
    return info;
  }

  // The controls are recognized by automation id, which the vendors keep stable
  // across localizations: the labels here are Chinese, and matching on text would
  // have failed. That stability is the whole reason this works on any machine.
  // The id only fills in a provider that frame detection did not already establish,
  // so a stray element called "checkbox" cannot relabel a Cloudflare widget.
  for (const Element& e : nodes) {
    if (e.automation_id == "checkbox") {
      info.checkbox_index = e.index;
      if (info.provider == CaptchaProvider::kNone) info.provider = CaptchaProvider::kHCaptcha;
    } else if (e.automation_id == "recaptcha-anchor") {
      info.checkbox_index = e.index;
      if (info.provider == CaptchaProvider::kNone) info.provider = CaptchaProvider::kReCaptcha;
    } else if (e.automation_id == "recaptcha-audio-button") {
      info.audio_button_index = e.index;
    } else if (e.automation_id == "recaptcha-verify-button") {
      info.verify_button_index = e.index;
    } else if (e.automation_id == "recaptcha-reload-button") {
      info.refresh_button_index = e.index;
    } else if (e.automation_id == "audio-response") {
      info.answer_field_index = e.index;
    } else if (e.automation_id == "menu-info") {
      info.accessibility_index = e.index;
    }
  }

  // Turnstile's checkbox carries no id, so it is the only one matched by role
  // within an already-identified widget.
  if (info.provider == CaptchaProvider::kTurnstile && info.checkbox_index < 0) {
    for (const Element& e : nodes) {
      if (e.role == "checkbox") {
        info.checkbox_index = e.index;
        break;
      }
    }
  }

  // Read the sitekey and the challenge token from whichever frame carries them: for
  // reCAPTCHA the anchor has the sitekey and the challenge frame has the token, so
  // looking at one frame only would lose one of the two.
  for (const std::string& url : frame_urls) {
    if (info.site_key.empty()) {
      info.site_key = param_value(url, "sitekey");
      if (info.site_key.empty()) info.site_key = param_value(url, "k");
    }
    if (info.challenge_token.empty()) info.challenge_token = param_value(url, "bft");
  }

  // Whether the widget has escalated to an actual challenge. hCaptcha marks it in
  // the fragment (`frame=challenge`); reCAPTCHA opens a second frame (`bframe`).
  // Turnstile never does, which is why its widget document must not be mistaken for
  // a challenge frame — its checkbox lives inside that document.
  bool challenge_frame_open = false;
  for (const std::string& url : frame_urls) {
    const std::string frame = lowered(url);
    if (contains(frame, "frame=challenge") || contains(frame, "bframe")) {
      challenge_frame_open = true;
      break;
    }
  }

  // State, in the order a person would experience it. The order matters: after a
  // reCAPTCHA checkbox is clicked the anchor checkbox stays on screen while the
  // challenge opens beside it, so "a checkbox is present" cannot be checked first
  // without reporting every open challenge as unstarted.
  //
  // The audio test is the answer *field*, not the audio *button*. Every reCAPTCHA
  // challenge frame contains the button that switches to audio, so testing for the
  // button reports an audio challenge the moment the visual one opens — which is
  // precisely what it did before this line changed.
  if (info.answer_field_index >= 0) {
    info.state = CaptchaState::kAudio;
    info.detail = "an audio challenge is open";
  } else if (challenge_frame_open) {
    info.state = CaptchaState::kVisual;
    info.detail = "an image challenge is open";
  } else if (info.checkbox_index >= 0) {
    info.state = CaptchaState::kCheckbox;
    info.detail = "the widget is showing its checkbox";
  } else {
    info.state = CaptchaState::kAbsent;
    info.detail = "a widget is present but exposes no control";
  }

  // The audio challenge does not start itself.
  //
  // reCAPTCHA's audio frame carries its own play control, and until that is pressed
  // the audio element holds an open render session that produces nothing: measured on
  // this machine, the capture saw `active peak 0.0000` for the whole window while the
  // challenge sat waiting for a person. So the control has to be found -- and found
  // without reading its label, which is a localized sentence ("按“播放”可听语音内容" here,
  // "Press PLAY to listen" in English).
  //
  // It is identified structurally instead: inside the challenge frame it is the one
  // button that is not one of reCAPTCHA's own named controls. Those all carry a
  // `recaptcha-` automation id, which the vendor keeps stable across languages -- the
  // same reason every other control here is matched by id.
  //
  // This is reCAPTCHA-only on purpose. hCaptcha reaches its audio challenge through
  // the accessibility menu, which this file does not drive, so guessing at a button
  // in an hCaptcha frame could press something that is not a play control.
  if (info.provider == CaptchaProvider::kReCaptcha && info.state == CaptchaState::kAudio) {
    size_t frame_at = nodes.size();
    int frame_depth = 0;
    for (size_t i = 0; i < nodes.size(); ++i) {
      const Element& e = nodes[i];
      if (e.role != "document" && e.role != "group") continue;
      if (contains(lowered(e.value), "bframe")) {
        frame_at = i;
        frame_depth = e.depth;
        break;
      }
    }
    // A frame's subtree is contiguous in a depth-first walk, so the search stops at
    // the first node that is no deeper than the frame itself.
    for (size_t i = frame_at; i < nodes.size(); ++i) {
      const Element& e = nodes[i];
      if (i != frame_at && e.depth <= frame_depth) break;
      if (e.role != "button") continue;
      if (starts_with(e.automation_id, "recaptcha-")) continue;
      info.play_button_index = e.index;
      break;
    }
  }

  // hCaptcha answers a checkbox click with a picture grid, and its accessibility
  // menu is the documented way out of it. Recording the button's index here means
  // the caller never has to know that menu exists.
  if (info.provider == CaptchaProvider::kHCaptcha && info.accessibility_index >= 0 &&
      info.state == CaptchaState::kVisual) {
    info.detail = "hCaptcha image challenge; the accessibility menu is available";
  }

  return info;
}

}  // namespace ghost
