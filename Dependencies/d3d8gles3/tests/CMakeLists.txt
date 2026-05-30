# Host unit tests for the header-free, GL-call-free parts of d3d8gles3:
# the FFP shader generator (string output) and the enum mapping tables.
#
# These need the GLES3 *headers* (for GLenum/GLuint typedefs) but link no GL
# driver — the generator emits source strings and never issues a GL call, and
# the tests only invoke the static Generate*/Map* functions.

add_executable(test_d3d8gles3
    test_ffp_shader_gen.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/ffp_shader_gen.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/gl_state_map.cpp
)
target_include_directories(test_d3d8gles3 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(DXVK_INCLUDE_DIR)
    target_include_directories(test_d3d8gles3 PRIVATE ${DXVK_INCLUDE_DIR})
endif()
target_compile_features(test_d3d8gles3 PRIVATE cxx_std_17)

# We exercise FFPShaderCache::Generate* and the Map* functions; GetProgram()
# (which calls GL) is intentionally not invoked here.
add_test(NAME d3d8gles3_ffp COMMAND test_d3d8gles3)
