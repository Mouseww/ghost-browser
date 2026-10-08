#include "solve_api.h"

#include <windows.h>
#include <winhttp.h>

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

#include "embed.h"
#include "speech.h"

namespace ghost {
namespace {

std::string url_encode(const std::string& text) {
  static const char* kHex = "0123456789ABCDEF";
  std::string out;
  out.reserve(text.size());
  for (const unsigned char c : text) {
    if (std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0x0F]);
    }
  }
  return out;
}

// One blocking HTTPS request.
//
// WinHTTP rather than WinINet: this runs inside a process that may be impersonating
// and holding a restricted token, where WinINet's per-user session state is exactly
// the kind of thing that fails quietly.
bool http_request(const std::string& method, const std::string& url,
                  const std::string& body, const std::string& content_type,
                  std::string* response, std::string* error) {
  const std::wstring wide_url = widen(url);
  URL_COMPONENTS parts;
  std::memset(&parts, 0, sizeof(parts));
  parts.dwStructSize = sizeof(parts);
  parts.dwSchemeLength = static_cast<DWORD>(-1);
  parts.dwHostNameLength = static_cast<DWORD>(-1);
  parts.dwUrlPathLength = static_cast<DWORD>(-1);
  parts.dwExtraInfoLength = static_cast<DWORD>(-1);
  if (::WinHttpCrackUrl(wide_url.c_str(), 0, 0, &parts) == FALSE) {
    if (error != nullptr) *error = "the service URL could not be parsed: " + url;
    return false;
  }
  const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
  std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
  if (parts.dwExtraInfoLength > 0) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);

  HINTERNET session = ::WinHttpOpen(L"ghost", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (session == nullptr) {
    if (error != nullptr) *error = "WinHttpOpen failed";
    return false;
  }
  ::WinHttpSetTimeouts(session, 15000, 15000, 60000, 60000);

  HINTERNET connect = ::WinHttpConnect(session, host.c_str(), parts.nPort, 0);
  if (connect == nullptr) {
    ::WinHttpCloseHandle(session);
    if (error != nullptr) *error = "could not reach " + narrow(host);
    return false;
  }

  const std::wstring wide_method = widen(method);
  const DWORD flags =
      parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
  HINTERNET request =
      ::WinHttpOpenRequest(connect, wide_method.c_str(), path.c_str(), nullptr,
                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
  if (request == nullptr) {
    ::WinHttpCloseHandle(connect);
    ::WinHttpCloseHandle(session);
    if (error != nullptr) *error = "could not open the request";
    return false;
  }

  std::wstring headers;
  if (!content_type.empty()) {
    headers = L"Content-Type: " + widen(content_type) + L"\r\n";
  }
  const BOOL sent = ::WinHttpSendRequest(
      request, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
      headers.empty() ? 0 : static_cast<DWORD>(-1L),
      body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
      static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);

  bool ok = false;
  std::string text;
  DWORD status = 0;
  if (sent != FALSE && ::WinHttpReceiveResponse(request, nullptr) != FALSE) {
    DWORD length = sizeof(status);
    ::WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &status, &length,
                          WINHTTP_NO_HEADER_INDEX);
    for (;;) {
      DWORD available = 0;
      if (::WinHttpQueryDataAvailable(request, &available) == FALSE || available == 0) {
        break;
      }
      std::string chunk(available, '\0');
      DWORD read = 0;
      if (::WinHttpReadData(request, chunk.data(), available, &read) == FALSE) break;
      chunk.resize(read);
      text += chunk;
      if (text.size() > 65536) break;  // a solving service never answers with more
    }
    ok = status >= 200 && status < 300;
    if (!ok && error != nullptr) {
      *error = "the service answered HTTP " + std::to_string(status) + ": " + text;
    }
  } else if (error != nullptr) {
    *error = "the request to " + narrow(host) + " did not complete";
  }

  ::WinHttpCloseHandle(request);
  ::WinHttpCloseHandle(connect);
  ::WinHttpCloseHandle(session);
  if (ok && response != nullptr) *response = text;
  return ok;
}

// Trims the whitespace a service may pad an answer with.
std::string trimmed(const std::string& text) {
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0) ++begin;
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) --end;
  return text.substr(begin, end - begin);
}

// Submits one job and polls until the service answers. Both tiers differ only in
// what they ask for and what they do with the reply, so the two calls that make
// up the protocol live here once.
bool submit_and_poll(const SolveApi& api, const std::string& body, std::string* answer,
                     std::string* error) {
  std::string reply;
  if (!http_request("POST", api.base_url + "/in.php", body,
                    "application/x-www-form-urlencoded", &reply, error)) {
    return false;
  }
  reply = trimmed(reply);
  if (reply.rfind("OK|", 0) != 0) {
    if (error != nullptr) *error = "the service refused the request: " + reply;
    return false;
  }
  const std::string id = reply.substr(3);

  // The service answers when it has an answer, and there is no way to ask sooner
  // that is not simply polling. Two minutes is generous for one challenge and
  // still short enough that a caller is not left hanging on a dead request.
  const ULONGLONG deadline = ::GetTickCount64() + 120000;
  while (::GetTickCount64() < deadline) {
    ::Sleep(5000);
    const std::string poll = api.base_url + "/res.php?key=" + url_encode(api.key) +
                             "&action=get&id=" + url_encode(id);
    std::string polled;
    if (!http_request("GET", poll, std::string(), std::string(), &polled, error)) {
      return false;
    }
    polled = trimmed(polled);
    if (polled.rfind("OK|", 0) == 0) {
      *answer = polled.substr(3);
      return true;
    }
    if (polled.find("CAPCHA_NOT_READY") != std::string::npos) continue;
    if (error != nullptr) *error = "the service failed: " + polled;
    return false;
  }
  if (error != nullptr) *error = "the service did not answer within two minutes";
  return false;
}

bool tier_configured(const SolveApi& api, std::string* error) {
  if (!api.provider.empty() && !api.key.empty()) return true;
  if (error != nullptr) {
    *error =
        "no solving service is configured: set GHOST_CAPTCHA_KEY, or add "
        "\"captcha_api_key\" to the profile";
  }
  return false;
}

}  // namespace

std::string base64_encode(const unsigned char* data, size_t size) {
  static const char* kAlphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((size + 2) / 3) * 4);
  size_t i = 0;
  for (; i + 2 < size; i += 3) {
    const uint32_t block = (static_cast<uint32_t>(data[i]) << 16) |
                           (static_cast<uint32_t>(data[i + 1]) << 8) |
                           static_cast<uint32_t>(data[i + 2]);
    out.push_back(kAlphabet[(block >> 18) & 0x3F]);
    out.push_back(kAlphabet[(block >> 12) & 0x3F]);
    out.push_back(kAlphabet[(block >> 6) & 0x3F]);
    out.push_back(kAlphabet[block & 0x3F]);
  }
  if (i + 1 == size) {
    const uint32_t block = static_cast<uint32_t>(data[i]) << 16;
    out.push_back(kAlphabet[(block >> 18) & 0x3F]);
    out.push_back(kAlphabet[(block >> 12) & 0x3F]);
    out.push_back('=');
    out.push_back('=');
  } else if (i + 2 == size) {
    const uint32_t block = (static_cast<uint32_t>(data[i]) << 16) |
                           (static_cast<uint32_t>(data[i + 1]) << 8);
    out.push_back(kAlphabet[(block >> 18) & 0x3F]);
    out.push_back(kAlphabet[(block >> 12) & 0x3F]);
    out.push_back(kAlphabet[(block >> 6) & 0x3F]);
    out.push_back('=');
  }
  return out;
}

SolveApi resolve_solve_api(const std::string& explicit_key,
                           const std::string& profile_key) {
  SolveApi api;
  api.base_url = "https://2captcha.com";
  if (const char* url = std::getenv("GHOST_CAPTCHA_URL")) {
    if (*url != '\0') api.base_url = url;
  }
  if (!explicit_key.empty()) {
    api.key = explicit_key;
  } else if (const char* env = std::getenv("GHOST_CAPTCHA_KEY")) {
    if (*env != '\0') api.key = env;
  } else {
    api.key = profile_key;
  }
  // A key is the whole tier. Without one the provider stays empty and every caller
  // can see that the tier is off rather than discovering it through a failed request.
  if (!api.key.empty()) api.provider = "2captcha";
  return api;
}

bool solve_audio_api(const SolveApi& api, const std::string& wav_path,
                     const std::string& language, std::string* digits,
                     std::string* error) {
  if (!tier_configured(api, error)) return false;
  const std::string audio = read_file(wav_path);
  if (audio.empty()) {
    if (error != nullptr) *error = "could not read the recording at " + wav_path;
    return false;
  }

  std::string body = "key=" + url_encode(api.key) + "&method=audio&body=" +
                     url_encode(base64_encode(
                         reinterpret_cast<const unsigned char*>(audio.data()),
                         audio.size()));
  if (!language.empty()) body += "&language=" + url_encode(language);

  std::string answer;
  if (!submit_and_poll(api, body, &answer, error)) return false;
  const std::string heard = digits_from(answer);
  if (heard.empty()) {
    if (error != nullptr) *error = "the service answered without digits: " + answer;
    return false;
  }
  if (digits != nullptr) *digits = heard;
  return true;
}

bool solve_recaptcha_api(const SolveApi& api, const std::string& site_key,
                         const std::string& page_url, std::string* token,
                         std::string* error) {
  if (!tier_configured(api, error)) return false;
  if (site_key.empty()) {
    if (error != nullptr) *error = "there is no site key to solve for";
    return false;
  }

  // The classic reCAPTCHA v2 job: the widget's key and the page it sits on are
  // the whole question, because the service reproduces the challenge itself
  // rather than being handed a picture of it.
  const std::string body = "key=" + url_encode(api.key) +
                           "&method=userrecaptcha&googlekey=" + url_encode(site_key) +
                           "&pageurl=" + url_encode(page_url);
  std::string answer;
  if (!submit_and_poll(api, body, &answer, error)) return false;
  if (answer.empty()) {
    if (error != nullptr) *error = "the service returned an empty token";
    return false;
  }
  if (token != nullptr) *token = answer;
  return true;
}

}  // namespace ghost
