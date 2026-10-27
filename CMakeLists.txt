cmake_minimum_required(VERSION 3.20)
project(Nexus CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The PC preview is built with CMake (SDL2 is only used for the window, input and audio).
# The PS5 app is built by tools/build-ps5-native.sh (inside WSL) with the PS5 payload SDK.
# src/ui, src/core and src/platform/platform.h are shared; only src/platform/<target> differs.
set(SDL2_ROOT "" CACHE PATH "SDL2 mingw dev root (contains x86_64-w64-mingw32)")
set(ARCH x86_64-w64-mingw32)

add_executable(nexus
    src/main.cpp
    src/ui/app.cpp
    src/ui/gfx.cpp
    src/ui/brand.cpp
    src/ui/stb_impl.cpp
    src/core/sfx_synth.cpp
    src/core/music_synth.cpp
    src/core/mixer.cpp
    src/platform/sample_data.cpp
    src/platform/pc/pc_platform.cpp)

target_include_directories(nexus PRIVATE ${SDL2_ROOT}/${ARCH}/include/SDL2)
target_link_directories(nexus PRIVATE ${SDL2_ROOT}/${ARCH}/lib)
target_link_libraries(nexus PRIVATE mingw32 SDL2main SDL2)
target_compile_options(nexus PRIVATE -Wall -Wextra -Wno-unused-parameter)
