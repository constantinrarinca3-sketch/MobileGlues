// MobileGlues - gl/pz_tile_batch.cpp
// Experimental order-preserving Project Zomboid StateRun compiler.

#include "pz_tile_batch.h"

#if defined(ZOMDROID_EXPERIMENTAL)

#include "buffer.h"
#include "glsl/shader_compat.h"
#include "log.h"
#include "mg.h"
#include "pz_census.h"
#include "threaded_submission.h"
#include "../egl/context.h"
#include "../egl/loader.h"
#include "../gles/loader.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace {

// Eight complete StateRuns plus their shader locations fit inside one 256-byte
// threaded-submission command. This keeps the compiler allocation-free on the
// producer path.
constexpr size_t kMaxRuns = 8;
constexpr size_t kMinimumCombinedRuns = 2;

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
    uint32_t offset = 0;
    GLfloat z = 0.0f;
    GLfloat chunk = 0.0f;
};

struct pending_batch {
    GLuint program = 0;
    GLuint element_buffer = 0;
    GLuint frontend_element_buffer = 0;
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
    unsigned long long scheduled_batches = 0;
    unsigned long long scheduled_runs = 0;
    unsigned long long single_fallbacks = 0;
    unsigned long long incompatible = 0;
    unsigned long long ranges_recovered = 0;
    unsigned long long ranges_rejected = 0;
    unsigned long long max_run = 0;
};

std::unordered_map<GLuint, bool> g_shader_eligible;
std::unordered_map<GLuint, std::unordered_set<GLuint>> g_program_shaders;
std::unordered_map<GLuint, program_state> g_programs;
thread_local pending_batch g_pending;
thread_local batch_stats g_stats;
std::atomic<unsigned long long> g_native_batches{0};
std::atomic<unsigned long long> g_native_runs{0};
std::atomic<unsigned long long> g_draws_saved{0};
std::atomic<unsigned long long> g_backend_fallback_runs{0};
std::atomic<unsigned long long> g_compact_batches{0};
std::atomic<unsigned long long> g_compact_runs{0};
std::atomic<unsigned long long> g_compact_draws_saved{0};
std::atomic<unsigned long long> g_compact_fallback_runs{0};

enum class backend_state : uint8_t { unknown, working, unavailable };
backend_state g_backend_state = backend_state::unknown; // worker-thread only
mg_pz_tile_batch_multidraw_fn g_backend_multidraw = nullptr;
std::atomic<backend_state> g_client_index_state{backend_state::unknown};

#if defined(MOBILEGLUES_TESTING)
mg_pz_tile_batch_multidraw_fn g_test_multidraw = nullptr;
#endif

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

bool backend_has_extension(const char* name) {
#if defined(MOBILEGLUES_TESTING)
    (void)name;
    return g_test_multidraw != nullptr;
#else
    if (GLES.glGetStringi == nullptr || GLES.glGetIntegerv == nullptr) return false;
    GLint count = 0;
    GLES.glGetIntegerv(GL_NUM_EXTENSIONS, &count);
    for (GLint index = 0; index < count; ++index) {
        const GLubyte* extension = GLES.glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(index));
        if (extension != nullptr && std::strcmp(reinterpret_cast<const char*>(extension), name) == 0) return true;
    }
    return false;
#endif
}

mg_pz_tile_batch_multidraw_fn resolve_backend_multidraw() {
    if (g_backend_state == backend_state::working) return g_backend_multidraw;
    if (g_backend_state == backend_state::unavailable) return nullptr;

#if defined(MOBILEGLUES_TESTING)
    g_backend_multidraw = g_test_multidraw;
#else
    const bool ext = backend_has_extension("GL_EXT_multi_draw_arrays");
    const bool angle = backend_has_extension("GL_ANGLE_multi_draw");
    if (ext || angle) {
        const char* name = ext ? "glMultiDrawElementsEXT" : "glMultiDrawElementsANGLE";
        if (gles != nullptr)
            g_backend_multidraw = reinterpret_cast<mg_pz_tile_batch_multidraw_fn>(proc_address(gles, name));
        if (g_backend_multidraw == nullptr && egl != nullptr) {
            auto get_proc = reinterpret_cast<eglGetProcAddress_PTR>(proc_address(egl, "eglGetProcAddress"));
            if (get_proc != nullptr)
                g_backend_multidraw = reinterpret_cast<mg_pz_tile_batch_multidraw_fn>(get_proc(name));
        }
    }
#endif

    if (g_backend_multidraw == nullptr) {
        g_backend_state = backend_state::unavailable;
        LOG_I("ZOMDROID_PZ_STATE_RUN_COMPILER backend=multi_draw_arrays available=0 fallback=ordered_singles")
        return nullptr;
    }
    // The first real call is still probed. Android dispatch libraries can
    // return a non-null extension trampoline even when the active driver does
    // not implement the extension.
    return g_backend_multidraw;
}

void drain_backend_errors() {
    if (GLES.glGetError == nullptr) return;
    for (int index = 0; index < 16 && GLES.glGetError() != GL_NO_ERROR; ++index) {
    }
}

bool call_backend_multidraw(GLenum mode, const GLsizei* counts, GLenum type,
                            const void* const* offsets, GLsizei run_count) {
    mg_pz_tile_batch_multidraw_fn function = resolve_backend_multidraw();
    if (function == nullptr) return false;
    const bool probing = g_backend_state == backend_state::unknown;
    if (probing) drain_backend_errors();
    function(mode, counts, type, offsets, run_count);
    if (probing) {
        const GLenum error = GLES.glGetError != nullptr ? GLES.glGetError() : GL_NO_ERROR;
        if (error != GL_NO_ERROR) {
            g_backend_state = backend_state::unavailable;
            g_backend_multidraw = nullptr;
            LOG_W_FORCE("ZOMDROID_PZ_STATE_RUN_COMPILER backend=multi_draw_arrays available=0 error=0x%04x "
                        "fallback=ordered_singles", error)
            return false;
        }
        g_backend_state = backend_state::working;
        LOG_I("ZOMDROID_PZ_STATE_RUN_COMPILER backend=multi_draw_arrays available=1")
    }
    return true;
}

bool exact_index_range(GLuint frontend_buffer, uint32_t offset, GLsizei count,
                       GLuint* minimum, GLuint* maximum) {
    if (frontend_buffer == 0 || count <= 0 || minimum == nullptr || maximum == nullptr) return false;
    uint64_t lifetime = 0;
    uint64_t version = 0;
    GLsizeiptr data_size = 0;
    if (!mg_pz_buffer_cache_identity(frontend_buffer, &lifetime, &version, &data_size)) return false;
    const void* source = mg_pz_buffer_cache_source(frontend_buffer, lifetime, version, data_size);
    const uint64_t bytes = static_cast<uint64_t>(count) * sizeof(GLushort);
    if (source == nullptr || static_cast<uint64_t>(offset) > static_cast<uint64_t>(data_size) ||
        bytes > static_cast<uint64_t>(data_size) - static_cast<uint64_t>(offset))
        return false;

    const auto* indices = static_cast<const unsigned char*>(source) + offset;
    GLuint low = std::numeric_limits<GLuint>::max();
    GLuint high = 0;
    for (GLsizei index = 0; index < count; ++index) {
        GLushort value = 0;
        std::memcpy(&value, indices + static_cast<size_t>(index) * sizeof(value), sizeof(value));
        low = value < low ? value : low;
        high = value > high ? value : high;
    }
    *minimum = low;
    *maximum = high;
    return true;
}

bool call_client_index_draw(GLenum mode, GLsizei count, GLenum type, const void* indices,
                            GLuint original_element_buffer) {
    backend_state state = g_client_index_state.load(std::memory_order_acquire);
    if (state == backend_state::unavailable) return false;
    const bool probing = state == backend_state::unknown;
    if (probing) drain_backend_errors();

    // GLES explicitly accepts a client pointer when ELEMENT_ARRAY_BUFFER is
    // zero. The command owns these bytes until this backend call returns.
    GLES.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    GLES.glDrawElements(mode, count, type, indices);
    GLES.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, original_element_buffer);

    if (probing) {
        const GLenum error = GLES.glGetError != nullptr ? GLES.glGetError() : GL_NO_ERROR;
        if (error != GL_NO_ERROR) {
            g_client_index_state.store(backend_state::unavailable, std::memory_order_release);
            LOG_W_FORCE("ZOMDROID_PZ_STATE_RUN_COMPILER backend=client_indices available=0 error=0x%04x "
                        "fallback=ordered_singles", error)
            return false;
        }
        g_client_index_state.store(backend_state::working, std::memory_order_release);
        LOG_I("ZOMDROID_PZ_STATE_RUN_COMPILER backend=client_indices available=1")
    }
    return true;
}

void upload_original_depth(const program_state& program, GLfloat z, GLfloat chunk,
                           bool upload_z, bool upload_chunk) {
    if (upload_z) GLES.glUniform1f(program.z_depth, z);
    if (upload_chunk) GLES.glUniform1f(program.chunk_depth, chunk);
}

void upload_original_depth(GLint z_location, GLint chunk_location, GLfloat z, GLfloat chunk,
                           bool upload_z, bool upload_chunk) {
    if (upload_z) GLES.glUniform1f(z_location, z);
    if (upload_chunk) GLES.glUniform1f(chunk_location, chunk);
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

struct state_run_command {
    GLint z_depth;
    GLint chunk_depth;
    GLint run_count_location;
    GLint run_start_location;
    GLint run_depth_location;
    GLenum mode;
    GLenum type;
    uint32_t count;
    GLfloat current_z;
    GLfloat current_chunk;
    std::array<draw_run, kMaxRuns> runs;

    state_run_command(const program_state& program, const pending_batch& pending)
        : z_depth(program.z_depth), chunk_depth(program.chunk_depth), run_count_location(program.run_count),
          run_start_location(program.run_start), run_depth_location(program.run_depth), mode(pending.mode),
          type(pending.type), count(static_cast<uint32_t>(pending.count)), current_z(pending.current_z),
          current_chunk(pending.current_chunk), runs(pending.runs) {}

    static void execute(void* storage) {
        auto* command = static_cast<state_run_command*>(storage);
        std::array<GLsizei, kMaxRuns> counts{};
        std::array<const void*, kMaxRuns> offsets{};
        std::array<GLint, kMaxRuns> starts{};
        std::array<GLfloat, kMaxRuns * 2> depths{};
        for (uint32_t index = 0; index < command->count; ++index) {
            const draw_run& run = command->runs[index];
            counts[index] = run.count;
            offsets[index] = reinterpret_cast<const void*>(static_cast<uintptr_t>(run.offset));
            starts[index] = static_cast<GLint>(run.start);
            depths[index * 2] = run.z;
            depths[index * 2 + 1] = run.chunk;
        }

        GLES.glUniform1i(command->run_count_location, static_cast<GLint>(command->count));
        GLES.glUniform1iv(command->run_start_location, static_cast<GLsizei>(command->count), starts.data());
        GLES.glUniform2fv(command->run_depth_location, static_cast<GLsizei>(command->count), depths.data());

        if (call_backend_multidraw(command->mode, counts.data(), command->type, offsets.data(),
                                   static_cast<GLsizei>(command->count))) {
            if (mg_pz_census_active) {
                g_native_batches.fetch_add(1, std::memory_order_relaxed);
                g_native_runs.fetch_add(command->count, std::memory_order_relaxed);
                g_draws_saved.fetch_add(command->count - 1, std::memory_order_relaxed);
            }
        } else {
            // Disable the rewritten shader path before replaying the exact
            // original sequence. This is also the normal path on a driver
            // without EXT/ANGLE_multi_draw.
            GLES.glUniform1i(command->run_count_location, 0);
            bool z_known = false;
            bool chunk_known = false;
            GLfloat driver_z = 0.0f;
            GLfloat driver_chunk = 0.0f;
            for (uint32_t index = 0; index < command->count; ++index) {
                const draw_run& run = command->runs[index];
                upload_original_depth(command->z_depth, command->chunk_depth, run.z, run.chunk,
                                      !z_known || !same_bits(driver_z, run.z),
                                      !chunk_known || !same_bits(driver_chunk, run.chunk));
                GLES.glDrawRangeElements(command->mode, run.start, run.end, run.count, command->type,
                                         reinterpret_cast<const void*>(static_cast<uintptr_t>(run.offset)));
                driver_z = run.z;
                driver_chunk = run.chunk;
                z_known = true;
                chunk_known = true;
            }
            if (mg_pz_census_active)
                g_backend_fallback_runs.fetch_add(command->count, std::memory_order_relaxed);
        }

        // Commands after this compiled group must observe the same uniform
        // state they would have seen after the original StateRun sequence.
        GLES.glUniform1i(command->run_count_location, 0);
        upload_original_depth(command->z_depth, command->chunk_depth,
                              command->current_z, command->current_chunk, true, true);
    }

    static void destroy(void* storage) { static_cast<state_run_command*>(storage)->~state_run_command(); }
};

static_assert(sizeof(state_run_command) <= mg_ts::kCommandPayloadBytes,
              "PZ StateRun command must remain inline in the packet queue");

struct compacted_state_run_command {
    GLint z_depth;
    GLint chunk_depth;
    GLint run_count_location;
    GLint run_start_location;
    GLint run_depth_location;
    GLenum mode;
    GLenum type;
    uint32_t run_count;
    GLsizei index_count;
    GLuint original_element_buffer;
    GLfloat current_z;
    GLfloat current_chunk;
    std::array<draw_run, kMaxRuns> runs;

    compacted_state_run_command(const program_state& program, const pending_batch& pending,
                                const std::array<draw_run, kMaxRuns>& exact_runs,
                                GLsizei total_indices)
        : z_depth(program.z_depth), chunk_depth(program.chunk_depth), run_count_location(program.run_count),
          run_start_location(program.run_start), run_depth_location(program.run_depth), mode(pending.mode),
          type(pending.type), run_count(static_cast<uint32_t>(pending.count)), index_count(total_indices),
          original_element_buffer(pending.element_buffer), current_z(pending.current_z),
          current_chunk(pending.current_chunk), runs(exact_runs) {}

    static void execute(void* storage) {
        auto* command = static_cast<compacted_state_run_command*>(storage);
        std::array<GLint, kMaxRuns> starts{};
        std::array<GLfloat, kMaxRuns * 2> depths{};
        for (uint32_t index = 0; index < command->run_count; ++index) {
            const draw_run& run = command->runs[index];
            starts[index] = static_cast<GLint>(run.start);
            depths[index * 2] = run.z;
            depths[index * 2 + 1] = run.chunk;
        }

        GLES.glUniform1i(command->run_count_location, static_cast<GLint>(command->run_count));
        GLES.glUniform1iv(command->run_start_location, static_cast<GLsizei>(command->run_count), starts.data());
        GLES.glUniform2fv(command->run_depth_location, static_cast<GLsizei>(command->run_count), depths.data());

        const void* indices = reinterpret_cast<const unsigned char*>(storage) + sizeof(*command);
        if (call_client_index_draw(command->mode, command->index_count, command->type, indices,
                                   command->original_element_buffer)) {
            if (mg_pz_census_active) {
                g_compact_batches.fetch_add(1, std::memory_order_relaxed);
                g_compact_runs.fetch_add(command->run_count, std::memory_order_relaxed);
                g_compact_draws_saved.fetch_add(command->run_count - 1, std::memory_order_relaxed);
            }
        } else {
            GLES.glUniform1i(command->run_count_location, 0);
            bool z_known = false;
            bool chunk_known = false;
            GLfloat driver_z = 0.0f;
            GLfloat driver_chunk = 0.0f;
            for (uint32_t index = 0; index < command->run_count; ++index) {
                const draw_run& run = command->runs[index];
                upload_original_depth(command->z_depth, command->chunk_depth, run.z, run.chunk,
                                      !z_known || !same_bits(driver_z, run.z),
                                      !chunk_known || !same_bits(driver_chunk, run.chunk));
                GLES.glDrawRangeElements(command->mode, run.start, run.end, run.count, command->type,
                                         reinterpret_cast<const void*>(static_cast<uintptr_t>(run.offset)));
                driver_z = run.z;
                driver_chunk = run.chunk;
                z_known = true;
                chunk_known = true;
            }
            if (mg_pz_census_active)
                g_compact_fallback_runs.fetch_add(command->run_count, std::memory_order_relaxed);
        }

        GLES.glUniform1i(command->run_count_location, 0);
        upload_original_depth(command->z_depth, command->chunk_depth,
                              command->current_z, command->current_chunk, true, true);
    }

    static void destroy(void* storage) {
        static_cast<compacted_state_run_command*>(storage)->~compacted_state_run_command();
    }
};

static_assert(sizeof(compacted_state_run_command) < mg_ts::kMaximumCommandPayloadBytes,
              "PZ compacted command must leave room for client indices");

bool enqueue_compacted(const program_state& program) {
    if (g_client_index_state.load(std::memory_order_acquire) == backend_state::unavailable) return false;

    uint64_t lifetime = 0;
    uint64_t version = 0;
    GLsizeiptr data_size = 0;
    if (!mg_pz_buffer_cache_identity(g_pending.frontend_element_buffer, &lifetime, &version, &data_size))
        return false;
    const void* source = mg_pz_buffer_cache_source(g_pending.frontend_element_buffer, lifetime, version, data_size);
    if (source == nullptr || data_size <= 0) return false;

    size_t total_indices = 0;
    std::array<draw_run, kMaxRuns> exact_runs = g_pending.runs;
    GLuint previous_end = 0;
    bool previous_known = false;
    for (size_t run_index = 0; run_index < g_pending.count; ++run_index) {
        draw_run& run = exact_runs[run_index];
        if (run.count <= 0) return false;
        const size_t count = static_cast<size_t>(run.count);
        if (count > static_cast<size_t>(std::numeric_limits<GLsizei>::max()) - total_indices) return false;
        const uint64_t bytes = static_cast<uint64_t>(count) * sizeof(GLushort);
        if (static_cast<uint64_t>(run.offset) > static_cast<uint64_t>(data_size) ||
            bytes > static_cast<uint64_t>(data_size) - static_cast<uint64_t>(run.offset))
            return false;

        const auto* run_source = static_cast<const unsigned char*>(source) + run.offset;
        GLuint minimum = std::numeric_limits<GLuint>::max();
        GLuint maximum = 0;
        for (size_t index = 0; index < count; ++index) {
            GLushort value = 0;
            std::memcpy(&value, run_source + index * sizeof(value), sizeof(value));
            minimum = value < minimum ? value : minimum;
            maximum = value > maximum ? value : maximum;
        }
        if (previous_known && minimum <= previous_end) return false;
        run.start = minimum;
        run.end = maximum;
        previous_end = maximum;
        previous_known = true;
        total_indices += count;
    }

    const size_t index_bytes = total_indices * sizeof(GLushort);
    const size_t payload_bytes = sizeof(compacted_state_run_command) + index_bytes;
    if (total_indices > static_cast<size_t>(std::numeric_limits<GLsizei>::max()) ||
        payload_bytes > mg_ts::kMaximumCommandPayloadBytes)
        return false;

    const mg_ts::reservation slot =
        mg_ts::reserve(&compacted_state_run_command::execute, &compacted_state_run_command::destroy,
                       payload_bytes, alignof(compacted_state_run_command), mg_ts::command_kind::draw);
    if (slot.storage == nullptr) return false;
    new (slot.storage) compacted_state_run_command(program, g_pending, exact_runs,
                                                    static_cast<GLsizei>(total_indices));
    auto* destination = static_cast<unsigned char*>(slot.storage) + sizeof(compacted_state_run_command);
    for (size_t run_index = 0; run_index < g_pending.count; ++run_index) {
        const draw_run& run = exact_runs[run_index];
        const size_t bytes = static_cast<size_t>(run.count) * sizeof(GLushort);
        std::memcpy(destination, static_cast<const unsigned char*>(source) + run.offset, bytes);
        destination += bytes;
    }
    mg_ts::publish(slot.sequence);
    return true;
}

bool enqueue_combined(const program_state& program) {
    if (!enqueue_compacted(program)) {
        const uint64_t sequence =
            mg_ts::enqueueAs<state_run_command>(mg_ts::command_kind::draw, program, g_pending);
        if (sequence == 0) return false;
    }
    if (mg_pz_census_active) {
        ++g_stats.scheduled_batches;
        g_stats.scheduled_runs += g_pending.count;
        if (g_pending.count > g_stats.max_run) g_stats.max_run = g_pending.count;
    }
    return true;
}

void flush_pending() {
    if (g_pending.inside_flush) return;
    g_pending.inside_flush = true;

    program_state* program = current_program_state(g_pending.program);
    if (g_pending.count != 0 && program != nullptr) {
        if (g_pending.count < kMinimumCombinedRuns || !enqueue_combined(*program))
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
    g_pending.frontend_element_buffer = 0;
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
            "ZOMDROID_PZ_STATE_RUN_COMPILER_PROGRAM program=%u z=%d chunk=%d count=%d start=%d depth=%d ready=1",
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
    const GLuint frontend_element_buffer = find_bound_buffer_by_target(GL_ELEMENT_ARRAY_BUFFER);
    const GLuint element_buffer = mg_driver_bound_buffer(GL_ELEMENT_ARRAY_BUFFER);
    const uintptr_t offset = reinterpret_cast<uintptr_t>(indices);
    if (element_buffer == 0 || offset > static_cast<uintptr_t>(std::numeric_limits<uint32_t>::max())) return false;

    if (mg_pz_census_active) ++g_stats.candidates;
    GLuint resolved_start = start;
    GLuint resolved_end = end;
    if (g_pending.count != 0) {
        draw_run& previous = g_pending.runs[g_pending.count - 1];
        bool ordered_ranges = start > previous.end;
        if (!ordered_ranges && g_pending.frontend_element_buffer == frontend_element_buffer) {
            GLuint previous_start = 0;
            GLuint previous_end = 0;
            GLuint current_start = 0;
            GLuint current_end = 0;
            if (exact_index_range(frontend_element_buffer, previous.offset, previous.count,
                                  &previous_start, &previous_end) &&
                exact_index_range(frontend_element_buffer, static_cast<uint32_t>(offset), count,
                                  &current_start, &current_end) &&
                current_start > previous_end) {
                previous.start = previous_start;
                previous.end = previous_end;
                resolved_start = current_start;
                resolved_end = current_end;
                ordered_ranges = true;
                if (mg_pz_census_active) ++g_stats.ranges_recovered;
            } else if (mg_pz_census_active) {
                ++g_stats.ranges_rejected;
            }
        }
        const bool compatible = g_pending.program == program_name && g_pending.element_buffer == element_buffer &&
                                g_pending.frontend_element_buffer == frontend_element_buffer &&
                                g_pending.mode == mode && g_pending.type == type && ordered_ranges;
        if (!compatible || g_pending.count == kMaxRuns) {
            if (mg_pz_census_active && !compatible) ++g_stats.incompatible;
            flush_pending();
        }
    }

    if (g_pending.count == 0) {
        g_pending.program = program_name;
        g_pending.element_buffer = element_buffer;
        g_pending.frontend_element_buffer = frontend_element_buffer;
        g_pending.mode = mode;
        g_pending.type = type;
    }
    g_pending.runs[g_pending.count++] =
        {resolved_start, resolved_end, count, static_cast<uint32_t>(offset),
         g_pending.current_z, g_pending.current_chunk};
    return true;
}

void mg_pz_tile_batch_before_backend(const char* command) {
    // Dispatch slots are used on both sides of threaded submission. Only the
    // producer owns the compiler; backend calls made while executing a packet
    // must go straight to GLES and must not touch producer-side maps.
    if (!mg_pz_tile_batch_active || g_pending.inside_flush || !mg_ts::availableAndActive()) return;
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
    const unsigned long long compact_saved = g_compact_draws_saved.load(std::memory_order_relaxed);
    const unsigned long long native_saved = g_draws_saved.load(std::memory_order_relaxed);
    LOG_I("ZOMDROID_PZ_STATE_RUN_COMPILER frames=%llu candidates=%llu scheduled=%llu/%llu "
          "compact=%llu/%llu native=%llu/%llu saved=%llu fallback=%llu/%llu single=%llu incompatible=%llu "
          "range=%llu/%llu max_run=%llu",
          g_stats.frames, g_stats.candidates, g_stats.scheduled_batches, g_stats.scheduled_runs,
          g_compact_batches.load(std::memory_order_relaxed),
          g_compact_runs.load(std::memory_order_relaxed),
          g_native_batches.load(std::memory_order_relaxed),
          g_native_runs.load(std::memory_order_relaxed),
          compact_saved + native_saved,
          g_compact_fallback_runs.load(std::memory_order_relaxed),
          g_backend_fallback_runs.load(std::memory_order_relaxed),
          g_stats.single_fallbacks, g_stats.incompatible,
          g_stats.ranges_recovered, g_stats.ranges_rejected, g_stats.max_run)
}

#if defined(MOBILEGLUES_TESTING)
void mg_pz_tile_batch_test_backend(mg_pz_tile_batch_multidraw_fn function) {
    g_test_multidraw = function;
    g_backend_multidraw = nullptr;
    g_backend_state = backend_state::unknown;
}
#endif

#endif
