// Exercises the real frontend buffer/VAO tables without a GLES driver.  A VAO
// must not start referring to an unrelated EBO when a deleted frontend name is
// recycled for a new buffer object.
#include <cstdio>

#include "gl/buffer.h"

GLuint get_ibo_by_vao(GLuint vao);
void update_vao_ibo_binding(GLuint vao, GLuint ibo);

static int fails = 0;

static void expect(const char* what, GLuint got, GLuint want) {
    if (got != want) {
        std::printf("  FAIL %-58s got=%u expected=%u\n", what, got, want);
        ++fails;
    }
}

int main() {
    InitBufferMap(8);
    InitVertexArrayMap(8);

    const GLuint vao = gen_array();
    const GLuint first = gen_buffer();
    update_vao_ibo_binding(vao, first);
    expect("live EBO binding is retained", get_ibo_by_vao(vao), first);

    remove_buffer(first);
    expect("deleted EBO is detached from the tracked VAO", get_ibo_by_vao(vao), 0);

    const GLuint replacement = gen_buffer();
    expect("test setup recycled the same frontend name", replacement, first);
    expect("old VAO does not inherit the replacement object", get_ibo_by_vao(vao), 0);

    update_vao_ibo_binding(vao, replacement);
    expect("an explicit rebind accepts the replacement lifetime", get_ibo_by_vao(vao), replacement);

    std::printf("%s (%d failures)\n", fails ? "FAILED" : "buffer lifetime checks passed", fails);
    return fails != 0;
}
