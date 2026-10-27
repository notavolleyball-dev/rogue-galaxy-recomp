# Rogue Galaxy Recompiled: the native executable.
#
# Links the C++ that ps2_recomp generated from your own disc (build/generated/,
# written by `python setup.py recompile`, never committed) against the
# PS2Recomp runtime in thirdparty/PS2Recomp.
#
#   cmake -S . -B build/game -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/game

cmake_minimum_required(VERSION 3.24)

project(RogueGalaxyRecomp
    VERSION 0.1.0
    DESCRIPTION "Rogue Galaxy (PS2) static recompilation"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(RG_VERSION "SCUS_974.90" CACHE STRING "Game release (config/<version>)")
set(RG_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build/generated" CACHE PATH
    "Parent of the per-image ps2_recomp outputs written by setup.py recompile")

# The framework's runtime only: the recompiler and analyzer are built
# separately (build/tools, see README), and the runner, tests and studio are
# not needed. EXCLUDE_FROM_ALL builds just what the game links.
set(PS2X_BUILD_RECOMP OFF)
set(PS2X_BUILD_ANALYZER OFF)
set(PS2X_BUILD_TEST OFF)
set(PS2X_BUILD_STUDIO OFF)
set(PS2X_BUILD_RUNTIME ON)
set(PS2X_ENABLE_DEBUG_UI OFF)

# VU1 microprograms compiled to C++ ahead of time, from two places:
# build/vu1-manifest, which setup.py recompile rebuilds from
# config/<version>/vu1-programs.txt and the user's disc, and build/vu1-profile,
# what local runs recorded with PS2_VU_PROGRAM_PROFILE. Both are game code:
# they stay in build/ and are never committed.
set(RG_VU1_PROFILES "${CMAKE_CURRENT_SOURCE_DIR}/build/vu1-manifest;${CMAKE_CURRENT_SOURCE_DIR}/build/vu1-profile"
    CACHE STRING "VU1 program directories to compile into the runtime, where they exist")
set(rg_vu1_found)
foreach(dir IN LISTS RG_VU1_PROFILES)
    if(EXISTS "${dir}/entries.txt")
        list(APPEND rg_vu1_found "${dir}")
    endif()
endforeach()
set(PS2X_VU_PROGRAM_PROFILES "${rg_vu1_found}" CACHE STRING
    "Local VU1 program profile directories to compile; never distributed" FORCE)
message(STATUS "VU1 programs: ${rg_vu1_found}")

# VU blocks: what the interpreter still ran, recorded with
# PS2_VU_AOT_PROFILE=FILE (VU0 micro programs above all, which the whole-
# program compile above does not cover) and kept in build/vu-blocks/*.txt,
# compiled as blocks (PS2X_VU_AOT_IMAGES). Game code too: never committed.
# One block compilation at a time: each shard takes gigabytes to compile.
file(GLOB rg_vu_blocks CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/build/vu-blocks/*.txt")
list(SORT rg_vu_blocks)
set(PS2X_VU_AOT_IMAGES "${rg_vu_blocks}" CACHE STRING
    "Local VU code images or block profiles to compile; never distributed" FORCE)
set(PS2X_VU_AOT_COMPILE_JOBS "1" CACHE STRING
    "Maximum concurrent runtime compilations with VU AOT (Ninja)" FORCE)
message(STATUS "VU blocks: ${rg_vu_blocks}")

# Local changes to the pinned framework (patches/ps2recomp), applied to the
# checkout once; `python setup.py tools` applies them too, before it builds
# the recompiler.
find_package(Git REQUIRED)
include(cmake/framework_patches.cmake)

# Binary folders not named thirdparty/: the runtime's FFmpeg download goes to
# ThirdParty/ffmpeg-prefix under the build folder, and with a thirdparty/
# there already CMake wrote the download's outputs with the folder's case
# and the game's link inputs with the other; Ninja, matching them by case,
# had no rule for the libraries, and a fresh build stopped at once
# (2026-10-26; an existing build had them from before).
add_subdirectory(thirdparty/PS2Recomp "${CMAKE_BINARY_DIR}/ps2recomp" EXCLUDE_FROM_ALL)

# The runtime asks for link-time optimization in Release. With clang on
# MinGW (COFF) that loses the TLS initialization routine of an inline
# thread_local with a destructor (ps2_vu_detail::regionCounters) and the link
# fails; the generated code, most of the executable, is not LTO'd anyway.
set_property(TARGET ps2_runtime PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF)

# The hardware GS backend (SDL3 GPU, from DQ8-Recompiled). Configuring
# downloads and builds SDL3, and glslang when no GLSL compiler is installed.
option(RG_GS_SDLGPU "Build the SDL3 GPU GS backend" ON)
if(RG_GS_SDLGPU)
    add_subdirectory(thirdparty/dq8-gfx "${CMAKE_BINARY_DIR}/dq8-gfx")
endif()

add_subdirectory(src)
