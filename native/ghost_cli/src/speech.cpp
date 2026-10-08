#include "speech.h"

#include <windows.h>

#include <sapi.h>

#include <cstdio>
#include <cstring>

// Deliberately no sphelper.h: that header pulls in ATL, and ATL is an optional
// Visual Studio component. Everything this file needs from it is four calls
// (SpEnumTokens, SpGetDescription, SpBindToFile, CSpEvent), each of which is a
// few lines of plain COM. Depending on an optional workload to transcribe audio
// would be a strange trade.
namespace ghost {
namespace {

class Apartment {
 public:
  Apartment() {
    const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    initialized_ = SUCCEEDED(hr);
    // RPC_E_CHANGED_MODE means somebody already picked an apartment; the calls
    // below work either way, but then this object must not uninitialize.
    ok_ = initialized_ || hr == RPC_E_CHANGED_MODE;
  }
  ~Apartment() {
    if (initialized_) ::CoUninitialize();
  }
  bool ok() const { return ok_; }

  Apartment(const Apartment&) = delete;
  Apartment& operator=(const Apartment&) = delete;

 private:
  bool initialized_ = false;
  bool ok_ = false;
};

std::wstring widen(const std::string& text) {
  if (text.empty()) return std::wstring();
  const int size = static_cast<int>(text.size());
  const int need = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), size, nullptr, 0);
  if (need <= 0) return std::wstring();
  std::wstring out(static_cast<size_t>(need), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), size, out.data(), need);
  return out;
}

std::string narrow(const wchar_t* text) {
  if (text == nullptr || *text == L'\0') return std::string();
  const int need =
      ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (need <= 1) return std::string();
  std::string out(static_cast<size_t>(need - 1), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), need, nullptr, nullptr);
  return out;
}

std::string lower(const std::string& text) {
  std::string out = text;
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

bool starts_with(const std::string& text, const std::string& prefix) {
  return text.size() >= prefix.size() &&
         text.compare(0, prefix.size(), prefix) == 0;
}

std::string hex_hr(HRESULT hr) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%08lX",
                static_cast<unsigned long>(static_cast<unsigned int>(hr)));
  return buf;
}

std::string join(const std::vector<std::string>& items) {  std::string out;
  for (size_t i = 0; i < items.size(); ++i) {
    if (i != 0) out += ", ";
    out += items[i];
  }
  return out;
}

// ISpObjectToken::GetStringValue hands back memory from the COM task allocator,
// not from new[].
std::string token_string(ISpObjectToken* token, const wchar_t* name) {
  LPWSTR value = nullptr;
  if (SUCCEEDED(token->GetStringValue(name, &value)) && value != nullptr) {
    const std::string out = narrow(value);
    ::CoTaskMemFree(value);
    return out;
  }
  return std::string();
}

// The "Language" attribute is a hexadecimal LCID ("804"), which is not how a
// caller thinks about a language. Turn it into a name like "zh-CN".
std::string culture_from_lcid(const std::string& hex) {
  if (hex.empty()) return std::string();
  const std::wstring wide = widen(hex);
  wchar_t* end = nullptr;
  const long value = ::wcstol(wide.c_str(), &end, 16);
  if (end == wide.c_str() || value <= 0) return std::string();
  wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
  if (::LCIDToLocaleName(static_cast<LCID>(value), name, LOCALE_NAME_MAX_LENGTH,
                         0) == 0) {
    return std::string();
  }
  return narrow(name);
}

std::string token_culture(ISpObjectToken* token) {
  // The language lives under the token's Attributes subkey, not at the top
  // level; asking for a bare "Language" silently returns nothing.
  std::string hex = token_string(token, L"Language");
  if (hex.empty()) {
    ISpDataKey* attributes = nullptr;
    if (SUCCEEDED(token->OpenKey(L"Attributes", &attributes)) &&
        attributes != nullptr) {
      LPWSTR value = nullptr;
      if (SUCCEEDED(attributes->GetStringValue(L"Language", &value)) &&
          value != nullptr) {
        hex = narrow(value);
        ::CoTaskMemFree(value);
      }
      attributes->Release();
    }
  }
  return culture_from_lcid(hex);
}

bool enum_recognizers(IEnumSpObjectTokens** out) {
  *out = nullptr;
  ISpObjectTokenCategory* category = nullptr;
  HRESULT hr = ::CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL,
                                  IID_ISpObjectTokenCategory,
                                  reinterpret_cast<void**>(&category));
  if (FAILED(hr) || category == nullptr) return false;
  hr = category->SetId(SPCAT_RECOGNIZERS, FALSE);
  if (SUCCEEDED(hr)) hr = category->EnumTokens(nullptr, nullptr, out);
  category->Release();
  return SUCCEEDED(hr) && *out != nullptr;
}

bool bind_to_file(const std::string& path, ISpStream** out) {
  *out = nullptr;
  const std::wstring wide = widen(path);
  if (wide.empty()) return false;
  HRESULT hr = ::CoCreateInstance(CLSID_SpStream, nullptr, CLSCTX_ALL,
                                  IID_ISpStream, reinterpret_cast<void**>(out));
  if (FAILED(hr) || *out == nullptr) return false;
  // A null format asks the stream to take the format from the file's own header,
  // which is what a .wav written by capture_loopback has.
  hr = (*out)->BindToFile(wide.c_str(), SPFM_OPEN_READONLY, nullptr, nullptr, 0);
  if (FAILED(hr)) {
    (*out)->Release();
    *out = nullptr;
    return false;
  }
  return true;
}

}  // namespace

std::vector<Recognizer> list_recognizers() {
  std::vector<Recognizer> out;
  Apartment apartment;
  if (!apartment.ok()) return out;

  IEnumSpObjectTokens* tokens = nullptr;
  if (!enum_recognizers(&tokens)) return out;

  ULONG count = 0;
  tokens->GetCount(&count);
  for (ULONG i = 0; i < count; ++i) {
    ISpObjectToken* token = nullptr;
    if (FAILED(tokens->Item(i, &token)) || token == nullptr) continue;

    Recognizer entry;
    LPWSTR id = nullptr;
    if (SUCCEEDED(token->GetId(&id)) && id != nullptr) {
      entry.id = narrow(id);
      ::CoTaskMemFree(id);
    }
    entry.description = token_string(token, L"");
    entry.culture = token_culture(token);
    out.push_back(entry);
    token->Release();
  }
  tokens->Release();
  return out;
}

std::string digits_from(const std::string& text) {
  static const char* kEnglish[10] = {"zero", "one",  "two",   "three", "four",
                                     "five", "six",  "seven", "eight", "nine"};
  // The CJK numerals a recognizer emits instead of "7", plus 〇 and 两.
  static const struct {
    unsigned codepoint;
    char digit;
  } kCjk[] = {{0x96F6, '0'}, {0x3007, '0'}, {0x4E00, '1'}, {0x4E8C, '2'},
              {0x4E24, '2'}, {0x4E09, '3'}, {0x56DB, '4'}, {0x4E94, '5'},
              {0x516D, '6'}, {0x4E03, '7'}, {0x516B, '8'}, {0x4E5D, '9'}};

  std::string out;
  const size_t size = text.size();
  for (size_t i = 0; i < size;) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c >= '0' && c <= '9') {
      out.push_back(static_cast<char>(c));
      ++i;
      continue;
    }
    if ((c | 0x20u) >= 'a' && (c | 0x20u) <= 'z') {
      std::string word;
      size_t j = i;
      while (j < size) {
        const unsigned char k = static_cast<unsigned char>(text[j]);
        if ((k | 0x20u) < 'a' || (k | 0x20u) > 'z') break;
        word.push_back(static_cast<char>(k | 0x20u));
        ++j;
      }
      for (int digit = 0; digit < 10; ++digit) {
        if (word == kEnglish[digit]) {
          out.push_back(static_cast<char>('0' + digit));
          break;
        }
      }
      i = j;
      continue;
    }
    unsigned codepoint = c;
    size_t length = 1;
    if ((c & 0xE0u) == 0xC0u) {
      codepoint = c & 0x1Fu;
      length = 2;
    } else if ((c & 0xF0u) == 0xE0u) {
      codepoint = c & 0x0Fu;
      length = 3;
    } else if ((c & 0xF8u) == 0xF0u) {
      codepoint = c & 0x07u;
      length = 4;
    }
    for (size_t k = 1; k < length && i + k < size; ++k) {
      codepoint =
          (codepoint << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3Fu);
    }
    for (const auto& entry : kCjk) {
      if (entry.codepoint == codepoint) {
        out.push_back(entry.digit);
        break;
      }
    }
    i += length;
  }
  return out;
}

// An SRGS grammar that admits nothing but a run of digits. Dictation has to
// choose between every word in the language; this has to choose between ten, so
// it cannot answer "but 719" to a challenge that said "three seven one nine".
//
// The words have to be the recognizer's own. A grammar's items are matched
// against what the engine emits, and an English engine emits "three", not
// "三" -- so a word list picked once, in Chinese, turns the digits-only grammar
// into a no-match machine on every non-Chinese machine, exactly where the audio
// route is most wanted. The list therefore follows the culture the recognizer
// reported; a culture nothing here knows gets the literal digits, which every
// engine can speak in some form and digits_from can always read back.
std::string digit_grammar_srgs(const std::string& culture) {
  const std::string lang = culture.empty() ? std::string("zh-CN") : culture;
  const std::string prefix = lower(culture);
  std::vector<std::string> words;
  bool en = starts_with(prefix, "en");
  bool zh = starts_with(prefix, "zh");
  if (!en && !zh) {
    // Unknown or missing culture: digits are the only list that is honest for
    // every engine, and recognize_wav still maps them back through digits_from.
    for (char c = '0'; c <= '9'; ++c) words.emplace_back(1, c);
  } else if (en) {
    words = {"zero", "one", "two", "three", "four",
             "five", "six", "seven", "eight", "nine"};
  } else {
    // zh. 两 is kept beside 二: recognizers emit either for a spoken "2".
    words = {"零", "一", "二", "两", "三", "四", "五", "六", "七", "八", "九"};
  }
  std::string items;
  for (const std::string& word : words) {
    items += "<item>";
    items += word;
    items += "</item>";
  }
  return "<grammar version=\"1.0\" xml:lang=\"" + lang +
         "\" root=\"code\" xmlns=\"http://www.w3.org/2001/06/grammar\""
         " tag-format=\"semantics/1.0\">"
         "<rule id=\"digit\" scope=\"private\"><one-of>" +
         items +
         "</one-of></rule>"
         "<rule id=\"code\" scope=\"public\">"
         "<item repeat=\"3-10\"><ruleref uri=\"#digit\"/></item>"
         "</rule></grammar>";
}

// SAPI loads XML grammars from a file, so the grammar goes through %TEMP% and is
// removed again. It is generated, not shipped: the digits it admits follow from
// the recognizer's culture.
bool load_digit_grammar(ISpRecoGrammar* grammar, const std::string& culture) {
  char name[64];
  std::snprintf(name, sizeof(name), "ghost-digits-%lu.xml",
                static_cast<unsigned long>(::GetCurrentProcessId()));
  std::string dir = ".";
  char temp[MAX_PATH] = {0};
  if (::GetTempPathA(MAX_PATH, temp) != 0) dir = temp;
  const std::string path = dir + name;

  const std::string srgs = digit_grammar_srgs(culture);
  HANDLE file = ::CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const BOOL wrote = ::WriteFile(file, srgs.data(),
                                 static_cast<DWORD>(srgs.size()), &written, nullptr);
  ::CloseHandle(file);
  if (!wrote || written != srgs.size()) {
    ::DeleteFileA(path.c_str());
    return false;
  }

  const std::wstring wide = widen(path);
  const HRESULT hr = grammar->LoadCmdFromFile(wide.c_str(), SPLO_STATIC);
  ::DeleteFileA(path.c_str());
  return SUCCEEDED(hr);
}

Transcript recognize_wav(const std::string& wav_path, const std::string& language,
                         bool digits_only) {
  Transcript out;
  Apartment apartment;
  if (!apartment.ok()) {
    out.error = "COM could not be initialized";
    return out;
  }

  IEnumSpObjectTokens* tokens = nullptr;
  if (!enum_recognizers(&tokens)) {
    out.error = "no speech recognizer is installed on this machine";
    return out;
  }
  ULONG count = 0;
  tokens->GetCount(&count);

  const std::string wanted = lower(language);
  std::vector<std::string> available;
  ISpObjectToken* chosen = nullptr;
  std::string chosen_culture;
  for (ULONG i = 0; i < count; ++i) {
    ISpObjectToken* token = nullptr;
    if (FAILED(tokens->Item(i, &token)) || token == nullptr) continue;
    const std::string culture = token_culture(token);
    available.push_back(culture.empty() ? std::string("(unknown)") : culture);
    if (chosen == nullptr &&
        (wanted.empty() || starts_with(lower(culture), wanted))) {
      chosen = token;  // kept; released after CreateInstance below
      chosen_culture = culture;
      continue;
    }
    token->Release();
  }
  tokens->Release();

  if (chosen == nullptr) {
    // The honest answer on a machine whose speech feature is not the language of
    // the challenge. The caller falls through to the solving-API tier.
    out.error = "no recognizer for \"" + language +
                "\" is installed (installed: " + join(available) + ")";
    return out;
  }

  // SAPI 5 separates the recognizer *object* from the *engine token*: the token
  // names an engine, and you hand that token to an in-process recognizer. Asking
  // the token to create itself fails here -- it comes back REGDB_E_CLASSNOTREG,
  // and creating its CLSID by hand comes back E_NOINTERFACE -- because the
  // desktop engine class registers itself as an engine, not as a recognizer.
  const std::string clsid_text = token_string(chosen, L"CLSID");
  ISpRecognizer* recognizer = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_SpInprocRecognizer, nullptr, CLSCTX_ALL,
                                IID_ISpRecognizer,
                                reinterpret_cast<void**>(&recognizer));
  if (SUCCEEDED(hr) && recognizer != nullptr) {
    hr = recognizer->SetRecognizer(chosen);
  }
  chosen->Release();
  if (FAILED(hr) || recognizer == nullptr) {
    out.error = "the recognizer could not be created (" + hex_hr(hr) +
                ", engine " + clsid_text + ")";
    return out;
  }

  ISpStream* stream = nullptr;
  if (!bind_to_file(wav_path, &stream)) {
    recognizer->Release();
    out.error = "the wave file could not be opened: " + wav_path;
    return out;
  }

  // Order matters: the recognizer has to be idle before its input is swapped.
  recognizer->SetRecoState(SPRST_INACTIVE);
  hr = recognizer->SetInput(stream, TRUE);
  if (SUCCEEDED(hr)) hr = recognizer->SetRecoState(SPRST_ACTIVE);
  if (FAILED(hr)) {
    stream->Release();
    recognizer->Release();
    out.error = "the recognizer refused the wave file";
    return out;
  }

  ISpRecoContext* context = nullptr;
  hr = recognizer->CreateRecoContext(&context);
  if (FAILED(hr) || context == nullptr) {
    recognizer->SetRecoState(SPRST_INACTIVE);
    stream->Release();
    recognizer->Release();
    out.error = "the recognizer could not create a context";
    return out;
  }

  context->SetNotifyWin32Event();
  const ULONGLONG interest = SPFEI(SPEI_RECOGNITION) | SPFEI(SPEI_END_SR_STREAM);
  context->SetInterest(interest, interest);

  ISpRecoGrammar* grammar = nullptr;
  hr = context->CreateGrammar(1, &grammar);
  if (SUCCEEDED(hr) && grammar != nullptr) {
    const bool loaded = digits_only ? load_digit_grammar(grammar, chosen_culture)
                                    : SUCCEEDED(grammar->LoadDictation(nullptr, SPLO_STATIC));
    if (!loaded) {
      grammar->Release();
      grammar = nullptr;
    }
  }
  if (grammar == nullptr) {
    context->Release();
    recognizer->SetRecoState(SPRST_INACTIVE);
    stream->Release();
    recognizer->Release();
    out.error = digits_only ? "the digits-only grammar could not be loaded"
                            : "the dictation grammar could not be loaded";
    return out;
  }
  if (digits_only) {
    grammar->SetRuleState(nullptr, nullptr, SPRS_ACTIVE);
  } else {
    grammar->SetDictationState(SPRS_ACTIVE);
  }

  std::string text;
  double confidence = 0.0;
  std::vector<std::string> alternatives;
  bool ended = false;
  const DWORD deadline = ::GetTickCount() + 30000;
  while (!ended && static_cast<long>(::GetTickCount() - deadline) < 0) {
    if (context->WaitForNotifyEvent(500) != S_OK) continue;
    SPEVENT event;
    ULONG fetched = 0;
    std::memset(&event, 0, sizeof(event));
    while (context->GetEvents(1, &event, &fetched) == S_OK && fetched == 1) {
      if (event.eEventId == SPEI_RECOGNITION) {
        ISpRecoResult* result = reinterpret_cast<ISpRecoResult*>(event.lParam);
        if (result != nullptr) {
          LPWSTR phrase_text = nullptr;
          if (SUCCEEDED(result->GetText(SP_GETWHOLEPHRASE, SP_GETWHOLEPHRASE, TRUE,
                                        &phrase_text, nullptr)) &&
              phrase_text != nullptr) {
            text = narrow(phrase_text);
            ::CoTaskMemFree(phrase_text);
          }
          ULONG elements = 1;
          SPPHRASE* phrase = nullptr;
          if (SUCCEEDED(result->GetPhrase(&phrase)) && phrase != nullptr) {
            // SPPHRASE has no element count of its own; its top-level rule
            // carries both the count and the engine's confidence.
            elements =
                phrase->Rule.ulCountOfElements == 0 ? 1 : phrase->Rule.ulCountOfElements;
            if (phrase->pElements != nullptr && phrase->Rule.ulCountOfElements > 0) {
              confidence = phrase->pElements[0].SREngineConfidence;
            }
            ::CoTaskMemFree(phrase);
          }
          ISpPhraseAlt* alts[8] = {};
          ULONG got = 0;
          if (SUCCEEDED(result->GetAlternates(0, elements, 8, alts, &got))) {
            for (ULONG a = 0; a < got && a < 8; ++a) {
              if (alts[a] == nullptr) continue;
              LPWSTR alt_text = nullptr;
              if (SUCCEEDED(alts[a]->GetText(SP_GETWHOLEPHRASE, SP_GETWHOLEPHRASE,
                                             TRUE, &alt_text, nullptr)) &&
                  alt_text != nullptr) {
                alternatives.push_back(narrow(alt_text));
                ::CoTaskMemFree(alt_text);
              }
              alts[a]->Release();
            }
          }
          // SAPI hands the result over with a reference the caller owns; not
          // releasing it would leak one result per recognised phrase.
          result->Release();
        }
      } else if (event.eEventId == SPEI_END_SR_STREAM) {
        ended = true;
      }
      std::memset(&event, 0, sizeof(event));
    }
  }

  grammar->SetDictationState(SPRS_INACTIVE);
  grammar->Release();
  context->Release();
  recognizer->SetRecoState(SPRST_INACTIVE);
  stream->Release();
  recognizer->Release();

  out.ok = true;
  out.text = text;
  out.digits = digits_from(text);
  for (const std::string& alternative : alternatives) {
    if (!out.digits.empty()) break;
    out.digits = digits_from(alternative);
  }
  out.confidence = confidence;
  out.recognizer = chosen_culture;
  out.alternatives = alternatives;
  return out;
}

}  // namespace ghost
