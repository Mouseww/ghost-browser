// Talking to the browser over a pipe that nothing else can see.
//
// Chrome normally exposes DevTools on a TCP port, which means a listening socket
// and a `DevToolsActivePort` file inside the profile. Both are discoverable: the
// port by any local process that scans, the file by anything that can read the
// profile directory.
//
// On Windows there is a second transport. `--remote-debugging-pipe` makes the
// browser speak the protocol on file descriptors 3 and 4, and
// `--remote-debugging-io-pipes=<read>,<write>` (content_switches.cc) replaces
// those descriptors with two handle values the launcher chooses. When that
// switch is present Chrome skips the descriptor check entirely
// (chrome_main_delegate.cc:1221-1233), so a launcher that creates its own
// anonymous pipes gets a full DevTools session with no socket and no file.
//
// The client below owns the parent ends of those two pipes. It is deliberately
// synchronous: the control plane answers one request at a time, and a request
// that could interleave with another would make the pipe's framing a shared
// resource for no gain.
#pragma once

#include <windows.h>

#include <string>

#include "json.h"

namespace ghost {

class CdpClient {
 public:
  CdpClient() = default;
  ~CdpClient();

  CdpClient(const CdpClient&) = delete;
  CdpClient& operator=(const CdpClient&) = delete;

  // Takes ownership of both parent ends. Either may be null, which leaves the
  // client disconnected.
  void adopt(HANDLE read_end, HANDLE write_end);
  bool connected() const { return read_ != nullptr && write_ != nullptr; }
  void close();

  // Sends one command and returns its result. Replies are matched by id, and
  // events -- messages that carry no id -- are skipped rather than mistaken for
  // the answer. Returns false on transport failure, timeout, or a CDP error.
  bool call(const std::string& method, const Json& params, const std::string& session_id,
            Json* result, std::string* error, int timeout_ms = 15000);

  // Convenience for a command with no parameters and no session.
  bool call(const std::string& method, Json* result, std::string* error,
            int timeout_ms = 15000);

  // Finds the page target and attaches to it, remembering the session id that
  // every later page-scoped command needs. Safe to call more than once: a new
  // navigation can replace the target, and the stale session would then answer
  // nothing.
  bool attach_to_page(std::string* error, int timeout_ms = 15000);
  bool attach_to_page(bool force, std::string* error, int timeout_ms = 15000);
  const std::string& page_session() const { return page_session_; }

  // Evaluates an expression in the page and returns its value as text. This is
  // the escape hatch the DOM-only commands below are built on, and it is only
  // ever called with a page session -- never with Runtime.enable, whose side
  // effects on the console object are the well-known way a page detects DevTools.
  bool evaluate(const std::string& expression, std::string* value, std::string* error,
                int timeout_ms = 15000);

 private:
  bool write_message(const std::string& text);
  bool read_message(std::string* text, int timeout_ms);

  HANDLE read_ = nullptr;
  HANDLE write_ = nullptr;
  int next_id_ = 1;
  std::string page_session_;
  // Bytes read from the pipe that did not yet contain a whole message. A single
  // ReadFile can return a partial frame, and dropping it would desynchronise
  // every later reply.
  std::string pending_;
};

// Human-readable form of the last Win32 failure, for a control-plane response.
std::string cdp_last_error();

}  // namespace ghost
