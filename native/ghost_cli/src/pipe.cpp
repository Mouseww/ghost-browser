#include "pipe.h"

#include <cstdio>

#include "embed.h"

namespace ghost {
namespace {

std::wstring pipe_path(const std::string& name) {
  return L"\\\\.\\pipe\\" + widen(name);
}

}  // namespace

int serve_pipe(const std::string& name, const PipeHandler& handler) {
  const std::wstring path = pipe_path(name);

  for (;;) {
    HANDLE pipe = CreateNamedPipeW(
        path.c_str(), PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, PIPE_UNLIMITED_INSTANCES, 1 << 16,
        1 << 16, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
      std::fprintf(stderr, "ghost: cannot create the control pipe (%lu)\n", GetLastError());
      return 1;
    }

    // ConnectNamedPipe returns FALSE with ERROR_PIPE_CONNECTED when a client
    // connected between creation and this call, which is success, not failure.
    BOOL connected = ConnectNamedPipe(pipe, nullptr);
    if (!connected && GetLastError() == ERROR_PIPE_CONNECTED) connected = TRUE;
    if (!connected) {
      CloseHandle(pipe);
      continue;
    }

    bool stop = false;
    std::string buffer;
    char chunk[8192];
    DWORD read = 0;

    while (!stop && ReadFile(pipe, chunk, sizeof(chunk), &read, nullptr) && read > 0) {
      buffer.append(chunk, read);
      size_t newline = 0;
      while (!stop && (newline = buffer.find('\n')) != std::string::npos) {
        std::string line = buffer.substr(0, newline);
        buffer.erase(0, newline + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        const std::string response = handler(line, &stop);
        const std::string out = response + "\n";
        DWORD written = 0;
        if (!WriteFile(pipe, out.data(), static_cast<DWORD>(out.size()), &written, nullptr)) {
          stop = true;
          break;
        }
        FlushFileBuffers(pipe);
      }
    }

    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
    if (stop) return 0;
  }
}

bool call_pipe(const std::string& name, const std::string& request, std::string* response,
               std::string* error, DWORD timeout_ms) {
  error->clear();
  response->clear();

  const std::wstring path = pipe_path(name);
  if (!WaitNamedPipeW(path.c_str(), timeout_ms)) {
    *error = "no ghost control pipe named " + name + " (is `ghost serve` running?)";
    return false;
  }

  HANDLE pipe = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            OPEN_EXISTING, 0, nullptr);
  if (pipe == INVALID_HANDLE_VALUE) {
    *error = "cannot open the control pipe (" + std::to_string(GetLastError()) + ")";
    return false;
  }

  DWORD mode = PIPE_READMODE_BYTE;
  SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);

  const std::string out = request + "\n";
  DWORD written = 0;
  if (!WriteFile(pipe, out.data(), static_cast<DWORD>(out.size()), &written, nullptr)) {
    *error = "cannot write to the control pipe";
    CloseHandle(pipe);
    return false;
  }
  FlushFileBuffers(pipe);

  std::string buffer;
  char chunk[8192];
  DWORD read = 0;
  while (buffer.find('\n') == std::string::npos) {
    if (!ReadFile(pipe, chunk, sizeof(chunk), &read, nullptr) || read == 0) break;
    buffer.append(chunk, read);
  }
  CloseHandle(pipe);

  const size_t newline = buffer.find('\n');
  *response = (newline == std::string::npos) ? buffer : buffer.substr(0, newline);
  if (response->empty()) {
    *error = "the control pipe returned nothing";
    return false;
  }
  return true;
}

}  // namespace ghost
