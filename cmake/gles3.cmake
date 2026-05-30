# cmake/gles3.cmake — wire the d3d8gles3 (D3D8 -> OpenGL ES 3.0) backend.
#
# Included from the top-level CMakeLists when SAGE_USE_GLES3 is ON, as the
# Android/mobile alternative to the DXVK path in cmake/dx8.cmake. The two are
# mutually exclusive — never fetch both (same rule dx8.cmake enforces).

if(NOT SAGE_USE_GLES3)
    return()
endif()

if(SAGE_USE_DX8)
    message(FATAL_ERROR "SAGE_USE_GLES3 (OpenGL ES backend) and SAGE_USE_DX8 (native DirectX) are mutually exclusive")
endif()

message(STATUS "Backend: d3d8gles3 (Direct3D 8 -> OpenGL ES 3.0)")

# --- D3D8 ABI headers --------------------------------------------------------
# The backend implements the same abstract COM interfaces the engine compiles
# against. Those declarations are platform-agnostic (no Vulkan), so we reuse the
# DXVK *native* headers — fetched headers-only, NOT built. This guarantees the
# engine and our implementation agree on the exact d3d8.h ABI.
#
# TODO(android): vendor the ~6 d3d8*.h headers into Dependencies/d3d8gles3/include
# to drop this fetch entirely for offline NDK builds.
include(FetchContent)
FetchContent_Declare(
    dxvk_headers
    GIT_REPOSITORY https://github.com/doitsujin/dxvk.git
    GIT_TAG        v2.6
    GIT_SHALLOW    TRUE
)
# Populate without configuring/building DXVK's own CMake/Meson project.
FetchContent_GetProperties(dxvk_headers)
if(NOT dxvk_headers_POPULATED)
    FetchContent_Populate(dxvk_headers)
endif()
set(DXVK_INCLUDE_DIR "${dxvk_headers_SOURCE_DIR}/include/native" CACHE PATH "D3D8 ABI headers (from DXVK, headers-only)")
message(STATUS "  D3D8 ABI headers: ${DXVK_INCLUDE_DIR}")

# --- The backend library -----------------------------------------------------
add_subdirectory(${CMAKE_SOURCE_DIR}/Dependencies/d3d8gles3 ${CMAKE_BINARY_DIR}/d3d8gles3)

# Everything that linked the DXVK d3d8 binary links d3d8gles3 instead. The engine
# resolves Direct3DCreate8 from this static lib.
# (core_config is the interface target every engine lib inherits from.)
target_link_libraries(core_config INTERFACE d3d8gles3)
