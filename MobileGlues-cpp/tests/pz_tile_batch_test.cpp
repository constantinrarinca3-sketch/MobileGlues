// Host-side sequence test for the ordered PZ StateRun batcher.

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

std::vector<draw_call> draws;
std::vector<GLint> run_counts;
std::vector<GLint> run_starts;
std::vector<GLfloat> run_depths;
std::vector<std::pair<GLint, GLfloat>> scalar_writes;

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
void fake_get_program(GLuint, GLenum, GLint* value) { *value = GL_TRUE; }
GLint fake_get_location(GLuint, const GLchar* name) {
    if (std::strcmp(name, "zDepth") == 0) return 1;
    if (std::strcmp(name, "chunkDepth") == 0) return 2;
    if (std::strcmp(name, "zomdroidBatchRunCount") == 0) return 3;
    if (std::strcmp(name, "zomdroidBatchRunStart[0]") == 0) return 4;
    if (std::strcmp(name, "zomdroidBatchDepth[0]") == 0) return 5;
    return -1;
}

alignas(std::max_align_t) std::array<unsigned char, mg_ts::kCommandPayloadBytes> command_storage{};
mg_ts::command_fn command_execute = nullptr;
mg_ts::command_fn command_destroy = nullptr;
uint64_t command_sequence = 0;

void setup_program() {
    GLES.glUniform1f = fake_uniform1f;
    GLES.glUniform1i = fake_uniform1i;
    GLES.glUniform1iv = fake_uniform1iv;
    GLES.glUniform2fv = fake_uniform2fv;
    GLES.glDrawRangeElements = fake_draw_range;
    GLES.glGetProgramiv = fake_get_program;
    GLES.glGetUniformLocation = fake_get_location;
    gl_state->current_program = 7;
    mg_pz_tile_batch_note_shader(11, true);
    mg_pz_tile_batch_attach_shader(7, 11);
    mg_pz_tile_batch_program_linked(7);
}

void depth(float z, float chunk) {
    assert(mg_pz_tile_batch_uniform1f(7, 1, z));
    assert(mg_pz_tile_batch_uniform1f(7, 2, chunk));
}

} // namespace

extern "C" GLuint mg_driver_bound_buffer(GLenum target) {
    return target == GL_ELEMENT_ARRAY_BUFFER ? 99 : 0;
}

namespace mg_ts {
bool active() { return true; }
reservation reserve(command_fn execute, command_fn destroy, size_t payload_size, size_t, command_kind) {
    assert(payload_size <= command_storage.size());
    command_execute = execute;
    command_destroy = destroy;
    return {command_storage.data(), ++command_sequence};
}
void publish(uint64_t) {
    command_execute(command_storage.data());
    command_destroy(command_storage.data());
}
void wait(uint64_t) {}
void flush_pending() {}
bool draw_async_safe(bool, bool) { return true; }
} // namespace mg_ts

int __android_log_print(int, const char*, const char*, ...) { return 0; }

int main() {
    setup_program();

    depth(0.1f, 0.01f);
    assert(mg_pz_tile_batch_draw_range(GL_TRIANGLES, 0, 2, 3, GL_UNSIGNED_SHORT,
                                       reinterpret_cast<const void*>(0)));
    depth(0.2f, 0.02f);
    assert(mg_pz_tile_batch_draw_range(GL_TRIANGLES, 3, 5, 3, GL_UNSIGNED_SHORT,
                                       reinterpret_cast<const void*>(6)));
    depth(0.3f, 0.03f);
    assert(mg_pz_tile_batch_draw_range(GL_TRIANGLES, 6, 8, 3, GL_UNSIGNED_SHORT,
                                       reinterpret_cast<const void*>(12)));
    mg_pz_tile_batch_flush();

    assert(draws.size() == 1);
    assert(draws[0].start == 0 && draws[0].end == 8 && draws[0].count == 9 && draws[0].offset == 0);
    assert((run_counts == std::vector<GLint>{3, 0}));
    assert((run_starts == std::vector<GLint>{0, 3, 6}));
    assert(run_depths.size() == 6 && run_depths[0] == 0.1f && run_depths[5] == 0.03f);

    draws.clear();
    run_counts.clear();
    depth(1.0f, 0.1f);
    assert(mg_pz_tile_batch_draw_range(GL_TRIANGLES, 10, 12, 3, GL_UNSIGNED_SHORT,
                                       reinterpret_cast<const void*>(20)));
    depth(2.0f, 0.2f);
    assert(mg_pz_tile_batch_draw_range(GL_TRIANGLES, 13, 15, 3, GL_UNSIGNED_SHORT,
                                       reinterpret_cast<const void*>(26)));
    mg_pz_tile_batch_flush();
    assert(draws.size() == 2);
    assert(draws[0].count == 3 && draws[1].count == 3);
    assert(run_counts.empty());

    draws.clear();
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

    std::puts("PZ tile batch checks passed");
    return 0;
}
