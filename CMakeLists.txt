cmake_minimum_required(VERSION 3.20)
project(HD VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(HD_BUILD_APP   "Build the HD desktop application (needs GLFW + Dear ImGui, fetched automatically)" ON)
option(HD_BUILD_TESTS "Build the headless core tests" ON)
option(HD_BUILD_OFFSCREEN_RENDER_TEST "Linux only: render the viewport offscreen via EGL (developer check)" OFF)
option(HD_BUILD_UI_SCREENSHOT_TEST "Linux only: run the UI headlessly via EGL and save screenshots (developer check)" OFF)

# ------------------------------------------------------------------ core engine
add_library(hd_core STATIC
  src/core/Demo.cpp
  src/core/Expr.cpp
  src/core/GeoIO.cpp
  src/core/Geometry.cpp
  src/core/Json.cpp
  src/core/Node.cpp
  src/core/Nodes.cpp
  src/core/Noise.cpp
  src/core/Ops.cpp
  src/core/PathTracer.cpp
  src/core/Pyro.cpp
  src/core/Sim.cpp
  src/core/Volume.cpp
)
target_include_directories(hd_core PUBLIC src)
find_package(Threads REQUIRED)
target_link_libraries(hd_core PUBLIC Threads::Threads)
if(MSVC)
  target_compile_options(hd_core PRIVATE /W3 /utf-8)
  target_compile_definitions(hd_core PUBLIC _USE_MATH_DEFINES NOMINMAX _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(hd_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# headless command-line tool (cook / export / demos)
add_executable(hdc src/tools/hdc.cpp)
target_link_libraries(hdc PRIVATE hd_core)

if(HD_BUILD_TESTS)
  enable_testing()
  add_executable(hd_tests tests/test_core.cpp)
  target_link_libraries(hd_tests PRIVATE hd_core)
  add_test(NAME hd_tests COMMAND hd_tests)
endif()

# ------------------------------------------------------------------ renderer (OpenGL 3.3 core)
add_library(hd_render STATIC
  src/render/GL.cpp
  src/render/Renderer.cpp
)
target_include_directories(hd_render PUBLIC src)
target_link_libraries(hd_render PUBLIC hd_core)
if(APPLE)
  target_compile_definitions(hd_render PUBLIC GL_SILENCE_DEPRECATION)
endif()
if(NOT WIN32)
  target_link_libraries(hd_render PUBLIC ${CMAKE_DL_LIBS})
endif()

if(HD_BUILD_OFFSCREEN_RENDER_TEST AND UNIX AND NOT APPLE)
  add_executable(hd_render_test tests/render_offscreen.cpp)
  target_link_libraries(hd_render_test PRIVATE hd_render)
endif()

# ------------------------------------------------------------------ application
if(HD_BUILD_APP)
  include(FetchContent)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
    GIT_SHALLOW TRUE)
  FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.9b-docking
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(glfw imgui)

  add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
  )
  target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
  target_link_libraries(imgui PUBLIC glfw)
  target_compile_definitions(imgui PUBLIC GLFW_INCLUDE_NONE)

  add_library(hd_app STATIC
    src/app/App.cpp
    src/app/Assets.cpp
    src/app/NetworkEditor.cpp
    src/app/ParmPane.cpp
    src/app/Menus.cpp
    src/app/Panels.cpp
    src/app/RenderView.cpp
    src/app/Theme.cpp
  )
  target_link_libraries(hd_app PUBLIC hd_render imgui glfw)

  set(HD_APP_SOURCES src/app/main.cpp)
  if(APPLE)
    add_executable(HD MACOSX_BUNDLE ${HD_APP_SOURCES})
    set_target_properties(HD PROPERTIES
      MACOSX_BUNDLE_BUNDLE_NAME "HD"
      MACOSX_BUNDLE_GUI_IDENTIFIER "com.vfxchris581.hd"
      MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})
  elseif(WIN32)
    add_executable(HD WIN32 ${HD_APP_SOURCES})
  else()
    add_executable(HD ${HD_APP_SOURCES})
  endif()
  target_link_libraries(HD PRIVATE hd_app)
  if(WIN32)
    target_link_libraries(HD PRIVATE opengl32)
    if(MSVC)
      target_link_options(HD PRIVATE /ENTRY:mainCRTStartup)
    endif()
  elseif(APPLE)
    target_link_libraries(HD PRIVATE "-framework OpenGL" "-framework Cocoa" "-framework IOKit")
  endif()
  # Linux developer check: run the whole UI headlessly and write screenshots
  if(HD_BUILD_UI_SCREENSHOT_TEST AND UNIX AND NOT APPLE)
    add_executable(hd_ui_screenshot tests/ui_screenshot.cpp)
    target_link_libraries(hd_ui_screenshot PRIVATE hd_app ${CMAKE_DL_LIBS})
  endif()

  # Ship example scenes next to the executable
  add_custom_command(TARGET HD POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/examples $<TARGET_FILE_DIR:HD>/examples)
endif()
