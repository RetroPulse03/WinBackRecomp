cmake_minimum_required(VERSION 3.20)
project(WinBackRecomp LANGUAGES C CXX)

# WinBack: Covert Operations (USA) - static recompilation for PC.
#
#   RecompiledFuncs/  N64Recomp output (generated from the ROM by tools/regen.sh)
#   rsp/aspMain.cpp   RSPRecomp output (audio microcode)
#   src/game/         game registration and patches
#   src/main/         host app: SDL window, RT64, audio, input
#
# Windows: build with clang-cl (MSVC can't handle the generated functions):
#   cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=RelWithDebInfo

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

if (CMAKE_VERSION VERSION_GREATER_EQUAL "3.24.0")
    cmake_policy(SET CMP0135 NEW)
endif()
if (WIN32)
    add_compile_definitions(_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR NOMINMAX)
endif()

set(DEPS ${CMAKE_SOURCE_DIR}/deps)

# SDL2 (Windows: official VC dev package). RT64's bundled SDL 2.26 headers
# don't compile with recent clang, so RT64 is pointed at these headers too.
if (WIN32)
    include(FetchContent)
    set(WINBACK_SDL2_VERSION "2.30.3")
    # Prefer a pre-downloaded copy in deps/ (some CMake builds can't verify
    # GitHub's TLS certificate).
    set(WINBACK_SDL2_ZIP ${DEPS}/SDL2-devel-${WINBACK_SDL2_VERSION}-VC.zip)
    if (NOT EXISTS ${WINBACK_SDL2_ZIP})
        set(WINBACK_SDL2_ZIP https://github.com/libsdl-org/SDL/releases/download/release-${WINBACK_SDL2_VERSION}/SDL2-devel-${WINBACK_SDL2_VERSION}-VC.zip)
    endif()
    FetchContent_Declare(sdl2
        URL ${WINBACK_SDL2_ZIP}
        URL_HASH SHA256=89999a6f3dc8f5e4fb03aaea6b4ad01747d9a81690f9f9a0674806e750483a38)
    FetchContent_MakeAvailable(sdl2)
endif()

set(RT64_STATIC TRUE)
set(RT64_SDL_WINDOW_VULKAN TRUE)
add_compile_definitions(HLSL_CPU)
add_subdirectory(${DEPS}/rt64 ${CMAKE_BINARY_DIR}/rt64)
target_include_directories(rt64 PRIVATE ${CMAKE_BINARY_DIR}/rt64/src)
if (WIN32)
    target_include_directories(rt64 BEFORE PRIVATE ${sdl2_SOURCE_DIR}/include)
endif()

add_subdirectory(${DEPS}/N64ModernRuntime ${CMAKE_BINARY_DIR}/N64ModernRuntime)

# --- Game library: recompiled code + glue ----------------------------------
file(GLOB WINBACK_FUNC_SRCS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/RecompiledFuncs/funcs_*.c)

add_library(WinBackGame STATIC
    ${WINBACK_FUNC_SRCS}
    RecompiledFuncs/lookup.cpp
    rsp/aspMain.cpp
    src/game/winback_game.cpp
)
target_include_directories(WinBackGame PUBLIC
    ${CMAKE_SOURCE_DIR}/src/game
    ${CMAKE_SOURCE_DIR}/RecompiledFuncs
    ${DEPS}/N64ModernRuntime/N64Recomp/include
)
target_link_libraries(WinBackGame PUBLIC librecomp ultramodern)

if (CMAKE_C_COMPILER_ID MATCHES "Clang")
    set_source_files_properties(${WINBACK_FUNC_SRCS} PROPERTIES COMPILE_OPTIONS
        "-Wno-unused-variable;-Wno-unused-but-set-variable;-Wno-implicit-function-declaration")
endif()
if (MSVC)
    target_compile_options(WinBackGame PRIVATE /bigobj)
endif()
# The recompiled RSP vector unit uses SSE4.1 intrinsics.
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    set_source_files_properties(rsp/aspMain.cpp PROPERTIES COMPILE_OPTIONS "-msse4.1")
endif()

# --- Executable ------------------------------------------------------------
add_executable(WinBackRecompiled
    src/main/main.cpp
    src/main/rt64_context.cpp
    src/main/audio.cpp
    src/main/input.cpp
)
target_include_directories(WinBackRecompiled PRIVATE
    ${CMAKE_SOURCE_DIR}/src/main
    ${DEPS}/rt64/src
    ${DEPS}/rt64/src/rhi
    ${DEPS}/rt64/src/render
    ${DEPS}/rt64/src/contrib
    ${DEPS}/rt64/src/contrib/hlslpp/include
    ${DEPS}/rt64/src/contrib/dxc/inc
    ${CMAKE_BINARY_DIR}/rt64/src
)
target_link_libraries(WinBackRecompiled PRIVATE WinBackGame rt64)

if (WIN32)
    target_include_directories(WinBackRecompiled PRIVATE ${sdl2_SOURCE_DIR}/include)
    target_link_directories(WinBackRecompiled PRIVATE ${sdl2_SOURCE_DIR}/lib/x64)
    target_link_libraries(WinBackRecompiled PRIVATE SDL2 Winmm.lib)
    add_custom_command(TARGET WinBackRecompiled POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${sdl2_SOURCE_DIR}/lib/x64/SDL2.dll"
            "${DEPS}/rt64/src/contrib/dxc/bin/x64/dxil.dll"
            "${DEPS}/rt64/src/contrib/dxc/bin/x64/dxcompiler.dll"
            $<TARGET_FILE_DIR:WinBackRecompiled>)
    # Console subsystem during bring-up so logs are visible.
    set_target_properties(WinBackRecompiled PROPERTIES WIN32_EXECUTABLE FALSE)
else()
    find_package(SDL2 REQUIRED)
    find_package(Threads REQUIRED)
    target_compile_definitions(WinBackRecompiled PRIVATE RT64_SDL_WINDOW_VULKAN)
    target_include_directories(WinBackRecompiled PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(WinBackRecompiled PRIVATE SDL2::SDL2 Threads::Threads ${CMAKE_DL_LIBS})
endif()
