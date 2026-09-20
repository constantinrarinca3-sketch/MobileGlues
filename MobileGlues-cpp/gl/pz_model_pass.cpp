#include "pz_model_pass.h"

#include "log.h"
#include "pz_census.h"

#include <bit>

namespace {

using count_t = unsigned long long;
constexpr unsigned kReportFrames = 300;

struct pass_counts {
    count_t begins = 0;
    count_t ends = 0;
    count_t gl_calls = 0;
    count_t draws = 0;
    count_t items = 0;
    count_t large_uniforms = 0;
    count_t large_uniforms_skipped = 0;
    count_t large_uniform_bytes_saved = 0;
};

struct marker_state {
    mg_pz_model_pass current = mg_pz_model_pass::none;
    pass_counts frame[3]{};
    pass_counts window[3]{};
    count_t malformed_frame = 0;
    count_t malformed_window = 0;
    unsigned frames = 0;
};

thread_local marker_state g_marker;

size_t pass_index(mg_pz_model_pass pass) {
    if (pass == mg_pz_model_pass::transparent) return 1u;
    if (pass == mg_pz_model_pass::zombie) return 2u;
    return 0u;
}

void add(pass_counts& out, const pass_counts& in) {
    out.begins += in.begins;
    out.ends += in.ends;
    out.gl_calls += in.gl_calls;
    out.draws += in.draws;
    out.items += in.items;
    out.large_uniforms += in.large_uniforms;
    out.large_uniforms_skipped += in.large_uniforms_skipped;
    out.large_uniform_bytes_saved += in.large_uniform_bytes_saved;
}

bool begin(mg_pz_model_pass pass) {
    if (g_marker.current != mg_pz_model_pass::none && mg_pz_census_active) ++g_marker.malformed_frame;
    g_marker.current = pass;
    if (mg_pz_census_active) ++g_marker.frame[pass_index(pass)].begins;
    return true;
}

bool end(mg_pz_model_pass pass) {
    if (g_marker.current != pass) {
        if (mg_pz_census_active) ++g_marker.malformed_frame;
    } else if (mg_pz_census_active) {
        ++g_marker.frame[pass_index(pass)].ends;
    }
    // Recover at every reserved end marker so a failed pass cannot classify
    // unrelated work for the rest of the frame.
    g_marker.current = mg_pz_model_pass::none;
    return true;
}

} // namespace

bool mg_pz_model_pass_handle_marker(GLenum source, GLenum type, GLuint id) {
    if (source != MG_PZ_MARKER_SOURCE_APPLICATION || type != MG_PZ_MARKER_TYPE) return false;
    switch (id) {
    case MG_PZ_MARKER_OPAQUE_BEGIN:
        return begin(mg_pz_model_pass::opaque);
    case MG_PZ_MARKER_OPAQUE_END:
        return end(mg_pz_model_pass::opaque);
    case MG_PZ_MARKER_TRANSPARENT_BEGIN:
        return begin(mg_pz_model_pass::transparent);
    case MG_PZ_MARKER_TRANSPARENT_END:
        return end(mg_pz_model_pass::transparent);
    case MG_PZ_MARKER_ZOMBIE_BEGIN:
        return begin(mg_pz_model_pass::zombie);
    case MG_PZ_MARKER_ZOMBIE_END:
        return end(mg_pz_model_pass::zombie);
    default:
        return false;
    }
}

bool mg_pz_model_pass_handle_uniform_marker(GLint location, GLfloat value) {
    if (location != MG_PZ_MARKER_UNIFORM_LOCATION) return false;
    const GLuint id = std::bit_cast<GLuint>(value);
    switch (id) {
    case MG_PZ_MARKER_OPAQUE_BEGIN:
        return begin(mg_pz_model_pass::opaque);
    case MG_PZ_MARKER_OPAQUE_END:
        return end(mg_pz_model_pass::opaque);
    case MG_PZ_MARKER_TRANSPARENT_BEGIN:
        return begin(mg_pz_model_pass::transparent);
    case MG_PZ_MARKER_TRANSPARENT_END:
        return end(mg_pz_model_pass::transparent);
    case MG_PZ_MARKER_ZOMBIE_BEGIN:
        return begin(mg_pz_model_pass::zombie);
    case MG_PZ_MARKER_ZOMBIE_END:
        return end(mg_pz_model_pass::zombie);
    default:
        return false;
    }
}

mg_pz_model_pass mg_pz_model_pass_current() {
    return g_marker.current;
}

void mg_pz_model_pass_gl_call() {
    if (!mg_pz_census_active || g_marker.current == mg_pz_model_pass::none) return;
    ++g_marker.frame[pass_index(g_marker.current)].gl_calls;
}

void mg_pz_model_pass_draw(GLsizei count, GLsizei instances) {
    if (!mg_pz_census_active || g_marker.current == mg_pz_model_pass::none) return;
    pass_counts& out = g_marker.frame[pass_index(g_marker.current)];
    ++out.draws;
    if (count > 0 && instances > 0) {
        out.items += static_cast<count_t>(count) * static_cast<count_t>(instances);
    }
}

void mg_pz_model_pass_large_uniform(size_t bytes, bool skipped) {
    if (!mg_pz_census_active || g_marker.current != mg_pz_model_pass::zombie) return;
    pass_counts& out = g_marker.frame[pass_index(g_marker.current)];
    ++out.large_uniforms;
    if (skipped) {
        ++out.large_uniforms_skipped;
        out.large_uniform_bytes_saved += static_cast<count_t>(bytes);
    }
}

void mg_pz_model_pass_present() {
    if (!mg_pz_census_active) return;
    if (g_marker.current != mg_pz_model_pass::none) {
        ++g_marker.malformed_frame;
        g_marker.current = mg_pz_model_pass::none;
    }
    for (size_t i = 0; i < 3; ++i) {
        add(g_marker.window[i], g_marker.frame[i]);
        g_marker.frame[i] = {};
    }
    g_marker.malformed_window += g_marker.malformed_frame;
    g_marker.malformed_frame = 0;
    ++g_marker.frames;
    if (g_marker.frames < kReportFrames) return;

    const pass_counts& opaque = g_marker.window[0];
    const pass_counts& transparent = g_marker.window[1];
    const pass_counts& zombie = g_marker.window[2];
    if (opaque.begins != 0 || transparent.begins != 0 || zombie.begins != 0 || g_marker.malformed_window != 0) {
        LOG_I("ZOMDROID_PZ_MODEL_PASS frames=%u opaque=%llu/%llu/%llu/%llu/%llu "
              "transparent=%llu/%llu/%llu/%llu/%llu zombie=%llu/%llu/%llu/%llu/%llu "
              "zombie_uniform=%llu/%llu/%lluB malformed=%llu",
              g_marker.frames, opaque.begins, opaque.ends, opaque.gl_calls, opaque.draws, opaque.items,
              transparent.begins, transparent.ends, transparent.gl_calls, transparent.draws, transparent.items,
              zombie.begins, zombie.ends, zombie.gl_calls, zombie.draws, zombie.items,
              zombie.large_uniforms, zombie.large_uniforms_skipped, zombie.large_uniform_bytes_saved,
              g_marker.malformed_window)
    }
    g_marker.window[0] = {};
    g_marker.window[1] = {};
    g_marker.window[2] = {};
    g_marker.malformed_window = 0;
    g_marker.frames = 0;
}

void mg_pz_model_pass_reset() {
    g_marker = {};
}
