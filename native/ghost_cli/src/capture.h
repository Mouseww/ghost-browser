// Screenshots taken from outside the browser.
//
// The alternative would be a debugging protocol's Page.captureScreenshot, which
// requires the protocol. PrintWindow asks the window to render itself into a
// bitmap we own, so the page never learns it was photographed.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace ghost {

// Capture the client area as top-down BGRA, 4 bytes per pixel.
bool capture_pixels(HWND window, std::vector<unsigned char>* pixels, int* width, int* height,
                    std::string* error);

// Capture the client area and write it as a BMP file.
bool capture_window(HWND window, const std::string& path, std::string* error);

}  // namespace ghost
