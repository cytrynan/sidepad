cmake_minimum_required(VERSION 3.20)
project(sidepad_pc_capture_spike LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "sidepad_pc_capture_spike targets Windows (DXGI Desktop Duplication only).")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(pc_capture_spike src/main.cpp)

# The control for the capture measurement: a vsync-paced screen-change
# generator, so capture fps always has a trustworthy source fps to be
# compared against. See src/testsource.cpp for why a timer-driven window
# was not good enough.
add_executable(pc_test_source src/testsource.cpp)

# d3d11.lib / dxgi.lib are pulled in via #pragma comment(lib, ...) in the
# sources, but listing them here too is harmless and makes the dependency
# visible without opening the source file.
foreach(target pc_capture_spike pc_test_source)
  target_link_libraries(${target} PRIVATE d3d11 dxgi)

  # Desktop Duplication needs Windows 8+ headers (DXGI 1.2 / IDXGIOutput1).
  # UNICODE keeps the resource-id macros (IDC_ARROW and friends) on their
  # wide form, matching the ...W APIs both sources call explicitly.
  target_compile_definitions(${target} PRIVATE
    _WIN32_WINNT=0x0602
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
  )
endforeach()
