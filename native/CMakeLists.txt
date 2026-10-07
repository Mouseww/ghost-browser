cmake_minimum_required(VERSION 3.20)
project(ghost_native LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "ghost_native currently targets x64 only")
endif()

if(MSVC)
  add_compile_options(/utf-8 /W3 /EHsc /permissive-)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
endif()

# ---------------------------------------------------------------------------
# MinHook (vendored)
# ---------------------------------------------------------------------------
add_library(minhook STATIC
  vendor/minhook/src/buffer.c
  vendor/minhook/src/hook.c
  vendor/minhook/src/trampoline.c
  vendor/minhook/src/hde/hde64.c)
target_include_directories(minhook PUBLIC
  vendor/minhook/include
  vendor/minhook/src)

# ---------------------------------------------------------------------------
# Icon
#
# Embedded twice: ghost.exe wears it, and ghost_shim.dll carries a copy so it can
# put it on the browser window it brands without knowing where the launcher was
# installed. Forward slashes are mandatory here — rc.exe treats a backslash
# inside a string literal as an escape sequence.
# ---------------------------------------------------------------------------
file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/assets/ghost.ico" GHOST_ICON_PATH)

# One version string for every artifact, so the shim and the launcher cannot
# disagree about which build is running.
set(GHOST_VERSION 0.6.0)
set(GHOST_VERSION_COMMA 0,6,0,0)

# ---------------------------------------------------------------------------
# ghost_shim — the injected native layer
# ---------------------------------------------------------------------------
configure_file(ghost_shim/res/shim.rc.in "${CMAKE_CURRENT_BINARY_DIR}/shim.rc" @ONLY)
add_library(ghost_shim SHARED
  ghost_shim/src/dllmain.cpp
  ghost_shim/src/profile.cpp
  ghost_shim/src/hook_engine.cpp
  ghost_shim/src/hooks_sysinfo.cpp
  ghost_shim/src/hooks_display.cpp
  ghost_shim/src/hooks_time.cpp
  ghost_shim/src/hooks_gpu.cpp
  ghost_shim/src/hooks_proc.cpp
  ghost_shim/src/hooks_brand.cpp
  ghost_shim/src/hooks_dwrite.cpp
  "${CMAKE_CURRENT_BINARY_DIR}/shim.rc")
target_include_directories(ghost_shim PRIVATE ghost_shim/include ghost_shim/src)
target_link_libraries(ghost_shim PRIVATE minhook shcore user32 gdi32 advapi32)
set_property(SOURCE "${CMAKE_CURRENT_BINARY_DIR}/shim.rc" APPEND PROPERTY
  OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/ghost.ico")

# ---------------------------------------------------------------------------
# ghost_launch — the injector
# ---------------------------------------------------------------------------
add_executable(ghost_launch ghost_launch/src/main.cpp)
target_link_libraries(ghost_launch PRIVATE advapi32)

# ---------------------------------------------------------------------------
# probe — bare-metal verification of the hooks, no browser involved
# ---------------------------------------------------------------------------
add_executable(probe tests/probe/src/main.cpp common/probe_report.cpp)
target_link_libraries(probe PRIVATE shcore user32 gdi32 advapi32)

# ---------------------------------------------------------------------------
# ghost — the shipped product: one self-contained executable
#
# Only the injectable shim is embedded. An earlier version also embedded
# probe.exe and extracted it on first use, which meant writing a second
# unsigned executable into %LOCALAPPDATA% and running it — the single pattern
# Windows Defender's behaviour heuristic reacts to most reliably (it quarantined
# exactly that file). Instead ghost.exe runs its own hidden `__probe` subcommand
# as the injection target, so the only executable ever launched is the one the
# user already has.
# ---------------------------------------------------------------------------
# rc.exe resolves relative paths against its own working directory, so the
# embedded file has to be absolute. It also has to use forward slashes: rc.exe
# processes backslash escapes inside string literals, so a native path like
# E:\projects\ghost\native\build would be read as a newline before "ative".
file(TO_CMAKE_PATH "${CMAKE_BINARY_DIR}/bin/ghost_shim.dll" GHOST_SHIM_DLL_PATH)
configure_file(ghost_cli/res/ghost.rc.in "${CMAKE_CURRENT_BINARY_DIR}/ghost.rc" @ONLY)

add_executable(ghost
  ghost_cli/src/main.cpp
  ghost_cli/src/embed.cpp
  ghost_cli/src/profile_gen.cpp
  ghost_cli/src/chrome.cpp
  ghost_cli/src/launch.cpp
  ghost_cli/src/json.cpp
  ghost_cli/src/window.cpp
  ghost_cli/src/input.cpp
  ghost_cli/src/uia.cpp
  ghost_cli/src/capture.cpp
  ghost_cli/src/pipe.cpp
  ghost_cli/src/daemon.cpp
  ghost_cli/src/captcha.cpp
  common/probe_report.cpp
  "${CMAKE_CURRENT_BINARY_DIR}/ghost.rc")
target_include_directories(ghost PRIVATE ghost_cli/src)
# version: VerQueryValue for reading the installed browser's version.
# shell32: CommandLineToArgvW, so non-ASCII paths survive argv.
# ole32/oleaut32/uiautomationcore: UI Automation, the accessible replacement for
# reading the DOM over a debugging protocol.
# gdi32: PrintWindow/BitBlt for screenshots taken from outside the browser.
target_link_libraries(ghost PRIVATE advapi32 shcore user32 version shell32 ole32 oleaut32
  uiautomationcore gdi32)
add_dependencies(ghost ghost_shim)
# The resource script references the built shim, but CMake cannot see through
# rc.exe. Without this the exe would silently keep embedding a stale shim after
# a rebuild.
set_property(SOURCE "${CMAKE_CURRENT_BINARY_DIR}/ghost.rc" APPEND PROPERTY
  OBJECT_DEPENDS "${CMAKE_BINARY_DIR}/bin/ghost_shim.dll")
set_property(SOURCE "${CMAKE_CURRENT_BINARY_DIR}/ghost.rc" APPEND PROPERTY
  OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/ghost.ico")

# Keep every artifact in one directory so ghost_launch can find ghost_shim.dll
# without the caller having to pass --dll.
set_target_properties(ghost_shim ghost_launch probe ghost PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
  LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin")
