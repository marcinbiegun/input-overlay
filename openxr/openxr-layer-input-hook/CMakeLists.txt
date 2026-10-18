cmake_minimum_required(VERSION 3.25)

# Also allow building/testing the layer without downloading OBS and Qt.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(openxr-layer-input-hook LANGUAGES CXX)
  if(NOT WIN32)
    message(FATAL_ERROR "The input hook currently supports Windows only")
  endif()
endif()

find_path(INPUT_HOOK_OPENXR_INCLUDE_DIR openxr/openxr_loader_negotiation.h
  DOC "Directory containing the Khronos OpenXR headers (optional, avoids downloading them)")
if(NOT INPUT_HOOK_OPENXR_INCLUDE_DIR)
  include(FetchContent)
  FetchContent_Declare(input_hook_openxr_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
    GIT_TAG release-1.1.43
    GIT_SHALLOW TRUE
    # Only consume generated headers; do not configure the SDK's loader or samples.
    SOURCE_SUBDIR input-overlay-headers-only)
  FetchContent_MakeAvailable(input_hook_openxr_headers)
  set(INPUT_HOOK_OPENXR_INCLUDE_DIR "${input_hook_openxr_headers_SOURCE_DIR}/include")
endif()

add_library(input-hook-ipc STATIC ipc_transport.cpp)
target_include_directories(input-hook-ipc PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(input-hook-ipc PUBLIC cxx_std_17)
target_compile_definitions(input-hook-ipc PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)

add_library(openxr-layer-input-hook SHARED layer.cpp)
target_link_libraries(openxr-layer-input-hook PRIVATE input-hook-ipc)
target_include_directories(openxr-layer-input-hook PRIVATE "${INPUT_HOOK_OPENXR_INCLUDE_DIR}")
target_compile_definitions(openxr-layer-input-hook PRIVATE XR_NO_PROTOTYPES)
set_target_properties(openxr-layer-input-hook PROPERTIES PREFIX "" OUTPUT_NAME "openxr-layer-input-hook")

file(GENERATE OUTPUT "$<TARGET_FILE_DIR:openxr-layer-input-hook>/XR_APILAYER_INPUT_OVERLAY_input_hook.json"
  CONTENT "{\n  \"file_format_version\": \"1.0.0\",\n  \"api_layer\": {\n    \"name\": \"XR_APILAYER_INPUT_OVERLAY_input_hook\",\n    \"library_path\": \"./$<TARGET_FILE_NAME:openxr-layer-input-hook>\",\n    \"api_version\": \"1.0\",\n    \"implementation_version\": \"1\",\n    \"description\": \"input-overlay controller input capture\",\n    \"disable_environment\": \"DISABLE_INPUT_OVERLAY_OPENXR\"\n  }\n}\n")

set(input_hook_install_dir "openxr/openxr-layer-input-hook")
install(TARGETS openxr-layer-input-hook RUNTIME DESTINATION "${input_hook_install_dir}")
install(FILES
  "$<TARGET_FILE_DIR:openxr-layer-input-hook>/XR_APILAYER_INPUT_OVERLAY_input_hook.json"
  Register-Layer.ps1 Unregister-Layer.ps1 README.md
  DESTINATION "${input_hook_install_dir}")

option(INPUT_HOOK_BUILD_TESTS "Build OpenXR input hook and named-pipe tests" OFF)
if(INPUT_HOOK_BUILD_TESTS)
  enable_testing()
  add_executable(input-hook-tests tests.cpp layer.cpp)
  target_link_libraries(input-hook-tests PRIVATE input-hook-ipc)
  target_include_directories(input-hook-tests PRIVATE "${INPUT_HOOK_OPENXR_INCLUDE_DIR}")
  target_compile_definitions(input-hook-tests PRIVATE XR_NO_PROTOTYPES)
  add_test(NAME input-hook-tests COMMAND input-hook-tests)
  set_tests_properties(input-hook-tests PROPERTIES TIMEOUT 30)
endif()
