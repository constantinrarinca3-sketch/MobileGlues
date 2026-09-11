// MobileGlues - gl/pz_material_stream_renderer.cpp
// Project Zomboid experiment V4.6: keep V4.5 batching, but never material-stream
// the application's surface/default-FBO pass. The V4.5 run proved that program 1
// is also used by MainScreen/UI: batching was already active while MainScreenState
// was still running and the menu flickered. V4.6 therefore keeps UI/HUD ordered on
// the original draw path and allows collection only for explicit offscreen FBOs.

#include "pz_repack_renderer.h"
#include "threaded_submission.h"
#include "pz_census.h"
#include "framebuffer.h"
#include "mg.h"
#include "FSR1/FSR1.h"
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
// ::v44_base and ::v43_base. Embed it under v46_base while providing aliases for
// those absolute references, exactly as the previous wrappers did for their base.
namespace v46_base {
namespace v44_base {
namespace v43_base {}
}
namespace mg_ts {
using backend_command_class = ::mg_ts::backend_command_class;
}
}
namespace v44_base = v46_base::v44_base;
namespace v43_base = v46_base::v44_base::v43_base;

namespace v46_base {
#include "pz_material_stream_renderer_v44_base.cpp"
} // namespace v46_base

namespace v46_base {
namespace v44_base {
namespace v43_base {
namespace legacy_v41 {
namespace v41_base {

namespace {
thread_local unsigned long long g_v46_surface_draws = 0;
thread_local unsigned long long g_v46_surface_flushes = 0;
}

bool v46_base_ready() {
    return g_v44_enabled && g_v41_enabled && g_material_enabled && g_enabled;
}

// Preserve V4.1+ TOTAL telemetry semantics for guarded draws: count the draw as
// seen, route it through the existing ordered fallback, and attribute a hard flush
// if this draw was the first surface command after an offscreen collector.
void v46_surface_fallback_draw(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    ++g_v46_surface_draws;

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
        g_v46_surface_flushes += attributed;
        v44_histogram_add("glDrawElements:surface_guard", attributed);
    }

    if (pending && g_material.stats.backend_draws != backend_before) v44_restore_after_emit(snapshot);
    maybe_report_material_stream();
    v44_maybe_report();
}

void v46_report_surface_guard(bool final) {
    LOG_I("ZOMDROID_PZ_MATERIAL_STREAM_V46 final=%d surface_draws=%llu surface_flushes=%llu "
          "guard=default_or_fsr_surface capture_scope=explicit_offscreen_fbo",
          final ? 1 : 0, g_v46_surface_draws, g_v46_surface_flushes)
    if (final) {
        g_v46_surface_draws = 0;
        g_v46_surface_flushes = 0;
    }
}

} // namespace v41_base
} // namespace legacy_v41
} // namespace v43_base
} // namespace v44_base
} // namespace v46_base

namespace {
using v46_draw_ptr = glDrawElements_PTR;
v46_draw_ptr g_v46_material_draw = nullptr;
bool g_v46_enabled = false;

bool v46_is_surface_pass() {
    // gl_state->current_draw_fbo is the backend draw binding. With FSR1 enabled,
    // application framebuffer 0 is redirected to g_renderFBO, so treat that
    // internal FBO as the surface pass too. An explicit application FBO is the
    // only scope in which V4.6 permits Material-Stream capture.
    if (gl_state == nullptr) return true;
    const GLuint draw_fbo = gl_state->current_draw_fbo;
    if (draw_fbo == 0) return true;
    const GLuint fsr_surface = FSR1_Context::g_renderFBO;
    return fsr_surface != 0 && draw_fbo == fsr_surface;
}

void v46_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    if (g_v46_enabled && v46_is_surface_pass()) {
        v46_base::v44_base::v43_base::legacy_v41::v41_base::v46_surface_fallback_draw(mode, count, type, indices);
        return;
    }
    g_v46_material_draw(mode, count, type, indices);
}
} // namespace

namespace mg_ts {
void pz_repack_before_backend_command(backend_command_class classification) {
    if (g_v46_enabled && classification == backend_command_class::context_release)
        v46_base::v44_base::v43_base::legacy_v41::v41_base::v46_report_surface_guard(true);
    v46_base::mg_ts::pz_repack_before_backend_command(classification);
}

void pz_repack_note_backend_command(const char* name) {
    v46_base::mg_ts::pz_repack_note_backend_command(name);
}
} // namespace mg_ts

void mg_pz_repack_renderer_install(void) {
    v46_base::mg_pz_repack_renderer_install();
    if (!v46_base::v44_base::v43_base::legacy_v41::v41_base::v46_base_ready()) return;

    g_v46_material_draw = static_cast<v46_draw_ptr>(GLES.glDrawElements);
    if (!g_v46_material_draw) {
        LOG_W_FORCE("ZOMDROID_PZ_MATERIAL_STREAM_V46 disabled reason=draw_wrapper_missing")
        return;
    }

    GLES.glDrawElements = v46_glDrawElements;
    g_v46_enabled = true;
    LOG_I("ZOMDROID_PZ_MATERIAL_STREAM_V46 enabled=1 revision=4.6 ui_guard=default_or_fsr_surface "
          "capture_scope=explicit_offscreen_fbo draw_range=v45 texparam_relax=none correctness=first")
}

#endif // ZOMDROID_EXPERIMENTAL
