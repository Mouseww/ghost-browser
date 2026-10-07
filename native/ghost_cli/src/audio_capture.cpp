#include "audio_capture.h"

#include <windows.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "embed.h"

namespace ghost {
namespace {

// The two subtype GUIDs a shared-mode mix format can carry. Defined here rather
// than pulled from <mmreg.h> so this file does not need an initguid.h dance or a
// uuid.lib dependency just to compare two constants.
const GUID kSubtypePcm = {
    0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const GUID kSubtypeFloat = {
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

template <typename T>
void release(T*& p) {
  if (p != nullptr) {
    p->Release();
    p = nullptr;
  }
}

std::string hresult_text(HRESULT hr) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
  return buf;
}

// A COM apartment for the duration of one call. RPC_E_CHANGED_MODE means the
// thread was already initialized with the other model, which is not an error
// here -- the interfaces used below are agile enough for that -- so it must not
// be reported as a failure.
class Apartment {
 public:
  Apartment() {
    hr_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    owned_ = SUCCEEDED(hr_);
  }
  ~Apartment() {
    if (owned_) CoUninitialize();
  }
  Apartment(const Apartment&) = delete;
  Apartment& operator=(const Apartment&) = delete;

  bool usable() const { return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE; }
  HRESULT hr() const { return hr_; }

 private:
  HRESULT hr_ = E_FAIL;
  bool owned_ = false;
};

std::string prop_string(IPropertyStore* store, REFPROPERTYKEY key) {
  PROPVARIANT value;
  PropVariantInit(&value);
  std::string out;
  if (SUCCEEDED(store->GetValue(key, &value)) && value.vt == VT_LPWSTR &&
      value.pwszVal != nullptr) {
    out = narrow(value.pwszVal);
  }
  PropVariantClear(&value);
  return out;
}

std::string device_name_of(IMMDevice* device) {
  IPropertyStore* store = nullptr;
  std::string name;
  if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store))) {
    name = prop_string(store, PKEY_Device_FriendlyName);
    release(store);
  }
  return name;
}

// Master volume and mute, worth reporting next to a silent capture: a muted or
// zeroed endpoint produces exactly the same "it heard nothing" as a browser that
// never played anything, and the two have nothing else in common.
void read_volume(IMMDevice* device, float* volume, bool* muted) {
  IAudioEndpointVolume* endpoint = nullptr;
  if (FAILED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(&endpoint)))) {
    return;
  }
  float level = 0.0f;
  BOOL silent = FALSE;
  if (SUCCEEDED(endpoint->GetMasterVolumeLevelScalar(&level))) *volume = level;
  if (SUCCEEDED(endpoint->GetMute(&silent))) *muted = silent != FALSE;
  release(endpoint);
}

// What the samples in the endpoint's mix format actually are. The shared-mode
// mix format is float32 on every modern Windows, but integer endpoints exist,
// and reading 16-bit samples as floats reports a loud capture as silence.
enum class SampleKind { kFloat32, kInt16, kInt32, kUnknown };

SampleKind sample_kind(const WAVEFORMATEX* format) {
  WORD tag = format->wFormatTag;
  if (tag == WAVE_FORMAT_EXTENSIBLE &&
      format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
    const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
    if (IsEqualGUID(ext->SubFormat, kSubtypeFloat)) {
      tag = WAVE_FORMAT_IEEE_FLOAT;
    } else if (IsEqualGUID(ext->SubFormat, kSubtypePcm)) {
      tag = WAVE_FORMAT_PCM;
    }
  }
  if (tag == WAVE_FORMAT_IEEE_FLOAT && format->wBitsPerSample == 32) {
    return SampleKind::kFloat32;
  }
  if (tag == WAVE_FORMAT_PCM && format->wBitsPerSample == 16) return SampleKind::kInt16;
  if (tag == WAVE_FORMAT_PCM && format->wBitsPerSample == 32) return SampleKind::kInt32;
  return SampleKind::kUnknown;
}

size_t bytes_per_sample(SampleKind kind) {
  switch (kind) {
    case SampleKind::kFloat32: return 4;
    case SampleKind::kInt16: return 2;
    case SampleKind::kInt32: return 4;
    default: return 0;
  }
}

// One sample, normalized to [-1, 1]. Non-finite floats become 0 rather than
// poisoning the running sum with a NaN, which would silently turn the whole
// capture's level into "not a number".
double sample_at(const uint8_t* base, size_t index, SampleKind kind) {
  switch (kind) {
    case SampleKind::kFloat32: {
      float v = 0.0f;
      std::memcpy(&v, base + index * sizeof(float), sizeof(float));
      if (!std::isfinite(v)) return 0.0;
      return std::min(1.0, std::max(-1.0, static_cast<double>(v)));
    }
    case SampleKind::kInt16: {
      int16_t v = 0;
      std::memcpy(&v, base + index * sizeof(int16_t), sizeof(int16_t));
      return v / 32768.0;
    }
    case SampleKind::kInt32: {
      int32_t v = 0;
      std::memcpy(&v, base + index * sizeof(int32_t), sizeof(int32_t));
      return v / 2147483648.0;
    }
    default:
      return 0.0;
  }
}

// Converts one sample to the 16-bit PCM a WAV file wants.
int16_t to_pcm16(const uint8_t* base, size_t index, SampleKind kind) {
  const double v = sample_at(base, index, kind);
  const double scaled = v * 32767.0;
  return static_cast<int16_t>(std::max(-32768.0, std::min(32767.0, scaled)));
}

void put_u32(std::string& out, uint32_t v) {
  out.push_back(static_cast<char>(v & 0xFF));
  out.push_back(static_cast<char>((v >> 8) & 0xFF));
  out.push_back(static_cast<char>((v >> 16) & 0xFF));
  out.push_back(static_cast<char>((v >> 24) & 0xFF));
}

void put_u16(std::string& out, uint16_t v) {
  out.push_back(static_cast<char>(v & 0xFF));
  out.push_back(static_cast<char>((v >> 8) & 0xFF));
}

}  // namespace

std::vector<AudioDevice> list_render_devices() {
  std::vector<AudioDevice> out;
  Apartment apartment;
  if (!apartment.usable()) return out;

  IMMDeviceEnumerator* enumerator = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              IID_PPV_ARGS(&enumerator)))) {
    return out;
  }

  std::string default_id;
  IMMDevice* default_device = nullptr;
  if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &default_device))) {
    LPWSTR id = nullptr;
    if (SUCCEEDED(default_device->GetId(&id)) && id != nullptr) {
      default_id = narrow(id);
      CoTaskMemFree(id);
    }
    release(default_device);
  }

  IMMDeviceCollection* collection = nullptr;
  if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection))) {
    UINT count = 0;
    if (SUCCEEDED(collection->GetCount(&count))) {
      for (UINT i = 0; i < count; ++i) {
        IMMDevice* device = nullptr;
        if (FAILED(collection->Item(i, &device))) continue;
        AudioDevice info;
        LPWSTR id = nullptr;
        if (SUCCEEDED(device->GetId(&id)) && id != nullptr) {
          info.id = narrow(id);
          CoTaskMemFree(id);
        }
        info.is_default = !info.id.empty() && info.id == default_id;
        info.name = device_name_of(device);
        read_volume(device, &info.volume, &info.muted);
        release(device);
        out.push_back(info);
      }
    }
    release(collection);
  }
  release(enumerator);
  return out;
}

// Which processes are playing, and how loudly, on the default render endpoint.
//
// A silent loopback capture is ambiguous: the page may have played nothing, or
// nothing may have opened a stream at all. The session list separates those, and
// it is the same information the Windows volume mixer shows.
std::vector<AudioSession> list_audio_sessions(std::string* error) {
  std::vector<AudioSession> out;
  Apartment apartment;
  if (!apartment.usable()) {
    if (error != nullptr) *error = "the audio stack could not be reached";
    return out;
  }

  IMMDeviceEnumerator* enumerator = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              IID_PPV_ARGS(&enumerator)))) {
    if (error != nullptr) *error = "there is no audio device enumerator";
    return out;
  }

  IMMDevice* device = nullptr;
  if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) {
    release(enumerator);
    if (error != nullptr) *error = "there is no default render endpoint";
    return out;
  }

  IAudioSessionManager2* manager = nullptr;
  if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(&manager)))) {
    release(device);
    release(enumerator);
    if (error != nullptr) *error = "the endpoint exposes no session manager";
    return out;
  }

  IAudioSessionEnumerator* sessions = nullptr;
  if (SUCCEEDED(manager->GetSessionEnumerator(&sessions))) {
    int count = 0;
    if (SUCCEEDED(sessions->GetCount(&count))) {
      for (int i = 0; i < count; ++i) {
        IAudioSessionControl* control = nullptr;
        if (FAILED(sessions->GetSession(i, &control)) || control == nullptr) continue;

        AudioSession session;
        IAudioSessionControl2* control2 = nullptr;
        if (SUCCEEDED(control->QueryInterface(
                __uuidof(IAudioSessionControl2), reinterpret_cast<void**>(&control2)))) {
          DWORD pid = 0;
          if (SUCCEEDED(control2->GetProcessId(&pid))) session.pid = pid;
          session.system_sounds = control2->IsSystemSoundsSession() == S_OK;
          LPWSTR identifier = nullptr;
          if (SUCCEEDED(control2->GetSessionIdentifier(&identifier)) &&
              identifier != nullptr) {
            session.name = narrow(identifier);
            CoTaskMemFree(identifier);
          }
          release(control2);
        }

        AudioSessionState state = AudioSessionStateInactive;
        if (SUCCEEDED(control->GetState(&state))) {
          session.state = static_cast<int>(state);
        }
        IAudioMeterInformation* meter = nullptr;
        if (SUCCEEDED(control->QueryInterface(
                __uuidof(IAudioMeterInformation), reinterpret_cast<void**>(&meter)))) {
          meter->GetPeakValue(&session.peak);
          release(meter);
        }

        release(control);
        out.push_back(session);
      }
    }
    release(sessions);
  }

  release(manager);
  release(device);
  release(enumerator);
  return out;
}

CaptureResult capture_loopback(double seconds, const std::string& device_id,
                               const std::string& wav_path) {
  CaptureResult result;
  result.wav_path = wav_path;
  if (seconds <= 0.0) {
    result.error = "capture length must be positive";
    return result;
  }

  Apartment apartment;
  if (!apartment.usable()) {
    result.error = "COM would not start on this thread: " + hresult_text(apartment.hr());
    return result;
  }

  IMMDeviceEnumerator* enumerator = nullptr;
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator));
  if (FAILED(hr) || enumerator == nullptr) {
    result.error = "no audio device enumerator: " + hresult_text(hr);
    return result;
  }

  IMMDevice* device = nullptr;
  if (!device_id.empty()) {
    hr = enumerator->GetDevice(widen(device_id).c_str(), &device);
  } else {
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
  }
  release(enumerator);
  if (FAILED(hr) || device == nullptr) {
    result.error = device_id.empty()
                       ? "this machine has no default audio output device: " +
                             hresult_text(hr)
                       : "no audio output device with that id: " + hresult_text(hr);
    return result;
  }
  result.device_name = device_name_of(device);
  read_volume(device, &result.volume, &result.muted);

  IAudioClient* client = nullptr;
  hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                        reinterpret_cast<void**>(&client));
  release(device);
  if (FAILED(hr) || client == nullptr) {
    result.error = "the audio endpoint would not open a client: " + hresult_text(hr);
    return result;
  }

  WAVEFORMATEX* format = nullptr;
  hr = client->GetMixFormat(&format);
  if (FAILED(hr) || format == nullptr) {
    result.error = "the audio endpoint would not report its format: " + hresult_text(hr);
    release(client);
    return result;
  }
  result.sample_rate = format->nSamplesPerSec;
  result.channels = format->nChannels;
  result.bits_per_sample = format->wBitsPerSample;

  // Loopback is the whole trick: the render endpoint hands back what it is
  // playing, so no page cooperation and no capture device is needed. The buffer
  // is one second; the capture loop drains it until the deadline.
  hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                          10000000, 0, format, nullptr);
  if (FAILED(hr)) {
    result.error = "loopback capture is not available on this endpoint: " +
                   hresult_text(hr);
    CoTaskMemFree(format);
    release(client);
    return result;
  }

  IAudioCaptureClient* capture = nullptr;
  hr = client->GetService(IID_PPV_ARGS(&capture));
  if (FAILED(hr) || capture == nullptr) {
    result.error = "the audio endpoint offered no capture interface: " + hresult_text(hr);
    CoTaskMemFree(format);
    release(client);
    return result;
  }

  hr = client->Start();
  if (FAILED(hr)) {
    result.error = "the audio endpoint would not start: " + hresult_text(hr);
    release(capture);
    CoTaskMemFree(format);
    release(client);
    return result;
  }

  const SampleKind kind = sample_kind(format);
  result.is_float = kind == SampleKind::kFloat32;
  const size_t sample_bytes = bytes_per_sample(kind);
  const size_t block_align = format->nBlockAlign;
  double sum_squares = 0.0;
  uint64_t counted = 0;

  const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(seconds * 1000.0);
  while (GetTickCount64() < deadline) {
    for (;;) {
      UINT32 packet = 0;
      if (FAILED(capture->GetNextPacketSize(&packet)) || packet == 0) break;

      BYTE* data = nullptr;
      UINT32 frames = 0;
      DWORD flags = 0;
      if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;

      const size_t bytes = static_cast<size_t>(frames) * block_align;
      if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr) {
        result.silent_frames += frames;
        result.pcm.insert(result.pcm.end(), bytes, 0);
      } else {
        result.pcm.insert(result.pcm.end(), data, data + bytes);
        if (sample_bytes != 0) {
          const size_t samples = bytes / sample_bytes;
          for (size_t i = 0; i < samples; ++i) {
            const double v = sample_at(data, i, kind);
            sum_squares += v * v;
            const double magnitude = std::fabs(v);
            if (magnitude > result.peak) result.peak = magnitude;
            ++counted;
          }
        }
      }
      result.frames += frames;
      capture->ReleaseBuffer(frames);
    }
    Sleep(10);
  }

  client->Stop();
  if (counted != 0) result.rms = std::sqrt(sum_squares / static_cast<double>(counted));
  if (result.sample_rate != 0) {
    result.seconds = static_cast<double>(result.frames) / result.sample_rate;
  }

  release(capture);
  CoTaskMemFree(format);
  release(client);

  if (!wav_path.empty()) {
    std::string error;
    if (!write_wav(wav_path, result, &error)) {
      result.error = error;
      return result;
    }
  }
  result.ok = true;
  return result;
}

bool write_wav(const std::string& path, const CaptureResult& capture, std::string* error) {
  if (capture.channels == 0 || capture.sample_rate == 0) {
    if (error != nullptr) *error = "nothing to write: the capture has no format";
    return false;
  }
  const SampleKind kind = capture.is_float ? SampleKind::kFloat32
                                           : (capture.bits_per_sample == 16
                                                  ? SampleKind::kInt16
                                                  : SampleKind::kInt32);
  const size_t sample_bytes = bytes_per_sample(kind);
  if (sample_bytes == 0) {
    if (error != nullptr) *error = "nothing to write: unknown sample format";
    return false;
  }

  const size_t samples = capture.pcm.size() / sample_bytes;
  const size_t frames = samples / capture.channels;
  const uint32_t data_bytes = static_cast<uint32_t>(frames * capture.channels * 2);

  std::string out;
  out.reserve(44 + data_bytes);
  out.append("RIFF", 4);
  put_u32(out, 36 + data_bytes);
  out.append("WAVE", 4);
  out.append("fmt ", 4);
  put_u32(out, 16);
  put_u16(out, 1);  // PCM
  put_u16(out, capture.channels);
  put_u32(out, capture.sample_rate);
  put_u32(out, capture.sample_rate * capture.channels * 2);
  put_u16(out, static_cast<uint16_t>(capture.channels * 2));
  put_u16(out, 16);
  out.append("data", 4);
  put_u32(out, data_bytes);
  for (size_t i = 0; i < frames * capture.channels; ++i) {
    put_u16(out, static_cast<uint16_t>(to_pcm16(capture.pcm.data(), i, kind)));
  }

  if (!write_file(path, out)) {
    if (error != nullptr) *error = "could not write " + path;
    return false;
  }
  return true;
}

std::string describe_capture(const CaptureResult& capture) {
  char buf[256];
  std::snprintf(buf, sizeof(buf), "%s  %u Hz  %uch  %u-bit  %.2fs",
                capture.device_name.empty() ? "(unnamed device)"
                                            : capture.device_name.c_str(),
                static_cast<unsigned>(capture.sample_rate),
                static_cast<unsigned>(capture.channels),
                static_cast<unsigned>(capture.bits_per_sample), capture.seconds);
  std::string out = buf;
  if (capture.volume >= 0.0f) {
    char vol[64];
    std::snprintf(vol, sizeof(vol), "  volume %.0f%%%s", capture.volume * 100.0f,
                  capture.muted ? " (muted)" : "");
    out += vol;
  }
  return out;
}

}  // namespace ghost
