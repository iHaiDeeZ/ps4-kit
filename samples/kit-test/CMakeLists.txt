# kit-test: the kit on the console, measured (see main.cpp). Also the smallest complete port: copy it
# to start a new one.
#   samples/kit-test/build.sh
cmake_minimum_required(VERSION 3.20)
project(kit_test C CXX ASM)
include(${CMAKE_CURRENT_LIST_DIR}/../../ps4-kit.cmake)

set(KIT_TEST_VERSION "00.00" CACHE STRING "Version shown in the results")

# SPIR-V for the test's shaders, as C arrays.
find_program(GLSLANG glslangValidator HINTS $ENV{HOME}/ps4kit-build/host-tools/bin REQUIRED)
set(_shader_headers)
foreach (shader quad.vert quad.frag quad2.frag quad_vb.vert quad_ubo.frag)
    set(_out ${CMAKE_CURRENT_BINARY_DIR}/${shader}.h)
    string(REPLACE "." "_" _var ${shader})
    add_custom_command(
        OUTPUT ${_out}
        COMMAND ${GLSLANG} -V --quiet --vn ${_var} -o ${_out} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}
        VERBATIM)
    list(APPEND _shader_headers ${_out})
endforeach ()

add_executable(kit_test main.cpp report.cpp bench_cpu.cpp bench_io.cpp bench_gpu.cpp ${_shader_headers})
target_compile_features(kit_test PRIVATE cxx_std_20)
target_compile_definitions(kit_test PRIVATE KIT_TEST_VERSION="${KIT_TEST_VERSION}")
target_include_directories(kit_test PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
ps4_kit_link_mesa(kit_test)
ps4_kit_app(kit_test)
