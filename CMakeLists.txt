cmake_minimum_required(VERSION 3.22)

project(VRPacing VERSION 0.9.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(VRPACING_BUILD_TESTS "Build portable core tests" ON)
option(VRPACING_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

if(MSVC)
  add_compile_options(/W4 /permissive- /Zc:__cplusplus /utf-8)
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
include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/third_party)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/third_party/openxr/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/third_party/openvr)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/third_party/oculus)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/third_party/nvapi)

if(CMAKE_SIZEOF_VOID_P EQUAL 4)
  set(VRPACING_IS_X86 TRUE)
  set(NVAPI_LIB "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nvapi/x86/nvapi.lib")
  set(NVAPI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nvapi/x86")
  set(NVAPI_LIB_NAME "nvapi")
  set(MINHOOK_HDE_SRC "third_party/minhook/src/hde/hde32.c")
  set(VRPACING_LAYER_TARGET_NAME "VRPacingLayer32")
  set(VRPACING_HOOK_TARGET_NAME "VRPacingHook32")
  set(VRPACING_LAYER_NAME "XR_APILAYER_ALEXZALVEZ_vrpacing_x86")
  set(VRPACING_LAYER_DLL_NAME "./VRPacingLayer32.dll")
  set(VRPACING_LAYER_JSON_NAME "VRPacingLayer32.json")
else()
  set(VRPACING_IS_X86 FALSE)
  set(NVAPI_LIB "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nvapi/amd64/nvapi64.lib")
  set(NVAPI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nvapi/amd64")
  set(NVAPI_LIB_NAME "nvapi64")
  set(MINHOOK_HDE_SRC "third_party/minhook/src/hde/hde64.c")
  set(VRPACING_LAYER_TARGET_NAME "VRPacingLayer")
  set(VRPACING_HOOK_TARGET_NAME "VRPacingHook")
  set(VRPACING_LAYER_NAME "XR_APILAYER_ALEXZALVEZ_vrpacing")
  set(VRPACING_LAYER_DLL_NAME "./VRPacingLayer.dll")
  set(VRPACING_LAYER_JSON_NAME "VRPacingLayer.json")
endif()

add_library(vrpacing_core STATIC
  src/core/mask_geometry.cpp
  src/core/frame_limiter_math.cpp
  src/core/hz_tracker.cpp
  src/common/visibility_mask.cpp
)
target_include_directories(vrpacing_core PUBLIC include src)

if(WIN32)
  add_library(minhook STATIC
    third_party/minhook/src/buffer.c
    third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c
    ${MINHOOK_HDE_SRC}
  )
  target_include_directories(minhook PUBLIC third_party/minhook/include third_party/minhook/src)
  if(MSVC)
    target_compile_options(minhook PRIVATE /W0)
  else()
    target_compile_options(minhook PRIVATE -w)
  endif()

  add_library(vrpacing_win_common STATIC
    src/common/config_json.cpp
    src/common/logging_win.cpp
    src/common/frame_limiter_win.cpp
  )
  target_link_libraries(vrpacing_win_common PUBLIC vrpacing_core)

  add_library(VRPacingLayer SHARED
    src/openxr/layer.cpp
    src/openxr/layer_dllmain.cpp
    src/gfx/d3d11_helper.cpp
    src/gfx/smaa_pass_d3d11.cpp
    src/gfx/nis_upscaler_d3d11.cpp
    src/gfx/vrs_foveated_d3d11.cpp
    src/gfx/vrs_foveated_d3d12.cpp
  )
  target_link_directories(VRPacingLayer PRIVATE
    "${NVAPI_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nvapi"
  )
  target_link_libraries(VRPacingLayer PRIVATE vrpacing_core vrpacing_win_common d3d11 d3d12 d3dcompiler dxgi minhook ${NVAPI_LIB_NAME} windowscodecs ole32)
  target_compile_definitions(VRPacingLayer PRIVATE XR_USE_PLATFORM_WIN32 XR_USE_GRAPHICS_API_D3D11 XR_USE_GRAPHICS_API_D3D12)
  set_target_properties(VRPacingLayer PROPERTIES OUTPUT_NAME "${VRPACING_LAYER_TARGET_NAME}")

  add_library(VRPacingHook SHARED
    src/hook_dllmain.cpp
    src/openvr/hook_openvr.cpp
    src/oculus/hook_oculus.cpp
    src/gfx/d3d11_helper.cpp
    src/gfx/smaa_pass_d3d11.cpp
    src/gfx/nis_upscaler_d3d11.cpp
    src/gfx/vrs_foveated_d3d11.cpp
  )
  target_link_libraries(VRPacingHook PRIVATE vrpacing_core vrpacing_win_common minhook d3d11 d3dcompiler dxgi "${NVAPI_LIB}" dxguid windowscodecs ole32)
  set_target_properties(VRPacingHook PROPERTIES OUTPUT_NAME "${VRPACING_HOOK_TARGET_NAME}")

  if(VRPACING_IS_X86)
    add_executable(VRPacingInjector32 WIN32
      src/injector/injector32_main.cpp
    )
    target_link_libraries(VRPacingInjector32 PRIVATE kernel32 user32 shell32)
    set_target_properties(VRPacingInjector32 PROPERTIES OUTPUT_NAME "VRPacingInjector32")
  else()
    add_executable(VRPacingTray WIN32
      src/tray/main.cpp
      src/injector/process_scanner.cpp
      src/common/config_json.cpp
      src/common/logging_win.cpp
    )
    target_link_libraries(VRPacingTray PRIVATE vrpacing_core user32 shell32 gdi32 advapi32 comctl32)
    set_target_properties(VRPacingTray PROPERTIES OUTPUT_NAME "VRPacingTray")

    if(MSVC)
      set_target_properties(VRPacingTray PROPERTIES LINK_FLAGS "/MANIFESTUAC:\"level='requireAdministrator' uiAccess='false'\"")
    endif()
  endif()

  configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/openxr/VRPacingLayer.json.in
    ${CMAKE_CURRENT_BINARY_DIR}/${VRPACING_LAYER_JSON_NAME}
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
