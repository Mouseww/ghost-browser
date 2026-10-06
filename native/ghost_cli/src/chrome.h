// Locating a Chromium browser on the host.
#pragma once

#include <string>

namespace ghost {

struct BrowserInstall {
  std::string name;     // "Chrome", "Edge"
  std::string path;     // absolute path to the executable
  std::string version;  // "154.0.8037.98"; empty when it cannot be read
};

// Find an installed Chromium browser. `prefer` is either empty/"chrome",
// "edge", or an explicit path to an executable.
BrowserInstall find_browser(const std::string& prefer = std::string());

// Version of a PE image, read from its version resource.
std::string file_version(const std::string& path);

// Value of an environment variable, empty when unset.
std::string env_var(const char* name);

}  // namespace ghost
