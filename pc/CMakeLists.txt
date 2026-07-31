cmake_minimum_required(VERSION 3.20)
project(sidepad_pc_capture_spike LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "sidepad_pc_capture_spike targets Windows (DXGI Desktop Duplication only).")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(pc_capture_spike src/main.cpp)

# d3d11.lib / dxgi.lib are pulled in via #pragma comment(lib, ...) in
# main.cpp, but listing them here too is harmless and makes the dependency
# visible without opening the source file.
target_link_libraries(pc_capture_spike PRIVATE d3d11 dxgi)

# Desktop Duplication needs Windows 8+ headers (DXGI 1.2 / IDXGIOutput1).
target_compile_definitions(pc_capture_spike PRIVATE
  _WIN32_WINNT=0x0602
  WIN32_LEAN_AND_MEAN
  NOMINMAX
)
