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
# ghost_shim — the injected native layer
# ---------------------------------------------------------------------------
add_library(ghost_shim SHARED
  ghost_shim/src/dllmain.cpp
  ghost_shim/src/profile.cpp
  ghost_shim/src/hook_engine.cpp
  ghost_shim/src/hooks_sysinfo.cpp
  ghost_shim/src/hooks_display.cpp
  ghost_shim/src/hooks_time.cpp
  ghost_shim/src/hooks_gpu.cpp
  ghost_shim/src/hooks_proc.cpp)
target_include_directories(ghost_shim PRIVATE ghost_shim/include ghost_shim/src)
target_link_libraries(ghost_shim PRIVATE minhook shcore user32 gdi32 advapi32)

# ---------------------------------------------------------------------------
# ghost_launch — the injector
# ---------------------------------------------------------------------------
add_executable(ghost_launch ghost_launch/src/main.cpp)
target_link_libraries(ghost_launch PRIVATE advapi32)

# ---------------------------------------------------------------------------
# probe — bare-metal verification of the hooks, no browser involved
# ---------------------------------------------------------------------------
add_executable(probe tests/probe/src/main.cpp)
target_link_libraries(probe PRIVATE shcore user32 gdi32 advapi32)

# Keep every artifact in one directory so ghost_launch can find ghost_shim.dll
# without the caller having to pass --dll.
set_target_properties(ghost_shim ghost_launch probe PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
  LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin")
