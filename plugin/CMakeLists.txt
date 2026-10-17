cmake_minimum_required(VERSION 3.15)
project(VolumeGuardLimiter VERSION 1.4.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Windows: link the C++ runtime statically, so the plugin does not need the Visual C++ Redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(DPF_EXAMPLES OFF CACHE BOOL "" FORCE)
set(DPF_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(dpf)

dpf_add_plugin(VolumeGuardLimiter
  TARGETS vst3 clap ${VGL_EXTRA_TARGETS}
  UI_TYPE opengl
  FILES_DSP
    src/VolumeGuardLimiter.cpp
  FILES_UI
    src/VolumeGuardLimiterUI.cpp)

target_include_directories(VolumeGuardLimiter PUBLIC src)

# Offline DSP checks: ctest
include(CTest)
if(BUILD_TESTING)
  add_executable(test_limiter tests/test_limiter.cpp)
  target_include_directories(test_limiter PRIVATE src)
  add_test(NAME limiter COMMAND test_limiter)
  add_executable(test_limiter_noclamp tests/test_limiter.cpp)
  target_include_directories(test_limiter_noclamp PRIVATE src)
  target_compile_definitions(test_limiter_noclamp PRIVATE VGL_TEST_NO_CLAMP)
  add_test(NAME limiter_without_safety_clamp COMMAND test_limiter_noclamp)
  add_executable(test_game tests/test_game.cpp)
  target_include_directories(test_game PRIVATE src)
  add_test(NAME game_mode COMMAND test_game)
  add_executable(test_game_noclamp tests/test_game.cpp)
  target_include_directories(test_game_noclamp PRIVATE src)
  target_compile_definitions(test_game_noclamp PRIVATE VGL_TEST_NO_CLAMP)
  add_test(NAME game_mode_without_safety_clamp COMMAND test_game_noclamp)
  add_executable(test_chain tests/test_chain.cpp)
  target_include_directories(test_chain PRIVATE src)
  add_test(NAME no_clicks_on_changes COMMAND test_chain)
endif()
