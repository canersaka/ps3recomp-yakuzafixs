cmake_minimum_required(VERSION 3.20)
# Keep in step with the changelog in README.md. This sat at 0.3.0 until August
# 2026 while the project shipped 0.6.x through 0.8.0, so every build printed
# "ps3recomp runtime v0.3.0" and was believed. v0.9.0 is the first TAGGED one.
project(ps3recomp_runtime
    VERSION 0.9.1
    LANGUAGES C CXX
    DESCRIPTION "ps3recomp - Static recompilation runtime for PS3 games"
)

# ---------------------------------------------------------------------------
# Standards
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(PS3RECOMP_BUILD_TESTS "Build unit tests" OFF)

# ---------------------------------------------------------------------------
# Collect source files
# ---------------------------------------------------------------------------

# HLE library sources
file(GLOB_RECURSE HLE_SOURCES
    "libs/system/*.c"
    "libs/filesystem/*.c"
    "libs/input/*.c"
    "libs/audio/*.c"
    "libs/video/*.c"
    "libs/network/*.c"
    "libs/spurs/*.c"
    "libs/sync/*.c"
    "libs/codec/*.c"
    "libs/font/*.c"
    "libs/misc/*.c"
    "libs/hardware/*.c"
    "runtime/diag/*.c"      # yz_frontier_trace, used by the live draw engine
)
# Exclude test harnesses (e.g. libs/video/tests/*): they define their own main().
list(FILTER HLE_SOURCES EXCLUDE REGEX "/tests/")

# The Metal backend is Objective-C and Apple-only, so it is added explicitly
# rather than globbed. It needs no platform #ifdef of its own as a result.
if(APPLE)
    enable_language(OBJC)
    list(APPEND HLE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/libs/video/rsx_metal_backend.m")
endif()
# The HLSL -> MSL translator is C++ (it drives glslang's C++ API) and compiles
# to a stub wherever the shader tools are absent, so it is part of the
# library everywhere. See the "Guest shaders on Metal" section below.
list(APPEND HLE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/libs/video/rsx_shader_msl.cpp")

# Runtime core (syscalls, PPU, SPU, memory)
file(GLOB_RECURSE RUNTIME_SOURCES
    "runtime/*.c"
    "runtime/*.cpp"
)
# Exclude:
#  - test harnesses (runtime/*/tests/*): they have their own main() and #include
#    per-game generated headers (spu_recomp.h) that a plain library build lacks.
#  - the PPU boot scaffold (runtime/ppu/, runtime/host/): these are compiled
#    per-game against the lifter-generated ppu_recomp.h (and the game's RSX/D3D12
#    backend), not as part of the game-agnostic runtime library. See
#    docs/PPU_RECOMP.md.
list(FILTER RUNTIME_SOURCES EXCLUDE REGEX "/tests/|/runtime/ppu/|/runtime/host/")

# ---------------------------------------------------------------------------
# Static library
# ---------------------------------------------------------------------------
add_library(ps3recomp_runtime STATIC
    ${HLE_SOURCES}
    ${RUNTIME_SOURCES}
)

target_include_directories(ps3recomp_runtime PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# HLE modules include their own headers
target_include_directories(ps3recomp_runtime PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/system"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/filesystem"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/input"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/audio"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/video"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/network"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/spurs"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/sync"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/codec"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/font"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/misc"
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime/spu"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# Platform-specific link libraries
if(WIN32)
    target_link_libraries(ps3recomp_runtime PRIVATE
        ws2_32      # Winsock for network modules
        xinput      # XInput for gamepad
        ole32       # COM for WASAPI audio
        bcrypt      # BCryptGenRandom for SSL RNG
    )
else()
    # cellPad and cellAudio select their SDL2 backends unconditionally off
    # Windows (see PAD_BACKEND_SDL2 / AUDIO_BACKEND_SDL2), so SDL2 is required
    # here -- docs/BUILDING.md already lists it as the Linux/macOS dependency.
    find_package(SDL2 QUIET)
    if(SDL2_FOUND)
        if(TARGET SDL2::SDL2)
            target_link_libraries(ps3recomp_runtime PRIVATE SDL2::SDL2)
        else()
            target_include_directories(ps3recomp_runtime PRIVATE ${SDL2_INCLUDE_DIRS})
            target_link_libraries(ps3recomp_runtime PRIVATE ${SDL2_LIBRARIES})
        endif()
        message(STATUS "  SDL2:         found (input + audio backends enabled)")
    else()
        message(FATAL_ERROR
            "SDL2 not found. It provides the cellPad and cellAudio backends on "
            "non-Windows hosts.\n"
            "  macOS:  brew install sdl2\n"
            "  Debian: apt install libsdl2-dev\n"
            "If SDL2 is installed in a non-standard prefix, pass "
            "-DCMAKE_PREFIX_PATH=<prefix>.")
    endif()

    find_package(Threads REQUIRED)
    target_link_libraries(ps3recomp_runtime PRIVATE Threads::Threads)
endif()

if(APPLE)
    target_link_libraries(ps3recomp_runtime PUBLIC
        "-framework Metal"
        "-framework QuartzCore"
        "-framework Foundation"
        "-framework AppKit"
    )
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/libs/video/rsx_metal_backend.m"
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    message(STATUS "  Metal:        RSX backend enabled (rsx_metal_backend.m)")
endif()

# ---------------------------------------------------------------------------
# Guest shaders on Metal: HLSL -> SPIR-V -> MSL
# ---------------------------------------------------------------------------
# The RSX decompilers emit HLSL for the D3D12 backend. Metal cannot compile
# that, so libs/video/rsx_shader_msl.cpp lowers it with glslang (HLSL ->
# SPIR-V) and spirv-cross (SPIR-V -> MSL). Both are optional: without them the
# module compiles to a stub, rsx_hlsl_to_msl_available() says so, and the
# Metal backend stays on its fixed-function path. The translation has no GPU
# in it, so it is built and tested on Linux as well -- that is where the
# decompilers' HLSL meets glslang's front end on every push.
#
#   macOS:  brew install glslang spirv-cross
#   Debian: apt install glslang-dev spirv-tools libspirv-cross-c-shared-dev
set(PS3RECOMP_MSL_TRANSLATION OFF)
if(UNIX)
    find_package(glslang CONFIG QUIET)

    # spirv-cross's C API. Debian ships it as a shared library and Homebrew as
    # a static one, under different pkg-config names -- and pkg-config itself
    # is not a given on a Mac, so fall back to a plain header + library search.
    set(SPVC_OK OFF)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(SPVC QUIET spirv-cross-c-shared)
        if(NOT SPVC_FOUND)
            pkg_check_modules(SPVC QUIET spirv-cross-c)
        endif()
        if(SPVC_FOUND)
            set(SPVC_INC     ${SPVC_INCLUDE_DIRS})
            set(SPVC_LIBS    ${SPVC_LINK_LIBRARIES})
            set(SPVC_LIBDIRS ${SPVC_LIBRARY_DIRS})
            set(SPVC_OK ON)
        endif()
    endif()
    if(NOT SPVC_OK)
        find_path(SPVC_INC_DIR spirv_cross_c.h PATH_SUFFIXES spirv_cross)
        find_library(SPVC_LIB NAMES spirv-cross-c-shared spirv-cross-c)
        if(SPVC_INC_DIR AND SPVC_LIB)
            set(SPVC_INC  ${SPVC_INC_DIR})
            set(SPVC_LIBS ${SPVC_LIB})
            get_filename_component(SPVC_LIBDIRS "${SPVC_LIB}" DIRECTORY)
            set(SPVC_OK ON)
        endif()
    endif()

    if(glslang_FOUND AND SPVC_OK
       AND TARGET glslang::glslang AND TARGET glslang::SPIRV)
        add_library(ps3recomp_msl_translation INTERFACE)
        target_compile_definitions(ps3recomp_msl_translation INTERFACE
            PS3RECOMP_HAVE_MSL_TRANSLATION=1)
        target_include_directories(ps3recomp_msl_translation INTERFACE ${SPVC_INC})
        target_link_libraries(ps3recomp_msl_translation INTERFACE
            glslang::glslang glslang::SPIRV ${SPVC_LIBS})
        if(TARGET glslang::glslang-default-resource-limits)
            target_link_libraries(ps3recomp_msl_translation INTERFACE
                glslang::glslang-default-resource-limits)
        endif()
        # A static spirv-cross C API (Homebrew's) needs the backends it was
        # built from, and neither pkg-config nor the library name says so:
        # pick up the static siblings next to it. The shared one carries
        # its own.
        if(SPVC_LIBS MATCHES "\\.a$")
            foreach(part msl glsl hlsl cpp reflect util core)
                find_library(SPVC_PART_${part} NAMES spirv-cross-${part}
                    HINTS ${SPVC_LIBDIRS} NO_DEFAULT_PATH)
                if(SPVC_PART_${part})
                    target_link_libraries(ps3recomp_msl_translation INTERFACE
                        ${SPVC_PART_${part}})
                endif()
            endforeach()
        endif()
        target_link_libraries(ps3recomp_runtime PRIVATE ps3recomp_msl_translation)
        set(PS3RECOMP_MSL_TRANSLATION ON)
        message(STATUS "  Shaders:      guest programs translated to MSL (glslang ${glslang_VERSION}, ${SPVC_LIBS})")
    else()
        message(STATUS "  Shaders:      glslang/spirv-cross not found; Metal backend stays fixed-function")
    endif()
endif()

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(ps3recomp_runtime PRIVATE
        -Wall -Wextra -Wno-unused-parameter
        -fno-strict-aliasing
        # No spontaneous fused multiply-add. MSVC never contracts, and x86-64
        # without -mfma has nothing to contract into, so every validated
        # result came from separate multiplies and adds; arm64 has FMA in
        # the base ISA and Clang fuses by default there. A PPC fmadd is
        # spelt as one where the lifter means one.
        -ffp-contract=off
    )
elseif(MSVC)
    target_compile_options(ps3recomp_runtime PRIVATE
        /W3 /wd4100 /wd4244 /wd4267 /wd4996
        /Zc:__cplusplus
        /experimental:c11atomics   # Enable C11 <stdatomic.h> support
    )
    target_compile_definitions(ps3recomp_runtime PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(PS3RECOMP_BUILD_TESTS)
    include(CTest)
    # sync_stress is written directly against the Win32 threading API
    # (WaitForMultipleObjects, joinable HANDLEs, Interlocked*, CRITICAL_SECTION).
    # Off Windows all of that comes from runtime/platform/win32_compat.h, so
    # the suite runs everywhere -- and is the arm64 memory-model regression
    # net for the lv2 sync primitives.
    add_subdirectory(tests/sync_stress)
endif()

# ---------------------------------------------------------------------------
# POSIX host harness
# ---------------------------------------------------------------------------
# Drives the cellGcm -> RSX -> backend bridge with no lifted game, so a backend
# can be brought up and regression-tested without a game binary or a recompiler
# run. Builds on any POSIX host: it picks Metal on Apple and the null backend's
# headless software path everywhere else, so Linux -- which has no real backend
# yet -- can still run the whole guest command stream end to end in CI.
if(UNIX)
    add_executable(ps3recomp_host
        runtime/host/host_posix.c
        runtime/host/host_stubs.c
    )
    target_link_libraries(ps3recomp_host PRIVATE ps3recomp_runtime)
    target_include_directories(ps3recomp_host PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/libs/video"
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    # The runtime carries C++ (and, with translation on, spirv-cross), so the
    # C++ driver must do the final link or the standard library goes missing.
    set_target_properties(ps3recomp_host PROPERTIES LINKER_LANGUAGE CXX)

    # Guest shader translation, decompilers -> HLSL -> MSL, with no GPU: the
    # regression net for the Metal backend's shader path on every platform.
    if(PS3RECOMP_MSL_TRANSLATION)
        add_executable(test_shader_msl
            libs/video/tests/test_shader_msl.c
            libs/video/rsx_shader_msl.cpp
            libs/video/rsx_vp_decompiler.c
            libs/video/rsx_fp_decompiler.c
        )
        target_include_directories(test_shader_msl PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/libs/video"
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
        )
        target_link_libraries(test_shader_msl PRIVATE ps3recomp_msl_translation)
    endif()
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS ps3recomp_runtime
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)

install(DIRECTORY include/ps3emu
    DESTINATION include
    FILES_MATCHING PATTERN "*.h"
)

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "ps3recomp runtime v${PROJECT_VERSION}")
message(STATUS "  C compiler:   ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
message(STATUS "  C++ compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Build type:   ${CMAKE_BUILD_TYPE}")
message(STATUS "")
