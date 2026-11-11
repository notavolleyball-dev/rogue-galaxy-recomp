# The renderer and presenter (SDL3 GPU), from DQ8-Recompiled's hardware GS
# backend, see README.md. Rogue Galaxy: since M1 of the game's
# docs/NATIVE-ONLY.md no GS is emulated here: the native renderer draws the
# game's records and the presenter shows them.

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
# Patched (Rogue Galaxy: added): with DQ8_GFX_GPU_TIMES=1, a GPU timestamp at
# each debug label, the GPU's time logged by label (cmake/patches).
FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.2.24
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE
    PATCH_COMMAND "${CMAKE_COMMAND}"
        -DPATCH=${CMAKE_CURRENT_SOURCE_DIR}/cmake/patches/sdl3-gpu-timestamps.patch
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Dq8ApplyPatch.cmake")
FetchContent_MakeAvailable(SDL3)

# The native pipeline (Rogue Galaxy: added; the game's docs/NATIVE-PIPELINE.md):
# records and the renderer that takes them. It links SDL3 alone, not the
# runtime, so nothing of the GS compiles here: the runtime's GS headers are
# not on its include path. Keep it that way; that is the guarantee.
add_library(dq8_gfx_native STATIC
    src/gfx/native/native_pipeline.cpp
    src/gfx/native/native_meshes.cpp
    src/gfx/native/native_passes.cpp
)
target_include_directories(dq8_gfx_native PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(dq8_gfx_native PUBLIC SDL3::SDL3-static)
foreach(shader IN ITEMS
        "native_view.vert;vert;kNativeViewVertSpirv"
        "native_view.frag;frag;kNativeViewFragSpirv"
        "native_mesh.vert;vert;kNativeMeshVertSpirv"
        "native_model.vert;vert;kNativeModelVertSpirv"
        "native_mesh.frag;frag;kNativeMeshFragSpirv"
        "native_resolve.vert;vert;kNativeResolveVertSpirv"
        "native_resolve.frag;frag;kNativeResolveFragSpirv")
    list(GET shader 0 file)
    list(GET shader 1 stage)
    list(GET shader 2 symbol)
    dq8_add_shader(dq8_gfx_native
        SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/native/shaders/${file}"
        STAGE ${stage} SYMBOL ${symbol})
endforeach()

# The presenter: the window, the device, the native renderer's frames fitted
# into the window, and the host's input. It links the native library and
# SDL3, not the runtime.
add_library(dq8_gfx_sdlgpu STATIC
    src/gfx/backends/sdlgpu/sdlgpu_presenter.cpp
    src/gfx/backends/sdlgpu/sdlgpu_device.cpp
    src/gfx/backends/sdlgpu/sdlgpu_display.cpp
    src/gfx/backends/sdlgpu/sdlgpu_input.cpp
    window_icon.cpp
)
target_include_directories(dq8_gfx_sdlgpu PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(dq8_gfx_sdlgpu PUBLIC dq8_gfx_native SDL3::SDL3-static)

foreach(shader IN ITEMS
        "present_triangle.vert;vert;kPresentTriangleVertSpirv"
        "present_sides.frag;frag;kPresentSidesFragSpirv"
        "present_fsr.frag;frag;kPresentFsrFragSpirv")
    list(GET shader 0 file)
    list(GET shader 1 stage)
    list(GET shader 2 symbol)
    dq8_add_shader(dq8_gfx_sdlgpu
        SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/backends/sdlgpu/shaders/${file}"
        STAGE ${stage} SYMBOL ${symbol})
endforeach()

# The in-game menu (src/ui), drawn with Dear ImGui over the backend's window.
# Dear ImGui is the submodule at thirdparty/imgui, the commit DQ8 pins.
set(DQ8_IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../imgui")
if(NOT EXISTS "${DQ8_IMGUI_DIR}/imgui.cpp")
    message(FATAL_ERROR
        "Dear ImGui not found under ${DQ8_IMGUI_DIR}. "
        "Run: git submodule update --init thirdparty/imgui")
endif()

add_library(dq8_imgui STATIC
    "${DQ8_IMGUI_DIR}/imgui.cpp"
    "${DQ8_IMGUI_DIR}/imgui_draw.cpp"
    "${DQ8_IMGUI_DIR}/imgui_tables.cpp"
    "${DQ8_IMGUI_DIR}/imgui_widgets.cpp"
    "${DQ8_IMGUI_DIR}/backends/imgui_impl_sdl3.cpp"
    "${DQ8_IMGUI_DIR}/backends/imgui_impl_sdlgpu3.cpp"
)
target_include_directories(dq8_imgui SYSTEM PUBLIC "${DQ8_IMGUI_DIR}" "${DQ8_IMGUI_DIR}/backends")
target_compile_definitions(dq8_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_link_libraries(dq8_imgui PUBLIC SDL3::SDL3-static)

# Roboto Medium from Dear ImGui's own fonts (Apache 2.0), compiled in so the
# menu needs no files beside the executable.
set(DQ8_UI_FONT "${DQ8_IMGUI_DIR}/misc/fonts/Roboto-Medium.ttf")
set(DQ8_UI_FONT_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/generated/ui_font_data.cpp")
add_custom_command(
    OUTPUT "${DQ8_UI_FONT_SOURCE}"
    COMMAND "${CMAKE_COMMAND}" "-DINPUT=${DQ8_UI_FONT}" "-DOUTPUT=${DQ8_UI_FONT_SOURCE}"
            -DSYMBOL=kUiFontData -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Dq8EmbedBinary.cmake"
    DEPENDS "${DQ8_UI_FONT}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Dq8EmbedBinary.cmake"
    COMMENT "Embedding the menu font"
    VERBATIM)

add_library(dq8_ui STATIC
    src/ui/ui_glyphs.cpp
    src/ui/ui_overlay.cpp
    src/ui/ui_pages.cpp
    src/ui/ui_script.cpp
    src/ui/ui_settings.cpp
    src/ui/ui_style.cpp
    src/ui/ui_widgets.cpp
    "${DQ8_UI_FONT_SOURCE}"
)
target_link_libraries(dq8_ui PUBLIC dq8_gfx_sdlgpu dq8_imgui)
