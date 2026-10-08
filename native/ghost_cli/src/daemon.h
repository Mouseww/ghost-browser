// The control plane: a JSON command loop that drives a real browser through the
// operating system instead of through a debugging protocol.
//
// Every command here is something a person could do with a mouse and a keyboard,
// and that is the design constraint, not a limitation. Nothing in this file
// touches the renderer, so nothing in this file can be detected from a page.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace ghost {

struct ServeOptions {
  std::string pipe_name;  // empty: derived from the profile id
  std::string profile_id = "default";
  std::string profile_json;
  std::string timezone;
  std::string locale;
  std::string browser;  // preference ("chrome"/"edge") or an explicit path
  std::string data_dir;
  std::vector<std::string> chrome_args;
  std::vector<std::string> urls;
  DWORD attach_pid = 0;  // non-zero: drive an already-running browser
  bool sandbox = false;
  // DevTools over anonymous pipes, on by default. It costs nothing a page can
  // see -- no port, no file in the profile -- and it is the only way to reach the
  // parts of a challenge that are not in the accessibility tree. `--no-cdp`
  // turns it off for callers who would rather have no debugging channel at all.
  bool cdp = true;
  bool verbose = false;
};

std::string default_pipe_name(const std::string& profile_id);

// Launches the browser (unless attaching), then serves the control pipe until a
// `shutdown` request arrives. Returns a process exit code.
int run_serve(const ServeOptions& options);

}  // namespace ghost
