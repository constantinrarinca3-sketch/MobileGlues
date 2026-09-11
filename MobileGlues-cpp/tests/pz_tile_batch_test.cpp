// Host-side sequence test for the ordered PZ StateRun compiler.

#include "../gl/pz_tile_batch.h"
#include "../gl/mg.h"
#include "../gl/pz_census.h"
#include "../gl/threaded_submission.h"
#include "../egl/context.h"
#include "../gles/loader.h"

#include <array>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

gles_func_t g_gles_func;
gles_caps_t g_gles_caps{};
bool mg_pz_census_active = false;
bool mg_pz_vao_fastpath_active = false;
bool mg_pz_attrib_fastpath_active = false;
bool mg_pz_uniform_fastpath_active = true;
bool mg_pz_buffer_streaming_active = false;
bool mg_pz_buffer_discard_coalesce_active = false;
bool mg_pz_state_shadow_active = false;
bool mg_pz_runtime_mipmap_skip_active = false;
bool mg_pz_quad_index_cache_active = false;
bool mg_pz_threaded_submission_active = true;
bool mg_pz_tile_batch_active = true;

extern "C" {
gl_state_s g_default_gl_state{};
thread_local gl_state_t gl_state = &g_default_gl_state;
}
thread_local MGContext* g_current_ctx = nullptr;

namespace {

struct draw_call {
    GLuint start;
    GLuint end;
    GLsizei count;
    uintptr_t offset;
};

struct multidraw_call {
    std::vector<GLsizei> counts;
    std::vector<uintptr_t> offsets;
};

struct client_draw_call {
    GLsizei count;
    std::vector<GLushort> indices;
};

std::vector<draw_call> draws;
std::vector<multidraw_call> multidraws;
std::vector<client_draw_call> client_draws;
std::vector<GLuint> element_bindings;
std::vector<GLint> run_counts;
std::vector<GLint> run_starts;
std::vector<GLfloat> run_depths;
std::vector<std::pair<GLint, GLfloat>> scalar_writes;
std::vector<unsigned char> index_shadow;

void fake_uniform1f(GLint location, GLfloat value) { scalar_writes.emplace_back(location, value); }
void fake_uniform1i(GLint location, GLint value) {
    if (location == 3) run_counts.push_back(value);
}
void fake_uniform1iv(GLint location, GLsizei count, const GLint* values) {
    if (location == 4) run_starts.assign(values, values + count);
}
void fake_uniform2fv(GLint location, GLsizei count, const GLfloat* values) {
    if (location == 5) run_depths.assign(values, values + count * 2);
}
void fake_draw_range(GLenum, GLuint start, GLuint end, GLsizei count, GLenum, const void* indices) {
    draws.push_back({start, end, count, reinterpret_cast<uintptr_t>(indices)});
}
void fake_draw_elements(GLenum, GLsizei count, GLenum type, const void* indices) {
    assert(type == GL_UNSIGNED_SHORT);
    const auto* values = static_cast<const GLushort*>(indices);
    client_draws.push_back({count, std::vector<GLushort>(values, values + count)});
}
void fake_bind_buffer(GLenum target, GLuint buffer) {
    if (target == GL_ELEMENT_ARRAY_BUFFER) element_bindings.push_back(buffer);
}
void GLAPIENTRY fake_multidraw(GLenum, const GLsizei* counts, GLenum, const void* const* indices,
                               GLsizei draw_count) {
    multidraw_call call;
    call.counts.assign(counts, counts + draw_count);
    for (GLsizei index = 0; index < draw_count; ++index)
        call.offsets.push_back(reinterpret_cast<uintptr_t>(indices[index]));
    multidraws.push_back(std::move(call));
}
GLenum fake_get_error() { return GL_NO_ERROR; }
void fake_get_program(GLuint, GLenum, GLint* value) { *value = GL_TRUE; }
GLint fake_get_location(GLuint, const GLchar* name) {
    if (std::strcmp(name, "zDepth") == 0) return 1;
    if (std::strcmp(name, "chunkDepth") == 0) return 2;
    if (std::strcmp(name, "zomdroidBatchRunCount") == 0) return 3;
    if (std::strcmp(name, "zomdroidBatchRunStart[0]") == 0) return 4;
    if (std::strcmp(name, "zomdroidBatchDepth[0]") == 0) return 5;
    return -1;
}

alignas(std::max_align_t) std::array<unsigned char, mg_ts::kMaximumCommandPayloadBytes> command_storage{};
mg_ts::command_fn command_execute = nullptr;
mg_ts::command_fn command_destroy = nullptr;
uint64_t command_sequence = 0;
bool worker_executing = false;

void setup_program() {
    GLES.glUniform1f = fake_uniform1f;
    GLES.glUniform1i = fake_uniform1i;
    GLES.glUniform1iv = fake_uniform1iv;
    GLES.glUniform2fv = fake_uniform2fv;
    GLES.glDrawRangeElements = fake_draw_range;
    GLES.glDrawElements = fake_draw_elements;
    GLES.glBindBuffer = fake_bind_buffer;
    GLES.glGetError = fake_get_error;
    GLES.glGetProgramiv = fake_get_program;
    GLES.glGetUniformLocation = fake_get_location;
    gl_state->current_program = 7;
    mg_pz_tile_batch_note_shader(11, true);
    mg_pz_tile_batch_attach_shader(7, 11);
    mg_pz_tile_batch_program_linked(7);
    mg_pz_tile_batch_test_backend(fake_multidraw);
}

void depth(float z, float chunk) {
    assert(mg_pz_tile_batch_uniform1f(7, 1, z));
    assert(mg_pz_tile_batch_uniform1f(7, 2, chunk));
}

} // namespace

extern "C" GLuint mg_driver_bound_buffer(GLenum target) {
    return target == GL_ELEMENT_ARRAY_BUFFER ? 99 : 0;
}
extern "C" GLuint find_bound_buffer_by_target(GLenum target) {
    return target == GL_ELEMENT_ARRAY_BUFFER ? 77 : 0;
}
extern "C" bool mg_pz_buffer_cache_identity(GLuint buffer, uint64_t* lifetime, uint64_t* version,
                                             GLsizeiptr* size) {
    if (buffer != 77 || index_shadow.empty()) return false;
    *lifetime = 1;
    *version = 1;
    *size = static_cast<GLsizeiptr>(index_shadow.size());
    return true;
}
extern "C" const void* mg_pz_buffer_cache_source(GLuint buffer, uint64_t lifetime, uint64_t version,
                                                  GLsizeiptr size) {
    return buffer == 77 && lifetime == 1 && version == 1 &&
                   size == static_cast<GLsizeiptr>(index_shadow.size())
               ? index_shadow.data()
               : nullptr;
}

namespace mg_ts {
bool active() { return !worker_executing; }
reservation reserve(command_fn execute, command_fn destroy, size_t payload_size, size_t, command_kind) {
    assert(payload_size <= command_storage.size());
    command_execute = execute;
    command_destroy = destroy;
    return {command_storage.data(), ++command_sequence};
}
void publish(uint64_t) {
    worker_executing = true;
    command_execute(command_storage.data());
    command_destroy(command_storage.data());
    worker_executing = false;
}
void wait(uint64_t) {}
void flush_pending() {}
bool draw_async_safe(bool, bool) { return true; }
} // namespace mg_ts

int __android_log_print(int, const char*, const char*, ...) { return 0; }
extern "C" void write_log(const char*, ...) {}
extern "C" void write_log_n(const char*, ...) {}

int main() {
    setup_program();

    depth(0.1f, 0.01f);
    assert(mg_pz_tile_batch_draw_range(GL_TRIANGLES, 0, 2, 3, GL_UNSIGNED_SHORT,
                                       reinterpret_cast<const void*>(0)));
    depth(0.2f, 0.02f);
    assert(mg_pz_tile_batch_draw_range(GL_TRIANGLES, 3, 5, 3, GL_UNSIGNED_SHORT,
                                       reinterpret_cast<const void*>(64)));
    depth(0.3f, 0.03f);
    assert(mg_pz_tile_batch_draw_range(GL_TRIANGLES, 6, 8, 3, GL_UNSIGNED_SHORT,
                                       reinterpret_cast<const void*>(128)));
    mg_pz_tile_batch_flush();

    assert(draws.empty());
    assert(multidraws.size() == 1);
    assert((multidraws[0].counts == std::vector<GLsizei>{3, 3, 3}));
    assert((multidraws[0].offsets == std::vector<uintptr_t>{0, 64, 128}));
    assert((run_counts == std::vector<GLint>{3, 0}));
    assert((run_starts == std::vector<GLint>{0, 3, 6}));
    assert(run_depths.size() == 6 && run_depths[0] == 0.1f && run_depths[5] == 0.03f);

    multidraws.clear();
    client_draws.clear();
    element_bindings.clear();
    run_counts.clear();
    depth(1.0f, 0.1f);
    assert(mg_pz_tile_batch_draw_range(GL_TRIANGLES, 10, 12, 3, GL_UNSIGNED_SHORT,
                                       reinterpret_cast<const void*>(20)));
    depth(2.0f, 0.2f);
    assert(mg_pz_tile_batch_draw_range(GL_TRIANGLES, 13, 15, 3, GL_UNSIGNED_SHORT,
                                       reinterpret_cast<const void*>(26)));
    mg_pz_tile_batch_flush();
    assert(draws.empty());
    assert(multidraws.size() == 1);
    assert((multidraws[0].counts == std::vector<GLsizei>{3, 3}));
    assert((run_counts == std::vector<GLint>{2, 0}));

    // PZ can report broad, overlapping draw ranges even though the actual
    // indices address separate vertices. Recover the exact ranges from the CPU
    // EBO shadow so those StateRuns remain safe to compile.
    multidraws.clear();
    run_counts.clear();
    index_shadow.resize(12);
    const std::array<GLushort, 6> shadow_values{40, 41, 42, 50, 51, 52};
    std::memcpy(index_shadow.data(), shadow_values.data(), index_shadow.size());
    depth(2.1f, 0.21f);
    assert(mg_pz_tile_batch_draw_range(GL_TRIANGLES, 0, 100, 3, GL_UNSIGNED_SHORT,
                                       reinterpret_cast<const void*>(0)));
    depth(2.2f, 0.22f);
    assert(mg_pz_tile_batch_draw_range(GL_TRIANGLES, 0, 100, 3, GL_UNSIGNED_SHORT,
                                       reinterpret_cast<const void*>(6)));
    mg_pz_tile_batch_flush();
    assert(multidraws.empty());
    assert(client_draws.size() == 1);
    assert(client_draws[0].count == 6);
    assert((client_draws[0].indices == std::vector<GLushort>{40, 41, 42, 50, 51, 52}));
    assert((element_bindings == std::vector<GLuint>{0, 99}));
    assert((run_starts == std::vector<GLint>{40, 50}));
    index_shadow.clear();

    draws.clear();
    multidraws.clear();
    run_counts.clear();
    scalar_writes.clear();
    depth(3.0f, 0.3f);
    assert(mg_pz_tile_batch_draw_range(GL_TRIANGLES, 20, 22, 3, GL_UNSIGNED_SHORT,
                                       reinterpret_cast<const void*>(40)));
    depth(4.0f, 0.4f); // belongs after the retained draw
    mg_pz_tile_batch_flush();
    assert(draws.size() == 1);
    assert(scalar_writes.size() == 4);
    assert(scalar_writes[0] == std::make_pair(1, 3.0f));
    assert(scalar_writes[1] == std::make_pair(2, 0.3f));
    assert(scalar_writes[2] == std::make_pair(1, 4.0f));
    assert(scalar_writes[3] == std::make_pair(2, 0.4f));

    // A missing backend changes performance only. It must replay the original
    // ordered draws with their original offsets and depth uniforms.
    draws.clear();
    scalar_writes.clear();
    mg_pz_tile_batch_test_backend(nullptr);
    depth(5.0f, 0.5f);
    assert(mg_pz_tile_batch_draw_range(GL_TRIANGLES, 30, 32, 3, GL_UNSIGNED_SHORT,
                                       reinterpret_cast<const void*>(200)));
    depth(6.0f, 0.6f);
    assert(mg_pz_tile_batch_draw_range(GL_TRIANGLES, 33, 35, 3, GL_UNSIGNED_SHORT,
                                       reinterpret_cast<const void*>(260)));
    mg_pz_tile_batch_flush();
    assert(draws.size() == 2);
    assert(draws[0].offset == 200 && draws[1].offset == 260);

    std::puts("PZ StateRun compiler checks passed");
    return 0;
}
