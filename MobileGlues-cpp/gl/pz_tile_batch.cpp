// MobileGlues - gl/pz_tile_batch.cpp
// Experimental order-preserving Project Zomboid StateRun batching.

#include "pz_tile_batch.h"

#if defined(ZOMDROID_EXPERIMENTAL)

#include "buffer.h"
#include "glsl/shader_compat.h"
#include "log.h"
#include "mg.h"
#include "pz_census.h"
#include "threaded_submission.h"
#include "../egl/context.h"
#include "../gles/loader.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace {

constexpr size_t kMaxRuns = static_cast<size_t>(mg_glsl_compat::k_pz_tile_batch_max_runs);
constexpr size_t kMinimumCombinedRuns = 3;

struct program_state {
    GLint z_depth = -1;
    GLint chunk_depth = -1;
    GLint run_count = -1;
    GLint run_start = -1;
    GLint run_depth = -1;
    bool ready = false;
    GLfloat current_z = 0.0f;
    GLfloat current_chunk = 0.0f;
    bool z_known = true;
    bool chunk_known = true;
};

struct draw_run {
    GLuint start = 0;
    GLuint end = 0;
    GLsizei count = 0;
    uintptr_t offset = 0;
    GLfloat z = 0.0f;
    GLfloat chunk = 0.0f;
};

struct pending_batch {
    GLuint program = 0;
    GLuint element_buffer = 0;
    GLenum mode = GL_TRIANGLES;
    GLenum type = GL_UNSIGNED_SHORT;
    std::array<draw_run, kMaxRuns> runs{};
    size_t count = 0;
    GLfloat current_z = 0.0f;
    GLfloat current_chunk = 0.0f;
    bool z_dirty = false;
    bool chunk_dirty = false;
    bool inside_flush = false;
    unsigned long long context_id = 0;
};

struct batch_stats {
    unsigned long long frames = 0;
    unsigned long long candidates = 0;
    unsigned long long combined_batches = 0;
    unsigned long long combined_runs = 0;
    unsigned long long draws_saved = 0;
    unsigned long long single_fallbacks = 0;
    unsigned long long incompatible = 0;
    unsigned long long max_run = 0;
};

std::unordered_map<GLuint, bool> g_shader_eligible;
std::unordered_map<GLuint, std::unordered_set<GLuint>> g_program_shaders;
std::unordered_map<GLuint, program_state> g_programs;
thread_local pending_batch g_pending;
thread_local batch_stats g_stats;

bool enabled() {
    return mg_pz_tile_batch_active && mg_pz_threaded_submission_active && mg_ts::availableAndActive();
}

void reset_for_context() {
    const unsigned long long current = g_current_ctx ? g_current_ctx->id : 0;
    if (g_pending.context_id == current) return;
    g_pending = {};
    g_pending.context_id = current;
}

program_state* current_program_state(GLuint program) {
    auto found = g_programs.find(program);
    return found != g_programs.end() && found->second.ready ? &found->second : nullptr;
}

bool same_bits(GLfloat lhs, GLfloat rhs) {
    uint32_t lhs_bits = 0;
    uint32_t rhs_bits = 0;
    std::memcpy(&lhs_bits, &lhs, sizeof(lhs_bits));
    std::memcpy(&rhs_bits, &rhs, sizeof(rhs_bits));
    return lhs_bits == rhs_bits;
}

void upload_original_depth(const program_state& program, GLfloat z, GLfloat chunk,
                           bool upload_z, bool upload_chunk) {
    if (upload_z) GLES.glUniform1f(program.z_depth, z);
    if (upload_chunk) GLES.glUniform1f(program.chunk_depth, chunk);
}

void execute_singles(const program_state& program) {
    bool z_known = false;
    bool chunk_known = false;
    GLfloat driver_z = 0.0f;
    GLfloat driver_chunk = 0.0f;
    for (size_t index = 0; index < g_pending.count; ++index) {
        const draw_run& run = g_pending.runs[index];
        const bool upload_z = !z_known || !same_bits(driver_z, run.z);
        const bool upload_chunk = !chunk_known || !same_bits(driver_chunk, run.chunk);
        upload_original_depth(program, run.z, run.chunk, upload_z, upload_chunk);
        GLES.glDrawRangeElements(g_pending.mode, run.start, run.end, run.count, g_pending.type,
                                 reinterpret_cast<const void*>(run.offset));
        driver_z = run.z;
        driver_chunk = run.chunk;
        z_known = true;
        chunk_known = true;
    }
    // Uniforms for the next StateRun can arrive before its texture/state
    // boundary flushes the retained draw. Preserve that ordering even when the
    // retained run was too short to combine.
    upload_original_depth(program, g_pending.current_z, g_pending.current_chunk,
                          !same_bits(driver_z, g_pending.current_z),
                          !same_bits(driver_chunk, g_pending.current_chunk));
    if (mg_pz_census_active) g_stats.single_fallbacks += g_pending.count;
}

void execute_combined(const program_state& program) {
    std::array<GLint, kMaxRuns> starts{};
    std::array<GLfloat, kMaxRuns * 2> depths{};
    GLsizei total_count = 0;
    for (size_t index = 0; index < g_pending.count; ++index) {
        const draw_run& run = g_pending.runs[index];
        starts[index] = static_cast<GLint>(run.start);
        depths[index * 2] = run.z;
        depths[index * 2 + 1] = run.chunk;
        total_count += run.count;
    }

    const GLint run_count = static_cast<GLint>(g_pending.count);
    GLES.glUniform1i(program.run_count, run_count);
    GLES.glUniform1iv(program.run_start, run_count, starts.data());
    GLES.glUniform2fv(program.run_depth, run_count, depths.data());
    const draw_run& first = g_pending.runs[0];
    const draw_run& last = g_pending.runs[g_pending.count - 1];
    GLES.glDrawRangeElements(g_pending.mode, first.start, last.end, total_count, g_pending.type,
                             reinterpret_cast<const void*>(first.offset));

    // Leave the rewritten shader indistinguishable from its original form for
    // every draw that is not retained by this batcher.
    GLES.glUniform1i(program.run_count, 0);
    upload_original_depth(program, g_pending.current_z, g_pending.current_chunk, true, true);

    if (mg_pz_census_active) {
        ++g_stats.combined_batches;
        g_stats.combined_runs += g_pending.count;
        g_stats.draws_saved += g_pending.count - 1;
        if (g_pending.count > g_stats.max_run) g_stats.max_run = g_pending.count;
    }
}

void flush_pending() {
    if (g_pending.inside_flush) return;
    g_pending.inside_flush = true;

    program_state* program = current_program_state(g_pending.program);
    if (g_pending.count != 0 && program != nullptr) {
        if (g_pending.count >= kMinimumCombinedRuns)
            execute_combined(*program);
        else
            execute_singles(*program);
        g_pending.z_dirty = false;
        g_pending.chunk_dirty = false;
    } else if (program != nullptr && (g_pending.z_dirty || g_pending.chunk_dirty)) {
        upload_original_depth(*program, g_pending.current_z, g_pending.current_chunk,
                              g_pending.z_dirty, g_pending.chunk_dirty);
        g_pending.z_dirty = false;
        g_pending.chunk_dirty = false;
    }

    g_pending.count = 0;
    g_pending.program = 0;
    g_pending.element_buffer = 0;
    g_pending.inside_flush = false;
}

} // namespace

void mg_pz_tile_batch_note_shader(GLuint shader, bool eligible) {
    if (eligible)
        g_shader_eligible[shader] = true;
    else
        g_shader_eligible.erase(shader);
}

bool mg_pz_tile_batch_shader_eligible(GLuint shader) {
    const auto found = g_shader_eligible.find(shader);
    return found != g_shader_eligible.end() && found->second;
}

void mg_pz_tile_batch_shader_deleted(GLuint shader) { g_shader_eligible.erase(shader); }

void mg_pz_tile_batch_attach_shader(GLuint program, GLuint shader) {
    g_program_shaders[program].insert(shader);
    g_programs.erase(program);
}

void mg_pz_tile_batch_detach_shader(GLuint program, GLuint shader) {
    auto found = g_program_shaders.find(program);
    if (found != g_program_shaders.end()) {
        found->second.erase(shader);
        if (found->second.empty()) g_program_shaders.erase(found);
    }
    g_programs.erase(program);
}

void mg_pz_tile_batch_program_linked(GLuint program) {
    g_programs.erase(program);
    if (!mg_pz_tile_batch_active) return;
    const auto attached = g_program_shaders.find(program);
    if (attached == g_program_shaders.end()) return;
    size_t eligible = 0;
    for (GLuint shader : attached->second) {
        if (mg_pz_tile_batch_shader_eligible(shader)) ++eligible;
    }
    if (eligible != 1) return;

    GLint linked = GL_FALSE;
    GLES.glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) return;

    program_state state;
    state.z_depth = GLES.glGetUniformLocation(program, "zDepth");
    state.chunk_depth = GLES.glGetUniformLocation(program, "chunkDepth");
    state.run_count = GLES.glGetUniformLocation(program, "zomdroidBatchRunCount");
    state.run_start = GLES.glGetUniformLocation(program, "zomdroidBatchRunStart[0]");
    state.run_depth = GLES.glGetUniformLocation(program, "zomdroidBatchDepth[0]");
    state.ready = state.z_depth >= 0 && state.chunk_depth >= 0 && state.run_count >= 0 &&
                  state.run_start >= 0 && state.run_depth >= 0;
    if (!state.ready) return;
    g_programs[program] = state;
#if defined(ZOMDROID_GL_BREADCRUMBS)
    if (mg_pz_census_active) {
        ZOMDROID_DIAGNOSTIC_LOG(
            "ZOMDROID_PZ_TILE_BATCH_PROGRAM program=%u z=%d chunk=%d count=%d start=%d depth=%d ready=1",
            program, state.z_depth, state.chunk_depth, state.run_count, state.run_start, state.run_depth);
    }
#endif
}

void mg_pz_tile_batch_program_deleted(GLuint program) {
    if (g_pending.program == program) flush_pending();
    g_programs.erase(program);
    g_program_shaders.erase(program);
}

bool mg_pz_tile_batch_uniform1f(GLuint program, GLint location, GLfloat value) {
    if (!enabled()) return false;
    reset_for_context();
    program_state* state = current_program_state(program);
    if (state == nullptr || (location != state->z_depth && location != state->chunk_depth)) return false;
    if (g_pending.program != 0 && g_pending.program != program) flush_pending();
    if (g_pending.program == 0) {
        g_pending.program = program;
        g_pending.current_z = state->current_z;
        g_pending.current_chunk = state->current_chunk;
    }
    if (location == state->z_depth) {
        g_pending.current_z = value;
        g_pending.z_dirty = true;
        state->current_z = value;
        state->z_known = true;
    } else {
        g_pending.current_chunk = value;
        g_pending.chunk_dirty = true;
        state->current_chunk = value;
        state->chunk_known = true;
    }
    return true;
}

bool mg_pz_tile_batch_draw_range(GLenum mode, GLuint start, GLuint end, GLsizei count,
                                 GLenum type, const void* indices) {
    if (!enabled() || mode != GL_TRIANGLES || type != GL_UNSIGNED_SHORT || count <= 0 || count % 3 != 0 ||
        start > end || end > static_cast<GLuint>(std::numeric_limits<GLint>::max()))
        return false;
    reset_for_context();
    const GLuint program_name = gl_state ? gl_state->current_program : 0;
    program_state* state = current_program_state(program_name);
    if (state == nullptr || !state->z_known || !state->chunk_known) return false;
    if (g_pending.program != 0 && g_pending.program != program_name) flush_pending();
    if (g_pending.program == 0) {
        g_pending.program = program_name;
        g_pending.current_z = state->current_z;
        g_pending.current_chunk = state->current_chunk;
    }
    const GLuint element_buffer = mg_driver_bound_buffer(GL_ELEMENT_ARRAY_BUFFER);
    const uintptr_t offset = reinterpret_cast<uintptr_t>(indices);
    if (element_buffer == 0 || offset > static_cast<uintptr_t>(std::numeric_limits<uint32_t>::max())) return false;

    if (mg_pz_census_active) ++g_stats.candidates;
    if (g_pending.count != 0) {
        const draw_run& previous = g_pending.runs[g_pending.count - 1];
        const uint64_t expected_offset = static_cast<uint64_t>(previous.offset) +
                                         static_cast<uint64_t>(previous.count) * sizeof(GLushort);
        uint64_t total = 0;
        for (size_t i = 0; i < g_pending.count; ++i)
            total += static_cast<uint64_t>(g_pending.runs[i].count);
        const bool total_fits = total <= static_cast<uint64_t>(std::numeric_limits<GLsizei>::max()) &&
                                static_cast<uint64_t>(count) <=
                                    static_cast<uint64_t>(std::numeric_limits<GLsizei>::max()) - total;
        const bool compatible = g_pending.program == program_name && g_pending.element_buffer == element_buffer &&
                                g_pending.mode == mode && g_pending.type == type &&
                                static_cast<uint64_t>(offset) == expected_offset && start > previous.end && total_fits;
        if (!compatible || g_pending.count == kMaxRuns) {
            if (mg_pz_census_active && !compatible) ++g_stats.incompatible;
            flush_pending();
        }
    }

    if (g_pending.count == 0) {
        g_pending.program = program_name;
        g_pending.element_buffer = element_buffer;
        g_pending.mode = mode;
        g_pending.type = type;
    }
    g_pending.runs[g_pending.count++] =
        {start, end, count, offset, g_pending.current_z, g_pending.current_chunk};
    return true;
}

void mg_pz_tile_batch_before_backend(const char* command) {
    if (!mg_pz_tile_batch_active || g_pending.inside_flush) return;
    reset_for_context();
    flush_pending();
    if (command == nullptr) return;
    if (std::strcmp(command, "glProgramUniform1f") == 0 ||
        std::strcmp(command, "glProgramUniform1fv") == 0) {
        for (auto& entry : g_programs) {
            entry.second.z_known = false;
            entry.second.chunk_known = false;
        }
    } else if (std::strcmp(command, "glUniform1fv") == 0 && gl_state != nullptr) {
        program_state* state = current_program_state(gl_state->current_program);
        if (state != nullptr) {
            state->z_known = false;
            state->chunk_known = false;
        }
    }
}

void mg_pz_tile_batch_flush() {
    if (!mg_pz_tile_batch_active) return;
    reset_for_context();
    flush_pending();
}

void mg_pz_tile_batch_present() {
    if (!mg_pz_tile_batch_active) return;
    mg_pz_tile_batch_flush();
    if (!mg_pz_census_active) return;
    ++g_stats.frames;
    if (g_stats.frames != 60 && g_stats.frames % 300 != 0) return;
    LOG_I("ZOMDROID_PZ_TILE_BATCH frames=%llu candidates=%llu batches=%llu runs=%llu saved=%llu "
          "single=%llu incompatible=%llu max_run=%llu",
          g_stats.frames, g_stats.candidates, g_stats.combined_batches, g_stats.combined_runs,
          g_stats.draws_saved, g_stats.single_fallbacks, g_stats.incompatible, g_stats.max_run)
}

#endif
