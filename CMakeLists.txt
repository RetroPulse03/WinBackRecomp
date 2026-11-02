cmake_minimum_required(VERSION 3.20)
project(WinBackRecomp VERSION 0.2.0 LANGUAGES C CXX)
set(WINBACK_VERSION_SUFFIX "" CACHE STRING "Appended to the version, e.g. -dev")

# WinBack: Covert Operations (USA) - static recompilation for PC.
#
#   generated/RecompiledFuncs/  N64Recomp output (generated from the ROM by tools/regen.sh)
#   generated/rsp/aspMain.cpp   RSPRecomp output (audio microcode)
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
    # SDL2's headers define _m_prefetch themselves unless this guard is set;
    # recent clang provides it as a builtin and rejects the redefinition.
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        add_compile_definitions(__PRFCHWINTRIN_H)
    endif()
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
# RT64 builds with -Werror; newer compilers add warnings in its contrib code.
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(rt64 PRIVATE -Wno-error)
endif()
if (WIN32)
    target_include_directories(rt64 BEFORE PRIVATE ${sdl2_SOURCE_DIR}/include)
endif()

# RmlUi draws the overlay menu (src/ui/) on top of the game through RT64's
# render hooks. Static build with FreeType; on Windows FreeType comes from the
# prebuilt static library in deps/freetype-windows-binaries.
if (WIN32)
    set(WINBACK_FREETYPE ${DEPS}/freetype-windows-binaries)
    add_library(Freetype::Freetype STATIC IMPORTED)
    set_target_properties(Freetype::Freetype PROPERTIES
        IMPORTED_LOCATION "${WINBACK_FREETYPE}/release static/vs2015-2022/win64/freetype.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${WINBACK_FREETYPE}/include")
endif()
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(RMLUI_SAMPLES OFF CACHE BOOL "" FORCE)
set(RMLUI_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
add_subdirectory(${DEPS}/RmlUi ${CMAKE_BINARY_DIR}/RmlUi)

add_subdirectory(${DEPS}/N64ModernRuntime ${CMAKE_BINARY_DIR}/N64ModernRuntime)
# librecomp's Controller Pak functions are "no pak" stubs; WinBack saves to
# the pak, so src/game/controller_pak.cpp provides a file-backed version.
get_target_property(LIBRECOMP_SRCS librecomp SOURCES)
list(FILTER LIBRECOMP_SRCS EXCLUDE REGEX "src/pak\.cpp$")
set_property(TARGET librecomp PROPERTY SOURCES ${LIBRECOMP_SRCS})

# --- Game library: recompiled code + glue ----------------------------------
file(GLOB WINBACK_FUNC_SRCS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/generated/RecompiledFuncs/funcs_*.c)

add_library(WinBackGame STATIC
    ${WINBACK_FUNC_SRCS}
    generated/RecompiledFuncs/lookup.cpp
    generated/rsp/aspMain.cpp
    src/game/winback_game.cpp
    src/game/controller_pak.cpp
    src/game/debug_stats.cpp
    src/game/level_select.cpp
    src/game/patches.c
)
target_include_directories(WinBackGame PUBLIC
    ${CMAKE_SOURCE_DIR}/src/game
    ${CMAKE_SOURCE_DIR}/generated/RecompiledFuncs
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
    set_source_files_properties(generated/rsp/aspMain.cpp PROPERTIES COMPILE_OPTIONS "-msse4.1")
endif()

# --- Executable ------------------------------------------------------------
add_executable(WinBackRecompiled
    src/main/main.cpp
    src/main/rt64_context.cpp
    src/main/audio.cpp
    src/main/input.cpp
    src/main/display.cpp
    src/main/mods.cpp
    src/ui/menu.cpp
    src/ui/ui_renderer.cpp
)
target_include_directories(WinBackRecompiled PRIVATE
    ${CMAKE_SOURCE_DIR}/src/main
    ${CMAKE_SOURCE_DIR}/src/ui
    ${CMAKE_BINARY_DIR}/src/ui/shaders
    ${CMAKE_BINARY_DIR}/embedded
    ${DEPS}/rt64/src
    ${DEPS}/rt64/src/rhi
    ${DEPS}/rt64/src/render
    ${DEPS}/rt64/src/contrib
    ${DEPS}/rt64/src/contrib/hlslpp/include
    ${DEPS}/rt64/src/contrib/dxc/inc
    ${CMAKE_BINARY_DIR}/rt64/src
)
target_link_libraries(WinBackRecompiled PRIVATE WinBackGame rt64 RmlUi::Core)

# Menu shaders, compiled with RT64's DXC helpers (build_vertex_shader and
# build_pixel_shader read these variables from the calling directory).
if (WIN32)
    set(DXC "${DEPS}/rt64/src/contrib/dxc/bin/x64/dxc.exe")
else()
    set(DXC "LD_LIBRARY_PATH=${DEPS}/rt64/src/contrib/dxc/lib/x64" "${DEPS}/rt64/src/contrib/dxc/bin/x64/dxc-linux")
endif()
set(DXC_COMMON_OPTS "-I${DEPS}/rt64/src")
set(DXC_DXIL_OPTS "-Wno-ignored-attributes")
set(DXC_SPV_OPTS "-spirv" "-fspv-target-env=vulkan1.0" "-fvk-use-dx-layout")
set(DXC_PS_OPTS "${DXC_COMMON_OPTS}" "-E" "PSMain" "-T ps_6_0")
set(DXC_VS_OPTS "${DXC_COMMON_OPTS}" "-E" "VSMain" "-T vs_6_0" "-fvk-invert-y")
build_vertex_shader(WinBackRecompiled "src/ui/shaders/InterfaceVS.hlsl" "src/ui/shaders/InterfaceVS.hlsl")
build_pixel_shader(WinBackRecompiled "src/ui/shaders/InterfacePS.hlsl" "src/ui/shaders/InterfacePS.hlsl")

# Fonts and menu documents are compiled into the executable, so the release
# needs no extra files next to it.
foreach(EMBED
        assets/fonts/Lato-Bold.ttf
        assets/fonts/Lato-BoldItalic.ttf
        assets/fonts/Lato-BlackItalic.ttf
        src/ui/menu.rml)
    get_filename_component(EMBED_NAME ${EMBED} NAME)
    string(MAKE_C_IDENTIFIER ${EMBED_NAME} EMBED_ID)
    set(EMBED_OUT ${CMAKE_BINARY_DIR}/embedded/${EMBED_ID})
    add_custom_command(OUTPUT ${EMBED_OUT}.c ${EMBED_OUT}.h
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/embedded
        COMMAND file_to_c ${CMAKE_SOURCE_DIR}/${EMBED} ${EMBED_ID} ${EMBED_OUT}.c ${EMBED_OUT}.h
        DEPENDS ${CMAKE_SOURCE_DIR}/${EMBED} file_to_c)
    target_sources(WinBackRecompiled PRIVATE ${EMBED_OUT}.c)
endforeach()
target_compile_definitions(WinBackRecompiled PRIVATE
    WINBACK_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    WINBACK_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    WINBACK_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    WINBACK_VERSION_SUFFIX="${WINBACK_VERSION_SUFFIX}")

if (WIN32)
    target_include_directories(WinBackRecompiled PRIVATE ${sdl2_SOURCE_DIR}/include)
    target_link_directories(WinBackRecompiled PRIVATE ${sdl2_SOURCE_DIR}/lib/x64)
    target_link_libraries(WinBackRecompiled PRIVATE SDL2 Winmm.lib Comdlg32.lib)
    add_custom_command(TARGET WinBackRecompiled POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${sdl2_SOURCE_DIR}/lib/x64/SDL2.dll"
            "${DEPS}/rt64/src/contrib/dxc/bin/x64/dxil.dll"
            "${DEPS}/rt64/src/contrib/dxc/bin/x64/dxcompiler.dll"
            $<TARGET_FILE_DIR:WinBackRecompiled>)
    # game/ is the folder to play from: every build refreshes the exe, its
    # symbols and DLLs there, and the game keeps its saves and data beside them.
    add_custom_command(TARGET WinBackRecompiled POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_SOURCE_DIR}/game"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:WinBackRecompiled>
            $<TARGET_PDB_FILE:WinBackRecompiled>
            "${sdl2_SOURCE_DIR}/lib/x64/SDL2.dll"
            "${DEPS}/rt64/src/contrib/dxc/bin/x64/dxil.dll"
            "${DEPS}/rt64/src/contrib/dxc/bin/x64/dxcompiler.dll"
            "${CMAKE_SOURCE_DIR}/game")
    # Version info resource. Release builds use the GUI subsystem (no console
    # window); WINBACK_CONSOLE=ON keeps the console for debugging.
    configure_file(src/main/version.rc.in ${CMAKE_BINARY_DIR}/version.rc @ONLY)
    target_sources(WinBackRecompiled PRIVATE ${CMAKE_BINARY_DIR}/version.rc)
    set_source_files_properties(${CMAKE_BINARY_DIR}/version.rc PROPERTIES
        OBJECT_DEPENDS ${CMAKE_SOURCE_DIR}/src/main/icon/icon.ico)
    option(WINBACK_CONSOLE "Build with a console window" OFF)
    if (NOT WINBACK_CONSOLE)
        set_target_properties(WinBackRecompiled PROPERTIES WIN32_EXECUTABLE TRUE)
        target_link_options(WinBackRecompiled PRIVATE /ENTRY:mainCRTStartup)
    endif()
else()
    find_package(SDL2 REQUIRED)
    find_package(Threads REQUIRED)
    target_compile_definitions(WinBackRecompiled PRIVATE RT64_SDL_WINDOW_VULKAN)
    target_include_directories(WinBackRecompiled PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(WinBackRecompiled PRIVATE SDL2::SDL2 Threads::Threads ${CMAKE_DL_LIBS})
endif()
