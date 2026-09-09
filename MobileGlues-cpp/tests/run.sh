#!/bin/sh
# Host-side checks for renderer code that is pure enough to run without a GPU.
# Tests that cover implementation files link the real translation units, not
# copies of their logic.
#
#   sh MobileGlues-cpp/tests/run.sh
set -e
cd "$(dirname "$0")/.."
INC="-I. -I./includes -I./include -I./3rdparty/xxhash"
CXX="${CXX:-g++} -std=gnu++20 -w $INC"
$CXX -o /tmp/mg_pixel_test  tests/pixel_size_test.cpp         gl/pixel.cpp
$CXX -o /tmp/mg_fb_test     tests/framebuffer_shuffle_test.cpp gl/framebuffer.cpp
$CXX -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -o /tmp/mg_buffer_lifetime_test tests/buffer_lifetime_test.cpp gl/buffer.cpp
$CXX -DZOMDROID_EXPERIMENTAL=1 -DMOBILEGLUES_TESTING=1 -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -o /tmp/mg_buffer_streaming_test tests/buffer_streaming_test.cpp gl/buffer.cpp
$CXX -DZOMDROID_EXPERIMENTAL=1 -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -o /tmp/mg_server_attrib_test tests/server_attrib_test.cpp gl/server_attrib.cpp gl/enable.cpp gl/pz_census.cpp
$CXX -o /tmp/mg_quad_test   tests/quad_indices_test.cpp
$CXX -o /tmp/mg_shader_compat_test tests/shader_compat_test.cpp
$CXX -DZOMDROID_EXPERIMENTAL=1 -DZOMDROID_GL_BREADCRUMBS=1 \
    -o /tmp/mg_pz_census_test tests/pz_census_test.cpp gl/pz_census.cpp
$CXX -DZOMDROID_EXPERIMENTAL=1 -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -o /tmp/mg_fixed_state_shadow_test tests/fixed_state_shadow_test.cpp gl/gl_native.cpp
$CXX -DZOMDROID_EXPERIMENTAL=1 -DMOBILEGLUES_TESTING=1 -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -o /tmp/mg_runtime_mipmap_skip_test tests/runtime_mipmap_skip_test.cpp gl/texture.cpp
$CXX -DZOMDROID_EXPERIMENTAL=1 -DMOBILEGLUES_TESTING=1 -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -o /tmp/mg_basevertex_fastpath_test tests/basevertex_fastpath_test.cpp gl/buffer.cpp
/tmp/mg_pixel_test
echo
/tmp/mg_fb_test
echo
/tmp/mg_buffer_lifetime_test
echo
/tmp/mg_buffer_streaming_test
echo
/tmp/mg_server_attrib_test
echo
/tmp/mg_quad_test
echo
/tmp/mg_shader_compat_test
echo
/tmp/mg_pz_census_test
echo
/tmp/mg_fixed_state_shadow_test
echo
/tmp/mg_runtime_mipmap_skip_test
echo
/tmp/mg_basevertex_fastpath_test
