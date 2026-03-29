cmake_minimum_required(VERSION 3.18)
project(keystrokes LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -fvisibility=hidden -ffunction-sections -fdata-sections -fexceptions -w")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -fvisibility=hidden -ffunction-sections -fdata-sections -w")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--gc-sections -Wl,--allow-shlib-undefined")

include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/include/ImGui
    ${CMAKE_SOURCE_DIR}/include/ImGui/backends
)

set(SOURCES
    libkeystrokes.cpp
    include/ImGui/imgui.cpp
    include/ImGui/imgui_draw.cpp
    include/ImGui/imgui_tables.cpp
    include/ImGui/imgui_widgets.cpp
    include/ImGui/backends/imgui_impl_opengl3.cpp
    include/ImGui/backends/imgui_impl_android.cpp
)

add_library(keystrokes SHARED ${SOURCES})
target_link_libraries(keystrokes log android EGL GLESv3 GLESv2)
