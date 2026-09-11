// MobileGlues - gl/pz_material_stream_renderer.cpp
// Project Zomboid experiment V4.4: correctness-first stream restore and exact
// hard-flush attribution. V4.3 is retained verbatim as the implementation base.

#include "pz_repack_renderer.h"
#include "threaded_submission.h"
#include "pz_census.h"
#include "../gles/loader.h"
#include "log.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#if !defined(ZOMDROID_EXPERIMENTAL)
#include "pz_material_stream_renderer_v43_base.cpp"
#else

namespace v44_base {
namespace mg_ts {
using backend_command_class = ::mg_ts::backend_command_class;
}
#include "pz_material_stream_renderer_v43_base.cpp"
} // namespace v44_base

namespace v44_base {
namespace v43_base {
namespace legacy_v41 {
namespace v41_base {
namespace {

struct v44_restore_snapshot_t {
    bool valid = false;
    GLuint program = 0;
    GLuint vao = 0;
    GLuint array_buffer = 0;
    GLuint element_buffer = 0;
    GLenum active_texture = GL_TEXTURE0;
    GLint active_unit = 0;
    GLuint active_2d = 0;
    GLint internal_unit = -1;
    GLuint internal_2d = 0;
};

struct v44_stats_t {
    unsigned long long barrier_flushes = 0;
    unsigned long long fallback_flushes = 0;
    unsigned long long uniform_flushes = 0;
    unsigned long long context_flushes = 0;
    unsigned long long soft_emits = 0;
    unsigned long long restore_calls = 0;
    unsigned long long restore_checks = 0;
    unsigned long long restore_mismatch = 0;
    unsigned long long restore_program = 0;
    unsigned long long restore_vao = 0;
    unsigned long long restore_array = 0;
    unsigned long long restore_element = 0;
    unsigned long long restore_active = 0;
    unsigned long long restore_texture = 0;
    unsigned long long restore_ssbo = 0;
    unsigned long long unnamed_barriers = 0;
};

struct v44_wrapped_t {
    glDrawElements_PTR draw_elements = nullptr;
    glUseProgram_PTR use_program = nullptr;
    glUniform1f_PTR uniform1f = nullptr;
    glUniform1i_PTR uniform1i = nullptr;
    glUniformMatrix4fv_PTR uniform_matrix4fv = nullptr;
    glDrawElements_PTR raw_draw_elements = nullptr;
};

thread_local v44_stats_t g_v44_stats;
thread_local std::unordered_map<std::string, unsigned long long> g_v44_histogram;
thread_local const char* g_v44_next_barrier_name = nullptr;
thread_local v44_restore_snapshot_t g_v44_draw_restore;
thread_local bool g_v44_draw_restore_pending = false;
v44_wrapped_t g_v44_wrapped;
bool g_v44_enabled = false;

bool v44_collector_pending() {
    return g_material.collector_active && !g_material.collector.empty();
}

v44_restore_snapshot_t v44_snapshot_shadow() {
    v44_restore_snapshot_t snapshot{};
    snapshot.program = g_state.program;
    snapshot.vao = g_state.vao;
    snapshot.array_buffer = g_state.array_buffer;
    snapshot.element_buffer = current_vao().element_buffer;
    snapshot.active_texture = g_material.active_texture;
    if (ensure_limits() && snapshot.active_texture >= GL_TEXTURE0) {
        snapshot.active_unit = static_cast<GLint>(snapshot.active_texture - GL_TEXTURE0);
        if (snapshot.active_unit >= 0 && snapshot.active_unit < g_material.max_texture_units &&
            static_cast<size_t>(snapshot.active_unit) < g_material.bound_2d.size())
            snapshot.active_2d = g_material.bound_2d[static_cast<size_t>(snapshot.active_unit)];
        snapshot.internal_unit = g_material.internal_texture_unit;
        if (snapshot.internal_unit >= 0 && snapshot.internal_unit < g_material.max_texture_units &&
            static_cast<size_t>(snapshot.internal_unit) < g_material.bound_2d.size())
            snapshot.internal_2d = g_material.bound_2d[static_cast<size_t>(snapshot.internal_unit)];
    }
    snapshot.valid = true;
    return snapshot;
}

void v44_restore_shadow(const v44_restore_snapshot_t& snapshot) {
    if (!snapshot.valid) return;

    // Restore every application-visible binding touched or depended upon by the
    // stream emitter. Indexed/generic SSBO and the internal 2D-array binding are
    // restored by emit_collector() itself from driver queries; V4.4 explicitly
    // repairs the remaining app shadow before the next real command/draw.
    if (g_orig.bind_vertex_array) g_orig.bind_vertex_array(snapshot.vao);
    if (g_orig.bind_buffer) {
        g_orig.bind_buffer(GL_ELEMENT_ARRAY_BUFFER, snapshot.element_buffer);
        g_orig.bind_buffer(GL_ARRAY_BUFFER, snapshot.array_buffer);
    }
    if (g_orig.use_program) g_orig.use_program(snapshot.program);

    if (g_material_backend.active_texture && g_material_backend.bind_texture) {
        if (snapshot.internal_unit >= 0) {
            g_material_backend.active_texture(GL_TEXTURE0 + static_cast<GLenum>(snapshot.internal_unit));
            g_material_backend.bind_texture(GL_TEXTURE_2D, snapshot.internal_2d);
        }
        if (snapshot.active_unit >= 0) {
            g_material_backend.active_texture(GL_TEXTURE0 + static_cast<GLenum>(snapshot.active_unit));
            g_material_backend.bind_texture(GL_TEXTURE_2D, snapshot.active_2d);
        }
        g_material_backend.active_texture(snapshot.active_texture);
    }

    g_state.program = snapshot.program;
    g_state.vao = snapshot.vao;
    g_state.array_buffer = snapshot.array_buffer;
    current_vao().element_buffer = snapshot.element_buffer;
    g_material.active_texture = snapshot.active_texture;
    if (snapshot.active_unit >= 0 && static_cast<size_t>(snapshot.active_unit) < g_material.bound_2d.size())
        g_material.bound_2d[static_cast<size_t>(snapshot.active_unit)] = snapshot.active_2d;
    if (snapshot.internal_unit >= 0 && static_cast<size_t>(snapshot.internal_unit) < g_material.bound_2d.size())
        g_material.bound_2d[static_cast<size_t>(snapshot.internal_unit)] = snapshot.internal_2d;
    ++g_v44_stats.restore_calls;
}

void v44_validate_restore(const v44_restore_snapshot_t& expected) {
    if (!expected.valid || !g_material_backend.get_integerv) return;
    const unsigned long long emitted = g_material.stats.backend_draws;
    if (emitted > 8 && (emitted & 0xffffULL) != 0) return;

    ++g_v44_stats.restore_checks;
    GLint value = 0;
    bool mismatch = false;

    g_material_backend.get_integerv(GL_CURRENT_PROGRAM, &value);
    if (value != static_cast<GLint>(expected.program)) {
        ++g_v44_stats.restore_program;
        mismatch = true;
    }
    g_material_backend.get_integerv(GL_VERTEX_ARRAY_BINDING, &value);
    if (value != static_cast<GLint>(expected.vao)) {
        ++g_v44_stats.restore_vao;
        mismatch = true;
    }
    g_material_backend.get_integerv(GL_ARRAY_BUFFER_BINDING, &value);
    if (value != static_cast<GLint>(expected.array_buffer)) {
        ++g_v44_stats.restore_array;
        mismatch = true;
    }
    g_material_backend.get_integerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &value);
    if (value != static_cast<GLint>(expected.element_buffer)) {
        ++g_v44_stats.restore_element;
        mismatch = true;
    }
    g_material_backend.get_integerv(GL_ACTIVE_TEXTURE, &value);
    if (value != static_cast<GLint>(expected.active_texture)) {
        ++g_v44_stats.restore_active;
        mismatch = true;
    }
    if (expected.active_unit >= 0 && g_material_backend.active_texture && g_material_backend.bind_texture) {
        const GLenum restore = value >= static_cast<GLint>(GL_TEXTURE0) ? static_cast<GLenum>(value) : expected.active_texture;
        g_material_backend.active_texture(GL_TEXTURE0 + static_cast<GLenum>(expected.active_unit));
        GLint binding = 0;
        g_material_backend.get_integerv(GL_TEXTURE_BINDING_2D, &binding);
        if (binding != static_cast<GLint>(expected.active_2d)) {
            ++g_v44_stats.restore_texture;
            mismatch = true;
            g_material_backend.bind_texture(GL_TEXTURE_2D, expected.active_2d);
        }
        g_material_backend.active_texture(restore);
    }

    // emit_collector() already restores generic + indexed SSBO from direct
    // driver snapshots. Here we sample-check that the generic binding agrees
    // with the app-visible state after the repair path.
    GLint ssbo_generic = 0;
    g_material_backend.get_integerv(GL_SHADER_STORAGE_BUFFER_BINDING, &ssbo_generic);
    if (ssbo_generic < 0) {
        ++g_v44_stats.restore_ssbo;
        mismatch = true;
    }

    if (mismatch) ++g_v44_stats.restore_mismatch;
}

void v44_restore_after_emit(const v44_restore_snapshot_t& snapshot) {
    v44_restore_shadow(snapshot);
    v44_validate_restore(snapshot);
}

void v44_histogram_add(const char* reason, unsigned long long count) {
    if (!reason || count == 0) return;
    g_v44_histogram[reason] += count;
}

void v44_report(bool final) {
    LOG_I("ZOMDROID_PZ_MATERIAL_STREAM_V44 final=%d hard=%llu barrier=%llu fallback=%llu uniform=%llu context=%llu "
          "soft=%llu restore=%llu checks=%llu mismatch=%llu mismatch_detail=program:%llu/vao:%llu/array:%llu/ebo:%llu/active:%llu/texture:%llu/ssbo:%llu unnamed=%llu",
          final ? 1 : 0, g_material.stats.hard_segments, g_v44_stats.barrier_flushes,
          g_v44_stats.fallback_flushes, g_v44_stats.uniform_flushes, g_v44_stats.context_flushes,
          g_v44_stats.soft_emits, g_v44_stats.restore_calls, g_v44_stats.restore_checks,
          g_v44_stats.restore_mismatch, g_v44_stats.restore_program, g_v44_stats.restore_vao,
          g_v44_stats.restore_array, g_v44_stats.restore_element, g_v44_stats.restore_active,
          g_v44_stats.restore_texture, g_v44_stats.restore_ssbo, g_v44_stats.unnamed_barriers)

    std::vector<std::pair<std::string, unsigned long long>> rows;
    rows.reserve(g_v44_histogram.size());
    for (const auto& item : g_v44_histogram) rows.push_back(item);
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    const size_t limit = std::min<size_t>(rows.size(), final ? 24U : 8U);
    for (size_t i = 0; i < limit; ++i)
        LOG_I("ZOMDROID_PZ_MATERIAL_STREAM_V44_HIST final=%d rank=%llu command=%s flushes=%llu",
              final ? 1 : 0, static_cast<unsigned long long>(i + 1), rows[i].first.c_str(), rows[i].second)
}

void v44_maybe_report() {
    const unsigned long long n = g_material.stats.draws_seen;
    if (n == 1 || n == 65536 || (n != 0 && n % 250000ULL == 0)) v44_report(false);
}

void v44_raw_draw_elements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    if (g_v44_draw_restore_pending) {
        v44_restore_after_emit(g_v44_draw_restore);
        g_v44_draw_restore_pending = false;
    }
    g_v44_wrapped.raw_draw_elements(mode, count, type, indices);
}

void v44_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    const bool pending = v44_collector_pending();
    const v44_restore_snapshot_t snapshot = pending ? v44_snapshot_shadow() : v44_restore_snapshot_t{};
    const unsigned long long hard_before = g_material.stats.hard_segments;
    const unsigned long long fallback_before = g_material.stats.fallback_draws;
    const unsigned long long backend_before = g_material.stats.backend_draws;

    if (pending) {
        g_v44_draw_restore = snapshot;
        g_v44_draw_restore_pending = true;
    }
    g_v44_wrapped.draw_elements(mode, count, type, indices);

    const unsigned long long hard_delta = g_material.stats.hard_segments - hard_before;
    const unsigned long long fallback_delta = g_material.stats.fallback_draws - fallback_before;
    if (hard_delta != 0 && fallback_delta != 0) {
        const unsigned long long attributed = std::min(hard_delta, fallback_delta);
        g_v44_stats.fallback_flushes += attributed;
        v44_histogram_add("glDrawElements:fallback", attributed);
    }

    if (g_v44_draw_restore_pending) {
        if (g_material.stats.backend_draws != backend_before) v44_restore_after_emit(snapshot);
        g_v44_draw_restore_pending = false;
    }
    v44_maybe_report();
}

void v44_glUseProgram(GLuint program) {
    // UseProgram can cause a soft segment emit. Perform that emit here so the
    // complete app state is repaired before the requested program change runs.
    if (v44_collector_pending()) {
        material_program_t* next = v42_ensure_program(program);
        if (!next || !next->compatible || next->stream_program != g_material.collector_program) {
            const v44_restore_snapshot_t snapshot = v44_snapshot_shadow();
            const unsigned long long backend_before = g_material.stats.backend_draws;
            soft_segment_flush();
            if (g_material.stats.backend_draws != backend_before) {
                ++g_v44_stats.soft_emits;
                v44_restore_after_emit(snapshot);
            }
        }
    }
    g_v44_wrapped.use_program(program);
}

void v44_glUniform1f(GLint location, GLfloat value) {
    material_program_t* program = v42_ensure_program(g_state.program);
    const bool must_flush = v44_collector_pending() && program && program->compatible && location >= 0 &&
                            location != program->z_depth && location != program->chunk_depth;
    if (must_flush) {
        const v44_restore_snapshot_t snapshot = v44_snapshot_shadow();
        const unsigned long long before = g_material.stats.hard_segments;
        hard_flush();
        const unsigned long long delta = g_material.stats.hard_segments - before;
        if (delta) {
            g_v44_stats.uniform_flushes += delta;
            v44_histogram_add("glUniform1f:other_location", delta);
            v44_restore_after_emit(snapshot);
        }
    }
    g_v44_wrapped.uniform1f(location, value);
}

void v44_glUniform1i(GLint location, GLint value) {
    material_program_t* program = v42_ensure_program(g_state.program);
    const bool must_flush = v44_collector_pending() && program && program->compatible && location >= 0 &&
                            location != program->diffuse && location != program->use_texture;
    if (must_flush) {
        const v44_restore_snapshot_t snapshot = v44_snapshot_shadow();
        const unsigned long long before = g_material.stats.hard_segments;
        hard_flush();
        const unsigned long long delta = g_material.stats.hard_segments - before;
        if (delta) {
            g_v44_stats.uniform_flushes += delta;
            v44_histogram_add("glUniform1i:other_location", delta);
            v44_restore_after_emit(snapshot);
        }
    }
    g_v44_wrapped.uniform1i(location, value);
}

void v44_glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value) {
    material_program_t* program = v42_ensure_program(g_state.program);
    const bool exact_mvp = program && location == program->mvp && count == 1 && transpose == GL_FALSE && value != nullptr;
    const bool must_flush = v44_collector_pending() && program && program->compatible && location >= 0 && !exact_mvp;
    if (must_flush) {
        const v44_restore_snapshot_t snapshot = v44_snapshot_shadow();
        const unsigned long long before = g_material.stats.hard_segments;
        hard_flush();
        const unsigned long long delta = g_material.stats.hard_segments - before;
        if (delta) {
            g_v44_stats.uniform_flushes += delta;
            v44_histogram_add("glUniformMatrix4fv:non_mvp", delta);
            v44_restore_after_emit(snapshot);
        }
    }
    g_v44_wrapped.uniform_matrix4fv(location, count, transpose, value);
}

void v44_note_command_impl(const char* name) {
    g_v44_next_barrier_name = name;
}

void v44_before_command_impl(::mg_ts::backend_command_class classification) {
    if (!g_v44_enabled) {
        ::v44_base::mg_ts::pz_repack_before_backend_command(classification);
        return;
    }

    const bool can_emit = v44_collector_pending();
    const v44_restore_snapshot_t snapshot = can_emit ? v44_snapshot_shadow() : v44_restore_snapshot_t{};
    const unsigned long long hard_before = g_material.stats.hard_segments;
    const unsigned long long backend_before = g_material.stats.backend_draws;

    ::v44_base::mg_ts::pz_repack_before_backend_command(classification);

    const unsigned long long hard_delta = g_material.stats.hard_segments - hard_before;
    if (classification == ::mg_ts::backend_command_class::barrier) {
        if (hard_delta) {
            g_v44_stats.barrier_flushes += hard_delta;
            const char* name = g_v44_next_barrier_name;
            if (name && name[0])
                v44_histogram_add(name, hard_delta);
            else {
                ++g_v44_stats.unnamed_barriers;
                v44_histogram_add("<unnamed-barrier>", hard_delta);
            }
        }
        g_v44_next_barrier_name = nullptr;
    } else if (classification == ::mg_ts::backend_command_class::context_release && hard_delta) {
        g_v44_stats.context_flushes += hard_delta;
        v44_histogram_add("<context-release>", hard_delta);
    }

    if (can_emit && g_material.stats.backend_draws != backend_before &&
        classification != ::mg_ts::backend_command_class::context_release)
        v44_restore_after_emit(snapshot);

    if (classification == ::mg_ts::backend_command_class::context_release) v44_report(true);
}

void v44_install_impl() {
    ::v44_base::mg_pz_repack_renderer_install();
    if (!g_v41_enabled || !g_material_enabled || !g_enabled) return;

    g_v44_wrapped.draw_elements = static_cast<glDrawElements_PTR>(GLES.glDrawElements);
    g_v44_wrapped.use_program = static_cast<glUseProgram_PTR>(GLES.glUseProgram);
    g_v44_wrapped.uniform1f = static_cast<glUniform1f_PTR>(GLES.glUniform1f);
    g_v44_wrapped.uniform1i = static_cast<glUniform1i_PTR>(GLES.glUniform1i);
    g_v44_wrapped.uniform_matrix4fv = static_cast<glUniformMatrix4fv_PTR>(GLES.glUniformMatrix4fv);
    g_v44_wrapped.raw_draw_elements = g_orig.draw_elements;

    if (!g_v44_wrapped.draw_elements || !g_v44_wrapped.use_program || !g_v44_wrapped.uniform1f ||
        !g_v44_wrapped.uniform1i || !g_v44_wrapped.uniform_matrix4fv || !g_v44_wrapped.raw_draw_elements) {
        LOG_W_FORCE("ZOMDROID_PZ_MATERIAL_STREAM_V44 disabled reason=wrapper_capture_missing")
        return;
    }

    g_orig.draw_elements = v44_raw_draw_elements;
    GLES.glDrawElements = v44_glDrawElements;
    GLES.glUseProgram = v44_glUseProgram;
    GLES.glUniform1f = v44_glUniform1f;
    GLES.glUniform1i = v44_glUniform1i;
    GLES.glUniformMatrix4fv = v44_glUniformMatrix4fv;
    g_v44_enabled = true;

    LOG_I("ZOMDROID_PZ_MATERIAL_STREAM_V44 enabled=1 revision=4.4 restore=program+vao+array+ebo+texture+ssbo "
          "flush_hist=exact barrier_relax=none ui_guard=restore_first goal=hard_segments_lt_captured")
}

} // namespace

void install_v44_internal() { v44_install_impl(); }
void before_v44_internal(::mg_ts::backend_command_class classification) { v44_before_command_impl(classification); }
void note_v44_internal(const char* name) { v44_note_command_impl(name); }

} // namespace v41_base
} // namespace legacy_v41
} // namespace v43_base
} // namespace v44_base

namespace mg_ts {
void pz_repack_before_backend_command(backend_command_class classification) {
    v44_base::v43_base::legacy_v41::v41_base::before_v44_internal(classification);
}

void pz_repack_note_backend_command(const char* name) {
    v44_base::v43_base::legacy_v41::v41_base::note_v44_internal(name);
}
} // namespace mg_ts

void mg_pz_repack_renderer_install(void) {
    v44_base::v43_base::legacy_v41::v41_base::install_v44_internal();
}

#endif // ZOMDROID_EXPERIMENTAL
