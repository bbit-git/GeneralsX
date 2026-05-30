# d3d8gles3 — Direct3D 8 -> OpenGL ES 3.0 backend (Android/mobile).
# Built only when SAGE_USE_GLES3 is ON. Produces a static lib that exports the
# same D3D8 COM ABI (Direct3DCreate8 + IDirect3D*8 interfaces) the WW3D2 engine
# already links against, so it is a drop-in replacement for the DXVK binary.

add_library(d3d8gles3 STATIC
    src/gl_state_map.cpp
    src/ffp_shader_gen.cpp
    src/device_gles3.cpp
    src/resources_gles3.cpp
    src/d3d8_entry.cpp
)

target_include_directories(d3d8gles3
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include   # our Direct3DCreate8 entry decl
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Reuse the same d3d8.h ABI the rest of the tree compiles against. On Android
# DXVK_INCLUDE_DIR is populated (headers-only) by cmake/gles3.cmake.
if(DXVK_INCLUDE_DIR)
    target_include_directories(d3d8gles3 PUBLIC ${DXVK_INCLUDE_DIR})
endif()

# GLES3 + EGL. On Android these come from the NDK sysroot; SDL3 owns context
# creation/swap, so we only need the client API libs here.
if(ANDROID)
    target_link_libraries(d3d8gles3 PUBLIC GLESv3 EGL android log)
else()
    # Desktop GLES3 (e.g. ANGLE / Mesa) for development & host unit tests.
    find_package(OpenGL REQUIRED)
    target_link_libraries(d3d8gles3 PUBLIC ${OPENGL_LIBRARIES})
endif()

# SDL3 supplies the GL context/window the device binds to.
if(TARGET SDL3::SDL3)
    target_link_libraries(d3d8gles3 PUBLIC SDL3::SDL3)
endif()

target_compile_features(d3d8gles3 PUBLIC cxx_std_17)
target_compile_definitions(d3d8gles3 PUBLIC SAGE_USE_GLES3)

if(RTS_BUILD_CORE_EXTRAS)
    add_subdirectory(tests)
endif()
