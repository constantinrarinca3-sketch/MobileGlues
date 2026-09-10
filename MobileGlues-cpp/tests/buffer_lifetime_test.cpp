// Exercises the real frontend buffer/VAO tables without a GLES driver.  A VAO
// must not start referring to an unrelated EBO when a deleted frontend name is
// recycled for a new buffer object.
#include <cstdio>
#include <cstddef>

#include "gl/buffer.h"

void mg_buffer_bind_context(unsigned long long ctx_id, unsigned long long group_id);
void mg_buffer_forget_context(unsigned long long ctx_id);

GLuint get_ibo_by_vao(GLuint vao);
void update_vao_ibo_binding(GLuint vao, GLuint ibo);
void set_buffer_data_size(GLuint buffer, size_t size);
bool get_known_buffer_data_size(GLuint buffer, GLsizeiptr* size);

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
    GLsizeiptr tracked_size = -1;
    expect("unallocated buffer size is not treated as known", get_known_buffer_data_size(first, &tracked_size),
           GL_FALSE);
    set_buffer_data_size(first, 4096);
    expect("allocated buffer size is available without a driver query",
           get_known_buffer_data_size(first, &tracked_size), GL_TRUE);
    expect("tracked buffer size matches its allocation", static_cast<GLuint>(tracked_size), 4096);
    update_vao_ibo_binding(vao, first);
    expect("live EBO binding is retained", get_ibo_by_vao(vao), first);

    remove_buffer(first);
    expect("deleted buffer size is not reused", get_known_buffer_data_size(first, &tracked_size), GL_FALSE);
    expect("deleted EBO is detached from the tracked VAO", get_ibo_by_vao(vao), 0);

    const GLuint replacement = gen_buffer();
    expect("test setup recycled the same frontend name", replacement, first);
    expect("replacement starts without the old buffer size", get_known_buffer_data_size(replacement, &tracked_size),
           GL_FALSE);
    expect("old VAO does not inherit the replacement object", get_ibo_by_vao(vao), 0);

    update_vao_ibo_binding(vao, replacement);
    expect("an explicit rebind accepts the replacement lifetime", get_ibo_by_vao(vao), replacement);

    // A reused internal group id must not inherit the object table of a context
    // that has already been destroyed.
    mg_buffer_bind_context(101, 201);
    InitBufferMap(8);
    const GLuint context_buffer = gen_buffer();
    set_buffer_data_size(context_buffer, 8192);
    expect("context group owns its live buffer", has_buffer(context_buffer), GL_TRUE);
    mg_buffer_bind_context(102, 201);
    mg_buffer_bind_context(0, 0);
    mg_buffer_forget_context(101);
    mg_buffer_bind_context(102, 201);
    expect("shared buffer table survives a sibling context", has_buffer(context_buffer), GL_TRUE);
    mg_buffer_bind_context(0, 0);
    mg_buffer_forget_context(102);
    mg_buffer_bind_context(103, 201);
    expect("last context teardown releases the shared buffer table", has_buffer(context_buffer), GL_FALSE);
    mg_buffer_bind_context(0, 0);
    mg_buffer_forget_context(103);

    std::printf("%s (%d failures)\n", fails ? "FAILED" : "buffer lifetime checks passed", fails);
    return fails != 0;
}
