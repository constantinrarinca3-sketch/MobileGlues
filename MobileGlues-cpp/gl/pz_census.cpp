// MobileGlues - gl/pz_census.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1.

#include "pz_census.h"

#include "log.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>

bool mg_pz_census_active = false;

namespace {

constexpr uint32_t kReportFrames = 300;
using count_t = unsigned long long;

struct counters_t {
    count_t draws_arrays = 0;
    count_t draws_elements = 0;
    count_t multidraw_calls = 0;
    count_t draw_commands = 0;
    count_t draw_items = 0;
    count_t triangles = 0;
    count_t quads = 0;
    count_t other_modes = 0;

    count_t use_program = 0;
    count_t use_program_redundant = 0;
    count_t bind_texture = 0;
    count_t bind_texture_redundant = 0;
    count_t active_texture = 0;
    count_t active_texture_redundant = 0;
    count_t bind_buffer = 0;
    count_t bind_buffer_same = 0;
    count_t bind_vao = 0;
    count_t bind_vao_same = 0;
    count_t bind_framebuffer = 0;
    count_t bind_framebuffer_same = 0;
    count_t enable_disable = 0;
    count_t enable_disable_redundant = 0;

    count_t uniform_calls = 0;
    count_t vertex_attrib_calls = 0;
    count_t fixed_state_calls = 0;
    count_t query_calls = 0;
    count_t sync_calls = 0;
    count_t buffer_data_calls = 0;
    count_t buffer_sub_data_calls = 0;
    count_t buffer_upload_bytes = 0;
    count_t buffer_map_calls = 0;
    count_t buffer_map_bytes = 0;
};

counters_t& operator+=(counters_t& out, const counters_t& in) {
#define MG_ADD_FIELD(field) out.field += in.field
    MG_ADD_FIELD(draws_arrays);
    MG_ADD_FIELD(draws_elements);
    MG_ADD_FIELD(multidraw_calls);
    MG_ADD_FIELD(draw_commands);
    MG_ADD_FIELD(draw_items);
    MG_ADD_FIELD(triangles);
    MG_ADD_FIELD(quads);
    MG_ADD_FIELD(other_modes);
    MG_ADD_FIELD(use_program);
    MG_ADD_FIELD(use_program_redundant);
    MG_ADD_FIELD(bind_texture);
    MG_ADD_FIELD(bind_texture_redundant);
    MG_ADD_FIELD(active_texture);
    MG_ADD_FIELD(active_texture_redundant);
    MG_ADD_FIELD(bind_buffer);
    MG_ADD_FIELD(bind_buffer_same);
    MG_ADD_FIELD(bind_vao);
    MG_ADD_FIELD(bind_vao_same);
    MG_ADD_FIELD(bind_framebuffer);
    MG_ADD_FIELD(bind_framebuffer_same);
    MG_ADD_FIELD(enable_disable);
    MG_ADD_FIELD(enable_disable_redundant);
    MG_ADD_FIELD(uniform_calls);
    MG_ADD_FIELD(vertex_attrib_calls);
    MG_ADD_FIELD(fixed_state_calls);
    MG_ADD_FIELD(query_calls);
    MG_ADD_FIELD(sync_calls);
    MG_ADD_FIELD(buffer_data_calls);
    MG_ADD_FIELD(buffer_sub_data_calls);
    MG_ADD_FIELD(buffer_upload_bytes);
    MG_ADD_FIELD(buffer_map_calls);
    MG_ADD_FIELD(buffer_map_bytes);
#undef MG_ADD_FIELD
    return out;
}

struct census_state_t {
    counters_t frame;
    counters_t window;
    counters_t worst_frame;
    uint64_t previous_present_ns = 0;
    uint64_t elapsed_ns = 0;
    uint64_t worst_ns = 0;
    uint32_t frames = 0;
    uint32_t timed_frames = 0;
    uint32_t over_20_ms = 0;
    uint32_t over_33_ms = 0;
    uint32_t over_50_ms = 0;
    uint32_t over_100_ms = 0;
    uint32_t failed_swaps = 0;
};

thread_local census_state_t g_census;

uint64_t monotonic_now_ns() {
    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return static_cast<uint64_t>(now.tv_sec) * 1000000000ULL + static_cast<uint64_t>(now.tv_nsec);
}

bool starts_with(const char* text, const char* prefix) {
    return std::strncmp(text, prefix, std::strlen(prefix)) == 0;
}

void reset_window(census_state_t& state) {
    state.window = {};
    state.worst_frame = {};
    state.elapsed_ns = 0;
    state.worst_ns = 0;
    state.frames = 0;
    state.timed_frames = 0;
    state.over_20_ms = 0;
    state.over_33_ms = 0;
    state.over_50_ms = 0;
    state.over_100_ms = 0;
    state.failed_swaps = 0;
}

void report(const census_state_t& state) {
    const double average_ms = state.timed_frames
                                  ? static_cast<double>(state.elapsed_ns) / 1000000.0 / state.timed_frames
                                  : 0.0;
    const double worst_ms = static_cast<double>(state.worst_ns) / 1000000.0;
    const counters_t& c = state.window;
    const counters_t& w = state.worst_frame;
    LOG_I("ZOMDROID_PZ_CENSUS schema=1 frames=%u avg_ms=%.3f max_ms=%.3f over20=%u over33=%u over50=%u "
          "over100=%u swap_fail=%u draw_a=%llu draw_e=%llu multidraw=%llu commands=%llu items=%llu "
          "mode_tri=%llu mode_quad=%llu mode_other=%llu program=%llu/%llu texture=%llu/%llu "
          "active_tex=%llu/%llu buffer_bind=%llu/%llu vao=%llu/%llu fbo=%llu/%llu enable=%llu/%llu "
          "uniform=%llu attrib=%llu state=%llu query=%llu sync=%llu upload=%llu+%llu/%lluB map=%llu/%lluB "
          "worst_draw=%llu worst_items=%llu worst_upload=%lluB",
          state.frames, average_ms, worst_ms, state.over_20_ms, state.over_33_ms, state.over_50_ms,
          state.over_100_ms, state.failed_swaps, c.draws_arrays, c.draws_elements, c.multidraw_calls,
          c.draw_commands, c.draw_items, c.triangles, c.quads, c.other_modes, c.use_program,
          c.use_program_redundant, c.bind_texture, c.bind_texture_redundant, c.active_texture,
          c.active_texture_redundant, c.bind_buffer, c.bind_buffer_same, c.bind_vao, c.bind_vao_same,
          c.bind_framebuffer, c.bind_framebuffer_same, c.enable_disable, c.enable_disable_redundant,
          c.uniform_calls, c.vertex_attrib_calls, c.fixed_state_calls, c.query_calls, c.sync_calls,
          c.buffer_data_calls, c.buffer_sub_data_calls, c.buffer_upload_bytes, c.buffer_map_calls,
          c.buffer_map_bytes, w.draw_commands, w.draw_items, w.buffer_upload_bytes)
}

} // namespace

void mg_pz_census_init(void) {
#if defined(ZOMDROID_EXPERIMENTAL)
    const char* value = std::getenv("MOBILEGLUES_PZ_CENSUS");
    mg_pz_census_active = value != nullptr && std::strcmp(value, "1") == 0;
    if (mg_pz_census_active) {
        LOG_I("ZOMDROID_PZ_CENSUS enabled=1 schema=1 interval_frames=%u", kReportFrames)
    }
#endif
}

void mg_pz_census_gl_call(const char* function) {
    if (!mg_pz_census_active || function == nullptr) return;
    counters_t& c = g_census.frame;
    if (starts_with(function, "glUniform") || starts_with(function, "glProgramUniform")) {
        ++c.uniform_calls;
    } else if (starts_with(function, "glVertexAttrib") || std::strcmp(function, "glEnableVertexAttribArray") == 0 ||
               std::strcmp(function, "glDisableVertexAttribArray") == 0) {
        ++c.vertex_attrib_calls;
    } else if (starts_with(function, "glBlend") || starts_with(function, "glDepth") ||
               starts_with(function, "glStencil") || std::strcmp(function, "glColorMask") == 0 ||
               std::strcmp(function, "glCullFace") == 0 || std::strcmp(function, "glFrontFace") == 0) {
        ++c.fixed_state_calls;
    } else if (starts_with(function, "glGet") || std::strcmp(function, "glCheckFramebufferStatus") == 0) {
        ++c.query_calls;
    } else if (std::strcmp(function, "glFinish") == 0 || std::strcmp(function, "glFlush") == 0 ||
               std::strcmp(function, "glFenceSync") == 0 || std::strcmp(function, "glClientWaitSync") == 0 ||
               std::strcmp(function, "glWaitSync") == 0 || std::strcmp(function, "glReadPixels") == 0) {
        ++c.sync_calls;
    }
}

void mg_pz_census_draw(bool indexed, GLenum mode, GLsizei count, GLsizei instances) {
    if (!mg_pz_census_active) return;
    counters_t& c = g_census.frame;
    if (indexed)
        ++c.draws_elements;
    else
        ++c.draws_arrays;
    ++c.draw_commands;
    const count_t safe_count = count > 0 ? static_cast<count_t>(count) : 0;
    const count_t safe_instances = instances > 0 ? static_cast<count_t>(instances) : 0;
    c.draw_items += safe_count * safe_instances;
    if (mode == GL_TRIANGLES)
        ++c.triangles;
    else if (mode == GL_QUADS)
        ++c.quads;
    else
        ++c.other_modes;
}

void mg_pz_census_multidraw(GLsizei commands) {
    if (!mg_pz_census_active) return;
    ++g_census.frame.multidraw_calls;
    if (commands > 0) g_census.frame.draw_commands += static_cast<count_t>(commands);
}

#define MG_PAIR_FUNCTION(name, total_field, redundant_field)                                                          \
    void name(bool redundant) {                                                                                        \
        if (!mg_pz_census_active) return;                                                                              \
        ++g_census.frame.total_field;                                                                                  \
        if (redundant) ++g_census.frame.redundant_field;                                                               \
    }

MG_PAIR_FUNCTION(mg_pz_census_use_program, use_program, use_program_redundant)
MG_PAIR_FUNCTION(mg_pz_census_bind_texture, bind_texture, bind_texture_redundant)
MG_PAIR_FUNCTION(mg_pz_census_active_texture, active_texture, active_texture_redundant)
MG_PAIR_FUNCTION(mg_pz_census_bind_buffer, bind_buffer, bind_buffer_same)
MG_PAIR_FUNCTION(mg_pz_census_bind_vao, bind_vao, bind_vao_same)
MG_PAIR_FUNCTION(mg_pz_census_bind_framebuffer, bind_framebuffer, bind_framebuffer_same)
MG_PAIR_FUNCTION(mg_pz_census_enable, enable_disable, enable_disable_redundant)

#undef MG_PAIR_FUNCTION

void mg_pz_census_buffer_data(GLsizeiptr bytes, bool sub_data) {
    if (!mg_pz_census_active) return;
    if (sub_data)
        ++g_census.frame.buffer_sub_data_calls;
    else
        ++g_census.frame.buffer_data_calls;
    if (bytes > 0) g_census.frame.buffer_upload_bytes += static_cast<count_t>(bytes);
}

void mg_pz_census_buffer_map(GLsizeiptr bytes) {
    if (!mg_pz_census_active) return;
    ++g_census.frame.buffer_map_calls;
    if (bytes > 0) g_census.frame.buffer_map_bytes += static_cast<count_t>(bytes);
}

void mg_pz_census_present(bool succeeded) {
    if (!mg_pz_census_active) return;
    census_state_t& state = g_census;
    const uint64_t now = monotonic_now_ns();
    uint64_t delta = 0;
    if (now != 0 && state.previous_present_ns != 0 && now >= state.previous_present_ns) {
        delta = now - state.previous_present_ns;
        state.elapsed_ns += delta;
        ++state.timed_frames;
        if (delta > 20000000ULL) ++state.over_20_ms;
        if (delta > 33333333ULL) ++state.over_33_ms;
        if (delta > 50000000ULL) ++state.over_50_ms;
        if (delta > 100000000ULL) ++state.over_100_ms;
        if (delta > state.worst_ns) {
            state.worst_ns = delta;
            state.worst_frame = state.frame;
        }
    }
    state.previous_present_ns = now;
    if (!succeeded) ++state.failed_swaps;
    state.window += state.frame;
    state.frame = {};
    ++state.frames;
    if (state.frames >= kReportFrames) {
        report(state);
        reset_window(state);
        // Keep the presentation timestamp: the next window must not discard its
        // first interval or hide a stall at the reporting boundary.
        state.previous_present_ns = now;
    }
}
