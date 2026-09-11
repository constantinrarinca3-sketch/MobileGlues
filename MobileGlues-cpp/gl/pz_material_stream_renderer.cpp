// MobileGlues - gl/pz_material_stream_renderer.cpp
// Project Zomboid experiment V4.7: retain V4.5 material-stream batching on the
// surface, but exclude the UI by PZ's own per-sprite depth semantics instead of
// by framebuffer. PZ 42.20.3 TextureDraw resets z/chunkDepth to exact zero for a
// normal 2D draw, while ISO/world sprites explicitly populate nextZ and/or
// nextChunkDepth from IsoDepthHelper before the draw is created. V4.6 proved an
// FBO guard is too broad because the world itself renders mostly to the surface.

#include "pz_repack_renderer.h"
#include "threaded_submission.h"
#include "pz_census.h"
#include "../gles/loader.h"
#include "log.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <regex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#if !defined(ZOMDROID_EXPERIMENTAL)
#include "pz_material_stream_renderer_v44_base.cpp"
#else

// V4.4 already nests V4.3 and contains intentional absolute references to
// ::v44_base and ::v43_base. Embed it under v47_base while preserving those
// names as aliases; the retained implementation stays byte-for-byte unchanged.
namespace v47_base {
namespace v44_base {
namespace v43_base {}
}
namespace mg_ts {
using backend_command_class = ::mg_ts::backend_command_class;
}
}
namespace v44_base = v47_base::v44_base;
namespace v43_base = v47_base::v44_base::v43_base;

namespace v47_base {
#include "pz_material_stream_renderer_v44_base.cpp"
} // namespace v47_base

namespace v47_base {
namespace v44_base {
namespace v43_base {
namespace legacy_v41 {
namespace v41_base {

namespace {
thread_local unsigned long long g_v47_ui_zero_depth_draws = 0;
thread_local unsigned long long g_v47_ui_zero_depth_flushes = 0;
thread_local unsigned long long g_v47_world_depth_draws = 0;
}

bool v47_base_ready() {
    return g_v44_enabled && g_v41_enabled && g_material_enabled && g_enabled;
}

// PZ 42.20.3 DefaultShader receives TextureDraw.z and TextureDraw.chunkDepth on
// the render thread. TextureDraw defaults/resets both to exact 0.0f; ISO sprites
// explicitly fill one or both from IsoDepthHelper. Require both uniform locations
// to exist so a different compatible shader cannot be mislabeled as UI merely
// because an optional uniform is absent and its cached value defaulted to zero.
bool v47_is_zero_depth_ui_draw() {
    material_program_t* program = v42_ensure_program(g_state.program);
    if (!program || !program->compatible || program->z_depth < 0 || program->chunk_depth < 0) return false;
    const bool zero_depth = program->z_depth_value == 0.0f && program->chunk_depth_value == 0.0f;
    if (!zero_depth) ++g_v47_world_depth_draws;
    return zero_depth;
}

// Keep TOTAL draw telemetry and ordering for guarded UI draws. If a world
// collector is pending at the world->UI boundary, fallback_draw() closes it before
// issuing the UI draw; V4.4 then repairs application-visible state after emit.
void v47_ui_fallback_draw(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    ++g_v47_ui_zero_depth_draws;

    const bool pending = v44_collector_pending();
    const v44_restore_snapshot_t snapshot = pending ? v44_snapshot_shadow() : v44_restore_snapshot_t{};
    const unsigned long long hard_before = g_material.stats.hard_segments;
    const unsigned long long fallback_before = g_material.stats.fallback_draws;
    const unsigned long long backend_before = g_material.stats.backend_draws;

    ++g_material.stats.draws_seen;
    fallback_draw(mode, count, type, indices);

    const unsigned long long hard_delta = g_material.stats.hard_segments - hard_before;
    const unsigned long long fallback_delta = g_material.stats.fallback_draws - fallback_before;
    if (hard_delta != 0 && fallback_delta != 0) {
        const unsigned long long attributed = std::min(hard_delta, fallback_delta);
        g_v44_stats.fallback_flushes += attributed;
        g_v47_ui_zero_depth_flushes += attributed;
        v44_histogram_add("glDrawElements:ui_zero_depth", attributed);
    }

    if (pending && g_material.stats.backend_draws != backend_before) v44_restore_after_emit(snapshot);
    maybe_report_material_stream();
    v44_maybe_report();
}

void v47_report_semantic_guard(bool final) {
    LOG_I("ZOMDROID_PZ_MATERIAL_STREAM_V47 final=%d ui_zero_depth_draws=%llu ui_flushes=%llu "
          "world_depth_draws=%llu guard=z0_and_chunk0 capture_scope=surface+offscreen",
          final ? 1 : 0, g_v47_ui_zero_depth_draws, g_v47_ui_zero_depth_flushes, g_v47_world_depth_draws)
    if (final) {
        g_v47_ui_zero_depth_draws = 0;
        g_v47_ui_zero_depth_flushes = 0;
        g_v47_world_depth_draws = 0;
    }
}

} // namespace v41_base
} // namespace legacy_v41
} // namespace v43_base
} // namespace v44_base
} // namespace v47_base

namespace {
using v47_draw_ptr = glDrawElements_PTR;
v47_draw_ptr g_v47_material_draw = nullptr;
bool g_v47_enabled = false;

void v47_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    if (g_v47_enabled &&
        v47_base::v44_base::v43_base::legacy_v41::v41_base::v47_is_zero_depth_ui_draw()) {
        v47_base::v44_base::v43_base::legacy_v41::v41_base::v47_ui_fallback_draw(mode, count, type, indices);
        return;
    }
    g_v47_material_draw(mode, count, type, indices);
}
} // namespace

namespace mg_ts {
void pz_repack_before_backend_command(backend_command_class classification) {
    if (g_v47_enabled && classification == backend_command_class::context_release)
        v47_base::v44_base::v43_base::legacy_v41::v41_base::v47_report_semantic_guard(true);
    v47_base::mg_ts::pz_repack_before_backend_command(classification);
}

void pz_repack_note_backend_command(const char* name) {
    v47_base::mg_ts::pz_repack_note_backend_command(name);
}
} // namespace mg_ts

void mg_pz_repack_renderer_install(void) {
    v47_base::mg_pz_repack_renderer_install();
    if (!v47_base::v44_base::v43_base::legacy_v41::v41_base::v47_base_ready()) return;

    g_v47_material_draw = static_cast<v47_draw_ptr>(GLES.glDrawElements);
    if (!g_v47_material_draw) {
        LOG_W_FORCE("ZOMDROID_PZ_MATERIAL_STREAM_V47 disabled reason=draw_wrapper_missing")
        return;
    }

    GLES.glDrawElements = v47_glDrawElements;
    g_v47_enabled = true;
    LOG_I("ZOMDROID_PZ_MATERIAL_STREAM_V47 enabled=1 revision=4.7 ui_guard=z0_and_chunk0 "
          "capture_scope=surface+offscreen draw_range=v45 texparam_relax=none correctness=semantic_depth")
}

#endif // ZOMDROID_EXPERIMENTAL
