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
$CXX -o /tmp/mg_quad_test   tests/quad_indices_test.cpp
$CXX -o /tmp/mg_shader_compat_test tests/shader_compat_test.cpp
/tmp/mg_pixel_test
echo
/tmp/mg_fb_test
echo
/tmp/mg_buffer_lifetime_test
echo
/tmp/mg_quad_test
echo
/tmp/mg_shader_compat_test
