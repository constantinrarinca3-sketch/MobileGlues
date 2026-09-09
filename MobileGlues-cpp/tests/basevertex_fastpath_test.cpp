// Checks the pointer-offset to baseVertex calculation without a GLES driver.
#include "gl/pz_census.h"

#include <GL/gl.h>
#include <cstdint>
#include <cstdio>

bool mg_pz_census_active = false;
bool mg_pz_vao_fastpath_active = false;
bool mg_pz_attrib_fastpath_active = false;
bool mg_pz_uniform_fastpath_active = false;
bool mg_pz_buffer_streaming_active = false;
bool mg_pz_state_shadow_active = false;
bool mg_pz_runtime_mipmap_skip_active = false;
bool mg_pz_basevertex_fastpath_active = true;

void mg_test_basevertex_reset();
void mg_test_basevertex_attrib(GLuint index, GLboolean enabled, uintptr_t frontend_pointer,
                               uintptr_t driver_pointer, GLsizei stride, GLuint divisor, bool binding_model);
int mg_test_find_common_basevertex(GLint* basevertex);

static int failures = 0;

static void expect(bool condition, const char* message) {
    if (condition) return;
    std::printf("FAIL %s\n", message);
    ++failures;
}

int main() {
    GLint base = 0;
    mg_test_basevertex_reset();
    mg_test_basevertex_attrib(0, GL_TRUE, 48, 0, 16, 0, false);
    mg_test_basevertex_attrib(1, GL_TRUE, 56, 8, 16, 0, false);
    expect(mg_test_find_common_basevertex(&base) == 0 && base == 3,
           "coherent pointer shifts must produce one base vertex");

    mg_test_basevertex_attrib(1, GL_TRUE, 40, 8, 16, 0, false);
    expect(mg_test_find_common_basevertex(&base) == 2,
           "different attribute shifts must fail closed");

    mg_test_basevertex_reset();
    mg_test_basevertex_attrib(0, GL_TRUE, 16, 48, 16, 0, false);
    expect(mg_test_find_common_basevertex(&base) == 0 && base == -2,
           "backward pointer shifts must preserve a negative base vertex");

    mg_test_basevertex_attrib(1, GL_TRUE, 32, 32, 16, 1, false);
    expect(mg_test_find_common_basevertex(&base) == 0 && base == -2,
           "an unchanged instanced attribute must not affect the vertex base");

    mg_test_basevertex_reset();
    mg_test_basevertex_attrib(0, GL_TRUE, 32, 0, 16, 0, true);
    expect(mg_test_find_common_basevertex(&base) == 2,
           "the separate binding model must remain on the direct path");

    mg_test_basevertex_reset();
    mg_test_basevertex_attrib(0, GL_TRUE, 0, 0, 16, 0, false);
    expect(mg_test_find_common_basevertex(&base) == 1 && base == 0,
           "an unchanged pointer needs no base-vertex draw");

    std::printf("%s (%d failures)\n", failures ? "FAILED" : "baseVertex checks passed", failures);
    return failures != 0;
}
