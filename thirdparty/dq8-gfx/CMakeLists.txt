# Hardware GS backend (SDL3 GPU) from DQ8-Recompiled, see README.md.
# Built only when RG_GS_SDLGPU is on; the game then picks it at run time.

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/Dq8HostSimd.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/Dq8Shaders.cmake")

# SDL3, built from source and linked statically: nothing to install.
include(FetchContent)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
# The backend uses SDL's GPU API, not SDL_render. Its Direct3D and Vulkan
# renderers bring SDL_d3dmath.c, whose global MatrixIdentity/MatrixMultiply
# clash with raylib's at link time.
set(SDL_RENDER_D3D OFF CACHE BOOL "" FORCE)
set(SDL_RENDER_D3D11 OFF CACHE BOOL "" FORCE)
set(SDL_RENDER_D3D12 OFF CACHE BOOL "" FORCE)
set(SDL_RENDER_VULKAN OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.2.24
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE)
FetchContent_MakeAvailable(SDL3)

# Backend-neutral GS support: local memory, swizzled access, transfers.
add_library(dq8_gfx_gs STATIC
    src/gfx/gs/gs_vram.cpp
    src/gfx/gs/gs_transfer.cpp
    src/gfx/gs/gs_state.cpp
)
target_include_directories(dq8_gfx_gs PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(dq8_gfx_gs PUBLIC dq8_host_simd ps2_runtime)

add_library(dq8_gfx_sdlgpu STATIC
    src/gfx/backends/sdlgpu/sdlgpu_backend.cpp
    src/gfx/backends/sdlgpu/sdlgpu_device.cpp
    src/gfx/backends/sdlgpu/sdlgpu_display.cpp
    src/gfx/backends/sdlgpu/sdlgpu_input.cpp
    src/gfx/backends/sdlgpu/sdlgpu_targets.cpp
    src/gfx/backends/sdlgpu/sdlgpu_textures.cpp
    window_icon.cpp
)
target_link_libraries(dq8_gfx_sdlgpu PUBLIC dq8_gfx_gs SDL3::SDL3-static)

foreach(shader IN ITEMS
        "gs_draw.vert;vert;kGsDrawVertSpirv"
        "gs_draw.frag;frag;kGsDrawFragSpirv"
        "gs_reinterpret.vert;vert;kGsReinterpretVertSpirv"
        "gs_reinterpret.frag;frag;kGsReinterpretFragSpirv"
        "gs_display.frag;frag;kGsDisplayFragSpirv"
        "gs_index8.frag;frag;kGsIndex8FragSpirv"
        "gs_local_copy.frag;frag;kGsLocalCopyFragSpirv")
    list(GET shader 0 file)
    list(GET shader 1 stage)
    list(GET shader 2 symbol)
    dq8_add_shader(dq8_gfx_sdlgpu
        SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/backends/sdlgpu/shaders/${file}"
        STAGE ${stage} SYMBOL ${symbol})
endforeach()
