// Local speech recognition, for the audio half of a human-verification
// challenge.
//
// The design goal is that no model ships inside ghost.exe. Windows already
// carries a speech engine -- the same one behind Voice Access and the old
// Windows Speech Recognition -- and SAPI 5 reaches it from an ordinary process
// with no installer and no service.
//
// What Windows does not always carry is a recognizer *for the language you
// need*: recognizers are installed per language feature, so an English audio
// challenge on a machine that only has the Chinese recognizer cannot be
// transcribed locally. recognize_wav() reports which recognizers do exist
// rather than failing silently, and the caller is expected to fall through to
// the solving-API tier when the answer is "none for this language".
#ifndef GHOST_CLI_SPEECH_H_
#define GHOST_CLI_SPEECH_H_

#include <string>
#include <vector>

namespace ghost {

struct Recognizer {
  std::string id;           // token id, e.g. "MS-2052-80-DESK"
  std::string culture;      // "zh-CN"; empty when the token does not say
  std::string description;  // human readable, localized by the engine
};

// Every recognizer the machine has, in token order. Empty is a real answer:
// it means no speech feature is installed at all.
std::vector<Recognizer> list_recognizers();

struct Transcript {
  bool ok = false;
  std::string error;
  std::string text;        // raw engine output
  std::string digits;      // 0-9 only, which is what a challenge wants
  double confidence = 0.0;
  std::string recognizer;  // the culture that produced the text
  std::vector<std::string> alternatives;
};

// Transcribe a 16-bit PCM WAV file. When `language` is non-empty only a
// recognizer whose culture starts with it is used ("en" matches "en-US"), and
// a machine without one yields ok=false with the installed list in `error`.
//
// `digits_only` loads a grammar that admits nothing but a run of digits instead
// of the dictation grammar. That is not a micro-optimisation: dictation spends
// its probability mass on words, and measured on this machine it recovers only
// about a quarter of the digits in a spoken code. A challenge answer that is
// merely close is worse than no answer, because typing it burns an attempt.
Transcript recognize_wav(const std::string& wav_path,
                         const std::string& language = {},
                         bool digits_only = false);

// Keep the digits a challenge would accept, mapping the number words and CJK
// numerals a recognizer may emit in place of "7".
std::string digits_from(const std::string& text);

}  // namespace ghost

#endif  // GHOST_CLI_SPEECH_H_
