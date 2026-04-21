cmake_minimum_required(VERSION 3.14)
project(runner LANGUAGES C CXX)

# Define the application target. To change its name, change BINARY_NAME in the
# top-level CMakeLists.txt, not the value here, or `flutter run` will no longer
# work.
#
# Any new source files that you add to the application should be added here.
add_executable(${BINARY_NAME} WIN32
  "flutter_window.cpp"
  "main.cpp"
  "utils.cpp"
  "win32_window.cpp"
  "native_camera.cpp"
  "screen_capture_source.cpp"
  "whiteboard_canvas.cpp"
  "whiteboard_canvas_process.cpp"
  "whiteboard_enhance.cpp"
  "virtual_display_manager.cpp"
  "ai_canvas_checker.cpp"
  "http_client.cpp"
  "${FLUTTER_MANAGED_DIR}/generated_plugin_registrant.cc"
  "Runner.rc"
  "runner.exe.manifest"
)

# Local paths configuration (gitignored)
# Copy local_paths.cmake.example to local_paths.cmake and update with your paths
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/local_paths.cmake")
  include("${CMAKE_CURRENT_SOURCE_DIR}/local_paths.cmake")
else()
  message(FATAL_ERROR 
    "local_paths.cmake not found!\n"
    "Please copy local_paths.cmake.example to local_paths.cmake and update with your local paths:\n"
    "  - OPENCV_ROOT: Path to OpenCV build directory\n"
  )
endif()

# 1. Include Headers
include_directories("${OPENCV_ROOT}/include")

# 2. Link Libraries
# Use generator expressions to link Debug lib in Debug mode, and Release lib in Release mode
target_link_libraries(${BINARY_NAME} PRIVATE 
  debug "${OPENCV_ROOT}/x64/vc16/lib/opencv_world4120d.lib"
  optimized "${OPENCV_ROOT}/x64/vc16/lib/opencv_world4120.lib"
)

# Copy the DLLs to the build directory so the app can run
# Copy Debug DLL
add_custom_command(TARGET ${BINARY_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
  "${OPENCV_ROOT}/x64/vc16/bin/opencv_world4120d.dll"
  "$<TARGET_FILE_DIR:${BINARY_NAME}>"
)
# Copy Release DLL
add_custom_command(TARGET ${BINARY_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
  "${OPENCV_ROOT}/x64/vc16/bin/opencv_world4120.dll"
  "$<TARGET_FILE_DIR:${BINARY_NAME}>"
)

# Copy models directory
add_custom_command(TARGET ${BINARY_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
  "${CMAKE_CURRENT_SOURCE_DIR}/models"
  "$<TARGET_FILE_DIR:${BINARY_NAME}>/models"
)

# Copy mediamtx.exe
add_custom_command(TARGET ${BINARY_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
  "${CMAKE_CURRENT_SOURCE_DIR}/../../server/bin/mediamtx.exe"
  "$<TARGET_FILE_DIR:${BINARY_NAME}>/mediamtx.exe"
)

# Copy mediamtx.yml
add_custom_command(TARGET ${BINARY_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
  "${CMAKE_CURRENT_SOURCE_DIR}/../../mediamtx.yml"
  "$<TARGET_FILE_DIR:${BINARY_NAME}>/mediamtx.yml"
)

# Copy VDD driver files
add_custom_command(TARGET ${BINARY_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
  "${CMAKE_CURRENT_SOURCE_DIR}/vdd_driver"
  "$<TARGET_FILE_DIR:${BINARY_NAME}>/vdd_driver"
)

# Apply the standard set of build settings. This can be removed for applications
# that need different build settings.
apply_standard_settings(${BINARY_NAME})

# Add preprocessor definitions for the build version.
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION=\"${FLUTTER_VERSION}\"")
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION_MAJOR=${FLUTTER_VERSION_MAJOR}")
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION_MINOR=${FLUTTER_VERSION_MINOR}")
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION_PATCH=${FLUTTER_VERSION_PATCH}")
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION_BUILD=${FLUTTER_VERSION_BUILD}")

# Disable Windows macros that collide with C++ standard library functions.
target_compile_definitions(${BINARY_NAME} PRIVATE "NOMINMAX")

# Add dependency libraries and include directories. Add any application-specific
# dependencies here.
target_link_libraries(${BINARY_NAME} PRIVATE flutter flutter_wrapper_app flutter_wrapper_plugin)
target_link_libraries(${BINARY_NAME} PRIVATE "dwmapi.lib" "d3d11.lib" "dxgi.lib" "setupapi.lib" "winhttp.lib")
target_include_directories(${BINARY_NAME} PRIVATE "${CMAKE_SOURCE_DIR}")

# Run the Flutter tool portions of the build. This must not be removed.
add_dependencies(${BINARY_NAME} flutter_assemble)

# ---------------------------------------------------------------------------
# kaptchi_cli — standalone video-to-PDF whiteboard processor
# Does not depend on Flutter; links only OpenCV + whiteboard canvas sources.
# ---------------------------------------------------------------------------
add_executable(kaptchi_cli
  "cli_main.cpp"
  "whiteboard_canvas.cpp"
  "whiteboard_canvas_process.cpp"
  "whiteboard_enhance.cpp"
  "ai_canvas_checker.cpp"
  "http_client.cpp"
)

target_include_directories(kaptchi_cli PRIVATE "${OPENCV_ROOT}/include")
target_include_directories(kaptchi_cli PRIVATE "${CMAKE_SOURCE_DIR}")

target_link_libraries(kaptchi_cli PRIVATE
  debug   "${OPENCV_ROOT}/x64/vc16/lib/opencv_world4120d.lib"
  optimized "${OPENCV_ROOT}/x64/vc16/lib/opencv_world4120.lib"
  "winhttp.lib"
)

target_compile_definitions(kaptchi_cli PRIVATE "NOMINMAX" "KAPTCHI_CLI=1")

# Use C++17 (required for std::filesystem if used in canvas sources)
target_compile_features(kaptchi_cli PRIVATE cxx_std_17)

# Copy kaptchi_cli.exe next to the main app binary after build
add_custom_command(TARGET kaptchi_cli POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
  "$<TARGET_FILE:kaptchi_cli>"
  "$<TARGET_FILE_DIR:${BINARY_NAME}>"
)
