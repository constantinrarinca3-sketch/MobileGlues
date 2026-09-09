// MobileGlues - gl/pz_census.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1.

#include "pz_census.h"

#include "log.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <array>
#include <unordered_map>

bool mg_pz_census_active = false;
bool mg_pz_vao_fastpath_active = false;
bool mg_pz_attrib_fastpath_active = false;
bool mg_pz_uniform_fastpath_active = false;

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
    count_t bind_vao_driver_confirmed = 0;
    count_t bind_vao_skipped = 0;
    count_t bind_framebuffer = 0;
    count_t bind_framebuffer_same = 0;
    count_t enable_disable = 0;
    count_t enable_disable_redundant = 0;

    count_t uniform_calls = 0;
    count_t uniform_tracked = 0;
    count_t uniform_exact = 0;
    count_t uniform_skipped = 0;
    count_t vertex_attrib_calls = 0;
    count_t attrib_tracked = 0;
    count_t attrib_exact = 0;
    count_t attrib_skipped = 0;
    std::array<count_t, 7> attrib_kind_calls{};
    std::array<count_t, 7> attrib_kind_exact{};
    std::array<count_t, 7> attrib_kind_skipped{};
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
    MG_ADD_FIELD(bind_vao_driver_confirmed);
    MG_ADD_FIELD(bind_vao_skipped);
    MG_ADD_FIELD(bind_framebuffer);
    MG_ADD_FIELD(bind_framebuffer_same);
    MG_ADD_FIELD(enable_disable);
    MG_ADD_FIELD(enable_disable_redundant);
    MG_ADD_FIELD(uniform_calls);
    MG_ADD_FIELD(uniform_tracked);
    MG_ADD_FIELD(uniform_exact);
    MG_ADD_FIELD(uniform_skipped);
    MG_ADD_FIELD(vertex_attrib_calls);
    MG_ADD_FIELD(attrib_tracked);
    MG_ADD_FIELD(attrib_exact);
    MG_ADD_FIELD(attrib_skipped);
    for (size_t i = 0; i < out.attrib_kind_calls.size(); ++i) {
        out.attrib_kind_calls[i] += in.attrib_kind_calls[i];
        out.attrib_kind_exact[i] += in.attrib_kind_exact[i];
        out.attrib_kind_skipped[i] += in.attrib_kind_skipped[i];
    }
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

struct uniform_value_t {
    uint32_t signature = 0;
    uint16_t bytes = 0;
    std::array<unsigned char, 128> value{};
};

thread_local std::unordered_map<uint64_t, uniform_value_t> g_uniform_values;
thread_local std::unordered_map<GLuint, uniform_value_t> g_attrib_values;
thread_local unsigned long long g_uniform_context = 0;

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
    LOG_I("ZOMDROID_PZ_CENSUS schema=2 frames=%u avg_ms=%.3f max_ms=%.3f over20=%u over33=%u over50=%u "
          "over100=%u swap_fail=%u draw_a=%llu draw_e=%llu multidraw=%llu commands=%llu items=%llu "
          "mode_tri=%llu mode_quad=%llu mode_other=%llu program=%llu/%llu texture=%llu/%llu "
          "active_tex=%llu/%llu buffer_bind=%llu/%llu vao=%llu/%llu/%llu/%llu fbo=%llu/%llu enable=%llu/%llu "
          "uniform=%llu/%llu/%llu/%llu attrib=%llu/%llu/%llu/%llu "
          "attrib_enable=%llu/%llu/%llu attrib_pointer=%llu/%llu attrib_divisor=%llu/%llu "
          "attrib_format=%llu/%llu attrib_binding=%llu/%llu attrib_vbuffer=%llu/%llu attrib_constant=%llu/%llu "
          "state=%llu query=%llu sync=%llu upload=%llu+%llu/%lluB map=%llu/%lluB "
          "worst_draw=%llu worst_items=%llu worst_upload=%lluB",
          state.frames, average_ms, worst_ms, state.over_20_ms, state.over_33_ms, state.over_50_ms,
          state.over_100_ms, state.failed_swaps, c.draws_arrays, c.draws_elements, c.multidraw_calls,
          c.draw_commands, c.draw_items, c.triangles, c.quads, c.other_modes, c.use_program,
          c.use_program_redundant, c.bind_texture, c.bind_texture_redundant, c.active_texture,
          c.active_texture_redundant, c.bind_buffer, c.bind_buffer_same, c.bind_vao, c.bind_vao_same,
          c.bind_vao_driver_confirmed, c.bind_vao_skipped,
          c.bind_framebuffer, c.bind_framebuffer_same, c.enable_disable, c.enable_disable_redundant,
          c.uniform_calls, c.uniform_tracked, c.uniform_exact, c.uniform_skipped, c.vertex_attrib_calls,
          c.attrib_tracked, c.attrib_exact, c.attrib_skipped, c.attrib_kind_calls[0], c.attrib_kind_exact[0],
          c.attrib_kind_skipped[0],
          c.attrib_kind_calls[1], c.attrib_kind_exact[1],
          c.attrib_kind_calls[2], c.attrib_kind_exact[2], c.attrib_kind_calls[3], c.attrib_kind_exact[3],
          c.attrib_kind_calls[4], c.attrib_kind_exact[4], c.attrib_kind_calls[5], c.attrib_kind_exact[5],
          c.attrib_kind_calls[6], c.attrib_kind_exact[6], c.fixed_state_calls, c.query_calls, c.sync_calls,
          c.buffer_data_calls, c.buffer_sub_data_calls, c.buffer_upload_bytes, c.buffer_map_calls,
          c.buffer_map_bytes, w.draw_commands, w.draw_items, w.buffer_upload_bytes)
}

} // namespace

void mg_pz_census_init(void) {
#if defined(ZOMDROID_EXPERIMENTAL)
    const char* value = std::getenv("MOBILEGLUES_PZ_CENSUS");
    mg_pz_census_active = value != nullptr && std::strcmp(value, "1") == 0;
    const char* vao_value = std::getenv("MOBILEGLUES_PZ_VAO_FASTPATH");
    mg_pz_vao_fastpath_active = vao_value != nullptr && std::strcmp(vao_value, "1") == 0;
    const char* attrib_value = std::getenv("MOBILEGLUES_PZ_ATTRIB_FASTPATH");
    mg_pz_attrib_fastpath_active = attrib_value != nullptr && std::strcmp(attrib_value, "1") == 0;
    const char* uniform_value = std::getenv("MOBILEGLUES_PZ_UNIFORM_FASTPATH");
    mg_pz_uniform_fastpath_active = uniform_value != nullptr && std::strcmp(uniform_value, "1") == 0;
    g_census = {};
    g_uniform_values.clear();
    g_attrib_values.clear();
    g_uniform_context = 0;
    if (mg_pz_census_active) {
        LOG_I("ZOMDROID_PZ_CENSUS enabled=1 schema=2 interval_frames=%u", kReportFrames)
    }
    if (mg_pz_vao_fastpath_active) LOG_I("ZOMDROID_PZ_VAO_FASTPATH enabled=1")
    if (mg_pz_attrib_fastpath_active) LOG_I("ZOMDROID_PZ_ATTRIB_FASTPATH enabled=1")
    if (mg_pz_uniform_fastpath_active) LOG_I("ZOMDROID_PZ_UNIFORM_FASTPATH enabled=1")
#endif
}

void mg_pz_census_gl_call(const char* function) {
    if (!mg_pz_census_active || function == nullptr) return;
    counters_t& c = g_census.frame;
    if (starts_with(function, "glUniform") || starts_with(function, "glProgramUniform")) {
        ++c.uniform_calls;
    } else if (starts_with(function, "glVertexAttrib") || std::strcmp(function, "glEnableVertexAttribArray") == 0 ||
               std::strcmp(function, "glDisableVertexAttribArray") == 0 ||
               std::strcmp(function, "glBindVertexBuffer") == 0) {
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
MG_PAIR_FUNCTION(mg_pz_census_bind_framebuffer, bind_framebuffer, bind_framebuffer_same)
MG_PAIR_FUNCTION(mg_pz_census_enable, enable_disable, enable_disable_redundant)

#undef MG_PAIR_FUNCTION

void mg_pz_census_bind_vao(bool same_frontend_binding, bool driver_confirmed, bool skipped) {
    if (!mg_pz_census_active) return;
    ++g_census.frame.bind_vao;
    if (same_frontend_binding) ++g_census.frame.bind_vao_same;
    if (driver_confirmed) ++g_census.frame.bind_vao_driver_confirmed;
    if (skipped) ++g_census.frame.bind_vao_skipped;
}

bool mg_pz_uniform_call(GLuint program, GLint location, uint32_t signature, GLsizei count, const void* value,
                        size_t bytes) {
    if ((!mg_pz_census_active && !mg_pz_uniform_fastpath_active) || program == 0 || location < 0) return false;
    const uint64_t key = (static_cast<uint64_t>(program) << 32U) | static_cast<uint32_t>(location);
    if (count != 1 || value == nullptr || bytes == 0 || bytes > 128) {
        mg_pz_census_forget_program(program);
        return false;
    }
    const auto found = g_uniform_values.find(key);
    const bool exact = found != g_uniform_values.end() && found->second.signature == signature &&
                       found->second.bytes == bytes && std::memcmp(found->second.value.data(), value, bytes) == 0;
    if (mg_pz_census_active) {
        ++g_census.frame.uniform_tracked;
        if (exact) ++g_census.frame.uniform_exact;
    }
    uniform_value_t& stored = g_uniform_values[key];
    stored.signature = signature;
    stored.bytes = static_cast<uint16_t>(bytes);
    std::memcpy(stored.value.data(), value, bytes);
    const bool skipped = mg_pz_uniform_fastpath_active && exact;
    if (mg_pz_census_active && skipped) ++g_census.frame.uniform_skipped;
    return skipped;
}

void mg_pz_uniform_driver_write(GLuint program, GLint location, uint32_t signature, GLsizei count, const void* value,
                                size_t bytes) {
    if ((!mg_pz_census_active && !mg_pz_uniform_fastpath_active) || program == 0 || location < 0) return;
    if (count != 1 || value == nullptr || bytes == 0 || bytes > 128) {
        mg_pz_census_forget_program(program);
        return;
    }
    const uint64_t key = (static_cast<uint64_t>(program) << 32U) | static_cast<uint32_t>(location);
    uniform_value_t& stored = g_uniform_values[key];
    stored.signature = signature;
    stored.bytes = static_cast<uint16_t>(bytes);
    std::memcpy(stored.value.data(), value, bytes);
}

void mg_pz_census_forget_program(GLuint program) {
    if (!mg_pz_census_active && !mg_pz_uniform_fastpath_active) return;
    for (auto it = g_uniform_values.begin(); it != g_uniform_values.end();) {
        if (static_cast<GLuint>(it->first >> 32U) == program)
            it = g_uniform_values.erase(it);
        else
            ++it;
    }
}

void mg_pz_census_context_changed(unsigned long long context_id) {
    if ((!mg_pz_census_active && !mg_pz_uniform_fastpath_active) || context_id == g_uniform_context) return;
    g_uniform_context = context_id;
    g_uniform_values.clear();
    g_attrib_values.clear();
}

void mg_pz_census_attrib(mg_pz_attrib_kind kind, bool tracked, bool exact_redundant, bool skipped) {
    if (!mg_pz_census_active) return;
    const size_t index = static_cast<size_t>(kind);
    if (index >= g_census.frame.attrib_kind_calls.size()) return;
    ++g_census.frame.attrib_kind_calls[index];
    if (!tracked) return;
    ++g_census.frame.attrib_tracked;
    if (exact_redundant) {
        ++g_census.frame.attrib_exact;
        ++g_census.frame.attrib_kind_exact[index];
    }
    if (skipped) {
        ++g_census.frame.attrib_skipped;
        ++g_census.frame.attrib_kind_skipped[index];
    }
}

void mg_pz_census_attrib_value(GLuint index, uint32_t signature, const void* value, size_t bytes) {
    if (!mg_pz_census_active || value == nullptr || bytes == 0 || bytes > 128) return;
    const auto found = g_attrib_values.find(index);
    const bool exact = found != g_attrib_values.end() && found->second.signature == signature &&
                       found->second.bytes == bytes && std::memcmp(found->second.value.data(), value, bytes) == 0;
    mg_pz_census_attrib(mg_pz_attrib_kind::constant, true, exact);
    uniform_value_t& stored = g_attrib_values[index];
    stored.signature = signature;
    stored.bytes = static_cast<uint16_t>(bytes);
    std::memcpy(stored.value.data(), value, bytes);
}

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
