// The control transport: a named pipe carrying newline-delimited JSON.
//
// A pipe rather than a TCP port, deliberately. The first thing a serious
// anti-bot page does is scan localhost for a debugging port; a named pipe has no
// port to scan and cannot be reached from inside the browser at all. That is the
// entire reason the control plane exists in this shape.
#pragma once

#include <windows.h>

#include <functional>
#include <string>

namespace ghost {

// Handles one request line and returns the response line. Setting `*stop` ends
// the server after the response is written.
using PipeHandler = std::function<std::string(const std::string& request, bool* stop)>;

// Serves until the handler asks to stop or the pipe breaks. Returns a process
// exit code.
int serve_pipe(const std::string& name, const PipeHandler& handler);

// Sends one request and reads one response. Used by `ghost call` and by the
// acceptance test.
bool call_pipe(const std::string& name, const std::string& request, std::string* response,
               std::string* error, DWORD timeout_ms);

}  // namespace ghost
