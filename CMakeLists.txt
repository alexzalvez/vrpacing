cmake_minimum_required(VERSION 3.22)
project(VRPacing VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(VRPACING_BUILD_TESTS "Build portable core tests" ON)
option(VRPACING_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

if(MSVC)
  add_compile_options(/W4 /permissive- /Zc:__cplusplus)
  if(VRPACING_WARNINGS_AS_ERRORS)
    add_compile_options(/WX)
  endif()
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
  if(VRPACING_WARNINGS_AS_ERRORS)
    add_compile_options(-Werror)
  endif()
endif()

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/third_party)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/third_party/openxr/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/third_party/openvr)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/third_party/oculus)

add_library(vrpacing_core STATIC
  src/core/mask_geometry.cpp
  src/core/frame_limiter_math.cpp
  src/core/hz_tracker.cpp
)

target_include_directories(vrpacing_core PUBLIC include)

if(WIN32)
  add_library(minhook STATIC
    third_party/minhook/src/buffer.c
    third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c
    third_party/minhook/src/hde/hde64.c
  )
  target_include_directories(minhook PUBLIC third_party/minhook/include third_party/minhook/src)

  add_library(vrpacing_win_common STATIC
    src/common/config_json.cpp
    src/common/logging_win.cpp
    src/common/frame_limiter_win.cpp
  )
  target_link_libraries(vrpacing_win_common PUBLIC vrpacing_core)

  add_library(VRPacingLayer SHARED
    src/openxr/layer.cpp
    src/gfx/nis_upscaler_d3d11.cpp
  )
  target_link_libraries(VRPacingLayer PRIVATE vrpacing_core vrpacing_win_common d3d11 d3dcompiler dxgi)
  target_compile_definitions(VRPacingLayer PRIVATE XR_USE_PLATFORM_WIN32 XR_USE_GRAPHICS_API_D3D11)
  set_target_properties(VRPacingLayer PROPERTIES OUTPUT_NAME "VRPacingLayer")

  add_library(VRPacingHook SHARED
    src/hook_dllmain.cpp
    src/openvr/hook_openvr.cpp
    src/oculus/hook_oculus.cpp
    src/gfx/nis_upscaler_d3d11.cpp
    src/injector/process_scanner.cpp
  )
  target_link_libraries(VRPacingHook PRIVATE vrpacing_core vrpacing_win_common minhook d3d11 d3dcompiler dxgi)
  set_target_properties(VRPacingHook PROPERTIES OUTPUT_NAME "VRPacingHook")

  add_executable(VRPacingTray WIN32
    src/tray/main.cpp
    src/injector/process_scanner.cpp
    src/common/config_json.cpp
    src/common/logging_win.cpp
  )
  target_link_libraries(VRPacingTray PRIVATE vrpacing_core user32 shell32 gdi32 advapi32)
  set_target_properties(VRPacingTray PROPERTIES OUTPUT_NAME "VRPacingTray")

  configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/openxr/VRPacingLayer.json.in
    ${CMAKE_CURRENT_BINARY_DIR}/VRPacingLayer.json
    @ONLY
  )
endif()

if(VRPACING_BUILD_TESTS)
  enable_testing()
  add_executable(vrpacing_tests
    tests/test_main.cpp
  )
  target_link_libraries(vrpacing_tests PRIVATE vrpacing_core)
  add_test(NAME vrpacing_tests COMMAND vrpacing_tests)
endif()
