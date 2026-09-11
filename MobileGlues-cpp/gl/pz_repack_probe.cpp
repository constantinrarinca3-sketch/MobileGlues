// MobileGlues - gl/pz_repack_probe.cpp
// Diagnostic-only Project Zomboid tiny indexed-draw census.
//
// This probe deliberately does not alter rendering. It sits under the frontend,
// on the backend dispatch table, and measures whether the stable PZ workload has
// enough tiny indexed TRIANGLES to justify a real repack/instance-style renderer.

#include "pz_repack_probe.h"

#if defined(ZOMDROID_EXPERIMENTAL)

#include "../gles/loader.h"
#include "log.h"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace {

struct originals_t {
    glDrawElements_PTR draw_elements = nullptr;
    glDrawElementsInstanced_PTR draw_elements_instanced = nullptr;
    glDrawElementsBaseVertex_PTR draw_elements_base_vertex = nullptr;
    glDrawElementsInstancedBaseVertex_PTR draw_elements_instanced_base_vertex = nullptr;
    glDrawRangeElements_PTR draw_range_elements = nullptr;
    glDrawRangeElementsBaseVertex_PTR draw_range_elements_base_vertex = nullptr;
    glUseProgram_PTR use_program = nullptr;
    glActiveTexture_PTR active_texture = nullptr;
    glBindTexture_PTR bind_texture = nullptr;
};

struct stats_t {
    unsigned long long indexed = 0;
    unsigned long long triangles = 0;
    unsigned long long triangle_fan = 0;
    unsigned long long shape_eligible = 0;
    unsigned long long quad6_shape = 0;
    unsigned long long fan4_shape = 0;
    unsigned long long exact3 = 0;
    unsigned long long exact6 = 0;
    unsigned long long exact9 = 0;
    unsigned long long exact12 = 0;
    unsigned long long le12 = 0;
    unsigned long long le24 = 0;
    unsigned long long le48 = 0;
    unsigned long long le96 = 0;
    unsigned long long type_u8 = 0;
    unsigned long long type_u16 = 0;
    unsigned long long type_u32 = 0;
    unsigned long long type_other = 0;
    unsigned long long instanced = 0;
    unsigned long long basevertex = 0;
    unsigned long long offset32 = 0;
    unsigned long long relaxed_runs = 0;
    unsigned long long relaxed_adjacent = 0;
    unsigned long long relaxed_max_run = 0;
};

struct thread_state_t {
    stats_t stats;
    stats_t last_report;
    GLuint program = 0;
    GLenum active_texture = GL_TEXTURE0;
    unsigned long long material_epoch = 1;
    unsigned long long previous_material_epoch = 0;
    unsigned long long relaxed_run = 0;
    bool previous_eligible = false;
};

originals_t g_orig;
bool g_enabled = false;
thread_local thread_state_t g_state;

bool valid_index_type(GLenum type) {
    return type == GL_UNSIGNED_BYTE || type == GL_UNSIGNED_SHORT || type == GL_UNSIGNED_INT;
}

void note_type(GLenum type, stats_t& s) {
    if (type == GL_UNSIGNED_BYTE)
        ++s.type_u8;
    else if (type == GL_UNSIGNED_SHORT)
        ++s.type_u16;
    else if (type == GL_UNSIGNED_INT)
        ++s.type_u32;
    else
        ++s.type_other;
}

stats_t delta(const stats_t& now, const stats_t& old) {
    stats_t d;
#define SUB(field) d.field = now.field - old.field
    SUB(indexed);
    SUB(triangles);
    SUB(triangle_fan);
    SUB(shape_eligible);
    SUB(quad6_shape);
    SUB(fan4_shape);
    SUB(exact3);
    SUB(exact6);
    SUB(exact9);
    SUB(exact12);
    SUB(le12);
    SUB(le24);
    SUB(le48);
    SUB(le96);
    SUB(type_u8);
    SUB(type_u16);
    SUB(type_u32);
    SUB(type_other);
    SUB(instanced);
    SUB(basevertex);
    SUB(offset32);
    SUB(relaxed_runs);
    SUB(relaxed_adjacent);
#undef SUB
    d.relaxed_max_run = now.relaxed_max_run;
    return d;
}

void report_probe() {
    const stats_t& s = g_state.stats;
    const stats_t d = delta(s, g_state.last_report);
    const double shape_pct = s.indexed ? 100.0 * static_cast<double>(s.shape_eligible) / static_cast<double>(s.indexed) : 0.0;
    const double q6_pct = s.triangles ? 100.0 * static_cast<double>(s.quad6_shape) / static_cast<double>(s.triangles) : 0.0;
    const double upper_pct = s.shape_eligible
                                 ? 100.0 * static_cast<double>(s.relaxed_adjacent) /
                                       static_cast<double>(s.shape_eligible)
                                 : 0.0;

    LOG_I("ZOMDROID_PZ_REPACK_PROBE indexed=%llu tri=%llu fan=%llu shape=%llu/%.2f%% quad6_shape=%llu/%.2f%% "
          "exact=%llu/%llu/%llu/%llu le=%llu/%llu/%llu/%llu type=%llu/%llu/%llu/%llu instanced=%llu "
          "basev=%llu offset32=%llu relaxed=%llu/%llu/%llu/%.2f%% delta=%llu/%llu/%llu/%llu",
          s.indexed, s.triangles, s.triangle_fan, s.shape_eligible, shape_pct, s.quad6_shape, q6_pct,
          s.exact3, s.exact6, s.exact9, s.exact12, s.le12, s.le24, s.le48, s.le96,
          s.type_u8, s.type_u16, s.type_u32, s.type_other, s.instanced, s.basevertex, s.offset32,
          s.relaxed_runs, s.relaxed_adjacent, s.relaxed_max_run, upper_pct,
          d.indexed, d.shape_eligible, d.quad6_shape, d.relaxed_adjacent)

    g_state.last_report = s;
}

void maybe_report() {
    const unsigned long long n = g_state.stats.indexed;
    if (n == 1 || n == 1024 || n == 65536 || (n != 0 && n % 250000ULL == 0)) report_probe();
}

void note_indexed(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instances,
                  GLint basevertex) {
    stats_t& s = g_state.stats;
    ++s.indexed;
    note_type(type, s);
    if (mode == GL_TRIANGLES) ++s.triangles;
    if (mode == GL_TRIANGLE_FAN) ++s.triangle_fan;
    if (instances > 1) ++s.instanced;
    if (basevertex != 0) ++s.basevertex;
    if (reinterpret_cast<uintptr_t>(indices) <= UINT32_MAX) ++s.offset32;

    if (mode == GL_TRIANGLES && count > 0) {
        if (count == 3) ++s.exact3;
        if (count == 6) ++s.exact6;
        if (count == 9) ++s.exact9;
        if (count == 12) ++s.exact12;
        if (count <= 12) ++s.le12;
        if (count <= 24) ++s.le24;
        if (count <= 48) ++s.le48;
        if (count <= 96) ++s.le96;
    }

    const bool eligible = mode == GL_TRIANGLES && count >= 3 && count <= 96 && (count % 3) == 0 &&
                          instances == 1 && valid_index_type(type);
    if (eligible) {
        ++s.shape_eligible;
        if (count == 6) ++s.quad6_shape;
        if (g_state.previous_eligible && g_state.previous_material_epoch == g_state.material_epoch) {
            ++s.relaxed_adjacent;
            ++g_state.relaxed_run;
        } else {
            ++s.relaxed_runs;
            g_state.relaxed_run = 1;
        }
        s.relaxed_max_run = std::max(s.relaxed_max_run, g_state.relaxed_run);
        g_state.previous_material_epoch = g_state.material_epoch;
        g_state.previous_eligible = true;
    } else {
        g_state.previous_eligible = false;
        g_state.relaxed_run = 0;
    }

    if (mode == GL_TRIANGLE_FAN && count == 4 && instances == 1 && valid_index_type(type)) ++s.fan4_shape;
    maybe_report();
}

void probe_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    note_indexed(mode, count, type, indices, 1, 0);
    g_orig.draw_elements(mode, count, type, indices);
}

void probe_glDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                   GLsizei instancecount) {
    note_indexed(mode, count, type, indices, instancecount, 0);
    g_orig.draw_elements_instanced(mode, count, type, indices, instancecount);
}

void probe_glDrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices, GLint basevertex) {
    note_indexed(mode, count, type, indices, 1, basevertex);
    g_orig.draw_elements_base_vertex(mode, count, type, indices, basevertex);
}

void probe_glDrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                            GLsizei instancecount, GLint basevertex) {
    note_indexed(mode, count, type, indices, instancecount, basevertex);
    g_orig.draw_elements_instanced_base_vertex(mode, count, type, indices, instancecount, basevertex);
}

void probe_glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void* indices) {
    note_indexed(mode, count, type, indices, 1, 0);
    g_orig.draw_range_elements(mode, start, end, count, type, indices);
}

void probe_glDrawRangeElementsBaseVertex(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                                        const void* indices, GLint basevertex) {
    note_indexed(mode, count, type, indices, 1, basevertex);
    g_orig.draw_range_elements_base_vertex(mode, start, end, count, type, indices, basevertex);
}

void probe_glUseProgram(GLuint program) {
    if (program != g_state.program) {
        g_state.program = program;
        ++g_state.material_epoch;
    }
    g_orig.use_program(program);
}

void probe_glActiveTexture(GLenum texture) {
    if (texture != g_state.active_texture) {
        g_state.active_texture = texture;
        ++g_state.material_epoch;
    }
    g_orig.active_texture(texture);
}

void probe_glBindTexture(GLenum target, GLuint texture) {
    // Backend calls reaching this layer have already passed the frontend's exact
    // redundant-bind filtering. Treat every surviving bind as a material break.
    ++g_state.material_epoch;
    g_orig.bind_texture(target, texture);
}

template <typename Pointer, typename Slot> Pointer capture(const Slot& slot) {
    return static_cast<Pointer>(slot);
}

} // namespace

void mg_pz_repack_probe_install(void) {
    const char* value = std::getenv("MOBILEGLUES_PZ_REPACK_PROBE");
    if (value == nullptr || std::strcmp(value, "1") != 0) return;

    g_orig.draw_elements = capture<glDrawElements_PTR>(GLES.glDrawElements);
    g_orig.draw_elements_instanced = capture<glDrawElementsInstanced_PTR>(GLES.glDrawElementsInstanced);
    g_orig.draw_elements_base_vertex = capture<glDrawElementsBaseVertex_PTR>(GLES.glDrawElementsBaseVertex);
    g_orig.draw_elements_instanced_base_vertex =
        capture<glDrawElementsInstancedBaseVertex_PTR>(GLES.glDrawElementsInstancedBaseVertex);
    g_orig.draw_range_elements = capture<glDrawRangeElements_PTR>(GLES.glDrawRangeElements);
    g_orig.draw_range_elements_base_vertex =
        capture<glDrawRangeElementsBaseVertex_PTR>(GLES.glDrawRangeElementsBaseVertex);
    g_orig.use_program = capture<glUseProgram_PTR>(GLES.glUseProgram);
    g_orig.active_texture = capture<glActiveTexture_PTR>(GLES.glActiveTexture);
    g_orig.bind_texture = capture<glBindTexture_PTR>(GLES.glBindTexture);

    if (g_orig.draw_elements == nullptr || g_orig.use_program == nullptr || g_orig.active_texture == nullptr ||
        g_orig.bind_texture == nullptr) {
        LOG_W_FORCE("ZOMDROID_PZ_REPACK_PROBE disabled reason=backend_capability_missing")
        return;
    }

    g_enabled = true;
    GLES.glDrawElements = probe_glDrawElements;
    if (g_orig.draw_elements_instanced) GLES.glDrawElementsInstanced = probe_glDrawElementsInstanced;
    if (g_orig.draw_elements_base_vertex) GLES.glDrawElementsBaseVertex = probe_glDrawElementsBaseVertex;
    if (g_orig.draw_elements_instanced_base_vertex)
        GLES.glDrawElementsInstancedBaseVertex = probe_glDrawElementsInstancedBaseVertex;
    if (g_orig.draw_range_elements) GLES.glDrawRangeElements = probe_glDrawRangeElements;
    if (g_orig.draw_range_elements_base_vertex)
        GLES.glDrawRangeElementsBaseVertex = probe_glDrawRangeElementsBaseVertex;
    GLES.glUseProgram = probe_glUseProgram;
    GLES.glActiveTexture = probe_glActiveTexture;
    GLES.glBindTexture = probe_glBindTexture;

    LOG_I("ZOMDROID_PZ_REPACK_PROBE enabled=1 mode=backend_observe_only tiny_max=96 "
          "quad6=shape_only_not_topology_validated relaxed_upper=ignores_vertex_buffer_attrib_and_uniform_fixed_state")
}

#else

void mg_pz_repack_probe_install(void) {}

#endif
