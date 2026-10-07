// WASAPI loopback capture.
//
// Why this file exists: a browser that is playing an audio challenge is pushing
// samples through the operating system's audio stack, and the operating system
// will hand those samples back to anyone who asks for the render endpoint's
// loopback. So the challenge audio can be obtained without extracting its URL,
// without reading the page, and without any of the debugging surfaces this
// browser exists to avoid. What arrives is not a guess about what the challenge
// meant to play -- it is what the machine actually played.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ghost {

struct AudioDevice {
  std::string id;
  std::string name;
  bool is_default = false;
  // Master volume and mute, because "the capture was silent" has an ordinary
  // explanation far more often than an interesting one.
  float volume = -1.0f;  // 0..1, or -1 when the endpoint would not say
  bool muted = false;
};

// Every render endpoint, so a caller can see what a capture would have listened
// to instead of being told "the default one".
std::vector<AudioDevice> list_render_devices();

// One process's audio session on the default render endpoint.
//
// This answers the question a loopback capture cannot: "did anything even try to
// play?" A capture that came back silent is ambiguous -- the page may have played
// nothing, or the browser may have opened no stream at all -- and the difference
// decides whether retrying is worth it. The peak here is the same number the
// Windows volume mixer animates.
struct AudioSession {
  uint32_t pid = 0;
  int state = 0;          // 0 inactive, 1 active, 2 expired (WASAPI's values)
  bool system_sounds = false;
  float peak = 0.0f;      // 0..1, instantaneous
  std::string name;       // session identifier, usually a file path
};

std::vector<AudioSession> list_audio_sessions(std::string* error = nullptr);

struct CaptureResult {
  bool ok = false;
  std::string error;
  std::string device_name;      // what was actually captured
  uint32_t sample_rate = 0;
  uint16_t channels = 0;
  uint16_t bits_per_sample = 0;
  bool is_float = false;        // samples are IEEE float, not integer PCM
  uint64_t frames = 0;          // frames captured
  uint64_t silent_frames = 0;   // frames the driver flagged as silent
  double peak = 0.0;            // 0..1, across all channels
  double rms = 0.0;             // 0..1
  double seconds = 0.0;
  float volume = -1.0f;         // endpoint master volume at capture time
  bool muted = false;
  std::vector<uint8_t> pcm;     // raw samples, in the endpoint's own format
  std::string wav_path;         // set when the capture was written to disk
};

// Captures `seconds` of whatever the machine is playing on the default render
// endpoint (or on `device_id` when that is not empty). Blocks for the duration.
//
// Returns ok=false with an explanation when the endpoint cannot be opened. An
// unplugged device, a machine with no audio at all, and a session whose audio
// stack is not running are all ordinary outcomes that a caller must be able to
// tell apart from "it played nothing".
CaptureResult capture_loopback(double seconds,
                               const std::string& device_id = std::string(),
                               const std::string& wav_path = std::string());

// Writes the capture as 16-bit PCM WAV. Exposed because a captured challenge is
// worth keeping -- to hand to a recogniser, and to listen to.
bool write_wav(const std::string& path, const CaptureResult& capture, std::string* error);

// Writes the capture as mono 16-bit PCM at `target_rate` (16000 when zero).
//
// This is the shape a remote solving service will accept. The render endpoint hands
// back 44.1 kHz stereo, which is both more than speech needs and large enough that a
// challenge longer than a few seconds cannot be uploaded at all.
bool write_speech_wav(const std::string& path, const CaptureResult& capture,
                      uint32_t target_rate = 16000, std::string* error = nullptr);

// A short description for a log line: "Speakers (Realtek) 48000 Hz 2ch".
std::string describe_capture(const CaptureResult& capture);

}  // namespace ghost
