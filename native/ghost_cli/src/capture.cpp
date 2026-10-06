#include "capture.h"

#include <cstdio>

#ifndef PW_RENDERFULLCONTENT
// Asks the window to include content composed by DirectComposition, which is
// how Chromium presents its frames. Without it PrintWindow returns a blank
// surface for the browser.
#define PW_RENDERFULLCONTENT 0x00000002
#endif

namespace ghost {

bool capture_pixels(HWND window, std::vector<unsigned char>* pixels, int* width, int* height,
                    std::string* error) {
  error->clear();
  pixels->clear();
  *width = 0;
  *height = 0;

  if (window == nullptr || !IsWindow(window)) {
    *error = "no such window";
    return false;
  }

  RECT client{};
  if (!GetClientRect(window, &client)) {
    *error = "GetClientRect failed";
    return false;
  }
  const int w = client.right - client.left;
  const int h = client.bottom - client.top;
  if (w <= 0 || h <= 0) {
    *error = "the window has no client area";
    return false;
  }

  HDC screen = GetDC(nullptr);
  if (screen == nullptr) {
    *error = "GetDC failed";
    return false;
  }
  HDC memory = CreateCompatibleDC(screen);
  if (memory == nullptr) {
    ReleaseDC(nullptr, screen);
    *error = "CreateCompatibleDC failed";
    return false;
  }

  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = w;
  info.bmiHeader.biHeight = -h;  // negative: rows run top-down, like the file format
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;

  void* bits = nullptr;
  HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (bitmap == nullptr || bits == nullptr) {
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    *error = "CreateDIBSection failed";
    return false;
  }

  HGDIOBJ previous = SelectObject(memory, bitmap);

  BOOL ok = PrintWindow(window, memory, PW_RENDERFULLCONTENT);
  if (!ok) {
    // Some windows refuse to render themselves. The screen is then the only
    // source, which requires the window to actually be visible.
    POINT origin{0, 0};
    ClientToScreen(window, &origin);
    ok = BitBlt(memory, 0, 0, w, h, screen, origin.x, origin.y, SRCCOPY);
  }

  if (ok) {
    const size_t size = static_cast<size_t>(w) * h * 4;
    pixels->assign(static_cast<unsigned char*>(bits),
                   static_cast<unsigned char*>(bits) + size);
    *width = w;
    *height = h;
  } else {
    *error = "PrintWindow and BitBlt both failed";
  }

  SelectObject(memory, previous);
  DeleteObject(bitmap);
  DeleteDC(memory);
  ReleaseDC(nullptr, screen);
  return ok != FALSE;
}

bool capture_window(HWND window, const std::string& path, std::string* error) {
  std::vector<unsigned char> pixels;
  int width = 0;
  int height = 0;
  if (!capture_pixels(window, &pixels, &width, &height, error)) return false;

  FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) {
    *error = "cannot open " + path + " for writing";
    return false;
  }

  BITMAPFILEHEADER file_header{};
  file_header.bfType = 0x4D42;  // "BM"
  file_header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
  file_header.bfSize = file_header.bfOffBits + static_cast<DWORD>(pixels.size());

  BITMAPINFOHEADER info_header{};
  info_header.biSize = sizeof(BITMAPINFOHEADER);
  info_header.biWidth = width;
  info_header.biHeight = height;  // positive: the pixels are already top-down
  info_header.biPlanes = 1;
  info_header.biBitCount = 32;
  info_header.biCompression = BI_RGB;
  info_header.biSizeImage = static_cast<DWORD>(pixels.size());

  std::fwrite(&file_header, sizeof(file_header), 1, file);
  std::fwrite(&info_header, sizeof(info_header), 1, file);
  std::fwrite(pixels.data(), 1, pixels.size(), file);
  std::fclose(file);
  return true;
}

}  // namespace ghost
