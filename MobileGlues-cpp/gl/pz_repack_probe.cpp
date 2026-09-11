// MobileGlues - gl/pz_repack_probe.cpp
// Diagnostic-only Project Zomboid tiny indexed-draw census.
//
// Phase 1 proved that PZ is dominated by tiny indexed triangle draws. Phase 2
// keeps rendering byte-for-byte unchanged and asks the next architectural
// question: can those draws actually be deindexed/repacked from CPU-visible
// persistent EBO/VBO backings without a driver readback?

#include "pz_repack_probe.h"

#if defined(ZOMDROID_EXPERIMENTAL)

#include "../gles/loader.h"
#include "log.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <unordered_map>

namespace {

constexpr unsigned kTrackedAttribs = 32;
constexpr unsigned kDeepSampleStride = 16;
constexpr unsigned long long kDeepWarmupSamples = 4096;

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

    glBindBuffer_PTR bind_buffer = nullptr;
    glBufferData_PTR buffer_data = nullptr;
    glBufferStorageEXT_PTR buffer_storage_ext = nullptr;
    glMapBufferRange_PTR map_buffer_range = nullptr;
    glUnmapBuffer_PTR unmap_buffer = nullptr;
    glDeleteBuffers_PTR delete_buffers = nullptr;
    glBindVertexArray_PTR bind_vertex_array = nullptr;
    glVertexAttribPointer_PTR vertex_attrib_pointer = nullptr;
    glVertexAttribIPointer_PTR vertex_attrib_i_pointer = nullptr;
    glEnableVertexAttribArray_PTR enable_vertex_attrib_array = nullptr;
    glDisableVertexAttribArray_PTR disable_vertex_attrib_array = nullptr;
    glVertexAttribDivisor_PTR vertex_attrib_divisor = nullptr;
};

struct mapping_t {
    unsigned char* pointer = nullptr;
    GLintptr offset = 0;
    GLsizeiptr length = 0;
    GLbitfield access = 0;
};

struct attrib_t {
    bool described = false;
    bool enabled = false;
    bool integer_format = false;
    GLint size = 0;
    GLenum type = 0;
    GLboolean normalized = GL_FALSE;
    GLsizei stride = 0;
    uintptr_t pointer = 0;
    GLuint buffer = 0;
    GLuint divisor = 0;
};

struct vao_t {
    GLuint element_buffer = 0;
    std::array<attrib_t, kTrackedAttribs> attribs{};
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

    // Sampled phase-2 feasibility counters. They never alter a GL argument.
    unsigned long long deep_samples = 0;
    unsigned long long ebo_readable = 0;
    unsigned long long vertex_readable = 0;
    unsigned long long repack_ready = 0;
    unsigned long long single_vbo = 0;
    unsigned long long no_enabled_attrib = 0;
    unsigned long long attrib_untracked = 0;
    unsigned long long attrib_unmapped = 0;
    unsigned long long attrib_out_of_range = 0;
    unsigned long long ebo_unmapped = 0;
    unsigned long long ebo_out_of_range = 0;
    unsigned long long quad6_samples = 0;
    unsigned long long quad6_topology = 0;
    unsigned long long quad6_nonquad = 0;
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

    GLuint array_buffer = 0;
    GLuint vao = 0;
    std::unordered_map<GLuint, mapping_t> mappings;
    std::unordered_map<GLuint, vao_t> vaos;
};

originals_t g_orig;
thread_local thread_state_t g_state;

vao_t& current_vao() {
    return g_state.vaos[g_state.vao];
}

GLuint bound_buffer(GLenum target) {
    if (target == GL_ARRAY_BUFFER) return g_state.array_buffer;
    if (target == GL_ELEMENT_ARRAY_BUFFER) return current_vao().element_buffer;
    return 0;
}

bool valid_index_type(GLenum type) {
    return type == GL_UNSIGNED_BYTE || type == GL_UNSIGNED_SHORT || type == GL_UNSIGNED_INT;
}

unsigned index_size(GLenum type) {
    if (type == GL_UNSIGNED_BYTE) return 1;
    if (type == GL_UNSIGNED_SHORT) return 2;
    if (type == GL_UNSIGNED_INT) return 4;
    return 0;
}

unsigned scalar_size(GLenum type) {
    switch (type) {
    case GL_BYTE:
    case GL_UNSIGNED_BYTE:
        return 1;
    case GL_SHORT:
    case GL_UNSIGNED_SHORT:
    case GL_HALF_FLOAT:
        return 2;
    case GL_INT:
    case GL_UNSIGNED_INT:
    case GL_FLOAT:
    case GL_FIXED:
        return 4;
    default:
        return 0;
    }
}

unsigned attrib_element_bytes(const attrib_t& a) {
    if (a.type == GL_INT_2_10_10_10_REV || a.type == GL_UNSIGNED_INT_2_10_10_10_REV) return 4;
    if (a.size < 1 || a.size > 4) return 0;
    const unsigned scalar = scalar_size(a.type);
    return scalar ? scalar * static_cast<unsigned>(a.size) : 0;
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
    SUB(shape_eligible);
    SUB(quad6_shape);
    SUB(relaxed_adjacent);
    SUB(deep_samples);
    SUB(ebo_readable);
    SUB(vertex_readable);
    SUB(repack_ready);
    SUB(quad6_samples);
    SUB(quad6_topology);
#undef SUB
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
    const double ebo_pct = s.deep_samples
                               ? 100.0 * static_cast<double>(s.ebo_readable) / static_cast<double>(s.deep_samples)
                               : 0.0;
    const double ready_pct = s.deep_samples
                                 ? 100.0 * static_cast<double>(s.repack_ready) / static_cast<double>(s.deep_samples)
                                 : 0.0;
    const double topology_pct = s.quad6_samples
                                    ? 100.0 * static_cast<double>(s.quad6_topology) /
                                          static_cast<double>(s.quad6_samples)
                                    : 0.0;

    LOG_I("ZOMDROID_PZ_REPACK_PROBE indexed=%llu tri=%llu fan=%llu shape=%llu/%.2f%% quad6_shape=%llu/%.2f%% "
          "exact=%llu/%llu/%llu/%llu le=%llu/%llu/%llu/%llu type=%llu/%llu/%llu/%llu instanced=%llu "
          "basev=%llu offset32=%llu relaxed=%llu/%llu/%llu/%.2f%% "
          "deep=%llu ebo=%llu/%.2f%% vertex=%llu ready=%llu/%.2f%% singlevbo=%llu "
          "deep_miss=%llu/%llu/%llu/%llu/%llu/%llu quad6_topo=%llu/%llu/%llu/%.2f%% "
          "delta=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu",
          s.indexed, s.triangles, s.triangle_fan, s.shape_eligible, shape_pct, s.quad6_shape, q6_pct,
          s.exact3, s.exact6, s.exact9, s.exact12, s.le12, s.le24, s.le48, s.le96,
          s.type_u8, s.type_u16, s.type_u32, s.type_other, s.instanced, s.basevertex, s.offset32,
          s.relaxed_runs, s.relaxed_adjacent, s.relaxed_max_run, upper_pct,
          s.deep_samples, s.ebo_readable, ebo_pct, s.vertex_readable, s.repack_ready, ready_pct, s.single_vbo,
          s.no_enabled_attrib, s.attrib_untracked, s.attrib_unmapped, s.attrib_out_of_range,
          s.ebo_unmapped, s.ebo_out_of_range, s.quad6_topology, s.quad6_nonquad,
          s.quad6_samples, topology_pct,
          d.indexed, d.shape_eligible, d.quad6_shape, d.relaxed_adjacent, d.deep_samples,
          d.ebo_readable, d.vertex_readable, d.repack_ready, d.quad6_samples, d.quad6_topology)

    g_state.last_report = s;
}

void maybe_report() {
    const unsigned long long n = g_state.stats.indexed;
    if (n == 1 || n == 1024 || n == 65536 || (n != 0 && n % 250000ULL == 0)) report_probe();
}

bool mapped_range(GLuint buffer, uint64_t byte_offset, uint64_t bytes, const unsigned char** out) {
    const auto found = g_state.mappings.find(buffer);
    if (found == g_state.mappings.end() || found->second.pointer == nullptr || found->second.length <= 0) return false;
    const mapping_t& map = found->second;
    if (map.offset < 0 || map.length < 0) return false;
    const uint64_t map_begin = static_cast<uint64_t>(map.offset);
    const uint64_t map_end = map_begin + static_cast<uint64_t>(map.length);
    if (map_end < map_begin || byte_offset < map_begin || bytes > map_end - byte_offset) return false;
    *out = map.pointer + static_cast<size_t>(byte_offset - map_begin);
    return true;
}

bool read_indices(GLenum type, const void* indices, GLsizei count, std::array<uint32_t, 96>& decoded,
                  uint32_t* min_index, uint32_t* max_index) {
    const unsigned bytes_per_index = index_size(type);
    if (bytes_per_index == 0 || count <= 0 || count > static_cast<GLsizei>(decoded.size())) return false;
    const GLuint ebo = current_vao().element_buffer;
    if (ebo == 0) return false;

    const uint64_t offset = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(indices));
    const uint64_t bytes = static_cast<uint64_t>(count) * bytes_per_index;
    const unsigned char* source = nullptr;
    if (!mapped_range(ebo, offset, bytes, &source)) return false;

    uint32_t lo = std::numeric_limits<uint32_t>::max();
    uint32_t hi = 0;
    for (GLsizei i = 0; i < count; ++i) {
        uint32_t value = 0;
        if (type == GL_UNSIGNED_BYTE) {
            value = source[i];
        } else if (type == GL_UNSIGNED_SHORT) {
            uint16_t v = 0;
            std::memcpy(&v, source + static_cast<size_t>(i) * 2U, sizeof(v));
            value = v;
        } else {
            uint32_t v = 0;
            std::memcpy(&v, source + static_cast<size_t>(i) * 4U, sizeof(v));
            value = v;
        }
        decoded[static_cast<size_t>(i)] = value;
        lo = std::min(lo, value);
        hi = std::max(hi, value);
    }
    *min_index = lo;
    *max_index = hi;
    return true;
}

bool is_quad6_topology(const std::array<uint32_t, 96>& idx) {
    if (idx[0] == idx[1] || idx[0] == idx[2] || idx[1] == idx[2]) return false;
    if (idx[3] == idx[4] || idx[3] == idx[5] || idx[4] == idx[5]) return false;

    std::array<uint32_t, 6> values = {idx[0], idx[1], idx[2], idx[3], idx[4], idx[5]};
    std::sort(values.begin(), values.end());
    unsigned unique = 1;
    for (size_t i = 1; i < values.size(); ++i) {
        if (values[i] != values[i - 1]) ++unique;
    }
    if (unique != 4) return false;

    unsigned shared = 0;
    for (unsigned a = 0; a < 3; ++a) {
        for (unsigned b = 3; b < 6; ++b) {
            if (idx[a] == idx[b]) ++shared;
        }
    }
    return shared == 2;
}

bool vertex_inputs_readable(uint32_t min_index, uint32_t max_index, GLint basevertex, bool* single_vbo) {
    const vao_t& vao = current_vao();
    bool any = false;
    GLuint first_buffer = 0;
    bool one_buffer = true;

    for (const attrib_t& a : vao.attribs) {
        if (!a.enabled) continue;
        any = true;
        if (!a.described || a.buffer == 0 || a.divisor != 0) {
            ++g_state.stats.attrib_untracked;
            return false;
        }
        if (first_buffer == 0)
            first_buffer = a.buffer;
        else if (a.buffer != first_buffer)
            one_buffer = false;

        const unsigned element_bytes = attrib_element_bytes(a);
        if (element_bytes == 0 || a.stride < 0) {
            ++g_state.stats.attrib_untracked;
            return false;
        }
        const uint64_t stride = a.stride ? static_cast<uint64_t>(a.stride) : element_bytes;

        const int64_t lo_index = static_cast<int64_t>(min_index) + basevertex;
        const int64_t hi_index = static_cast<int64_t>(max_index) + basevertex;
        if (lo_index < 0 || hi_index < lo_index) {
            ++g_state.stats.attrib_out_of_range;
            return false;
        }
        const uint64_t pointer = static_cast<uint64_t>(a.pointer);
        const uint64_t lo_offset = pointer + static_cast<uint64_t>(lo_index) * stride;
        const uint64_t hi_offset = pointer + static_cast<uint64_t>(hi_index) * stride;
        if (lo_offset < pointer || hi_offset < pointer || hi_offset > std::numeric_limits<uint64_t>::max() - element_bytes) {
            ++g_state.stats.attrib_out_of_range;
            return false;
        }

        const unsigned char* unused = nullptr;
        if (!mapped_range(a.buffer, lo_offset, element_bytes, &unused) ||
            !mapped_range(a.buffer, hi_offset, element_bytes, &unused)) {
            if (g_state.mappings.find(a.buffer) == g_state.mappings.end())
                ++g_state.stats.attrib_unmapped;
            else
                ++g_state.stats.attrib_out_of_range;
            return false;
        }
    }

    if (!any) {
        ++g_state.stats.no_enabled_attrib;
        return false;
    }
    *single_vbo = one_buffer;
    return true;
}

void deep_check(GLsizei count, GLenum type, const void* indices, GLint basevertex) {
    stats_t& s = g_state.stats;
    ++s.deep_samples;
    std::array<uint32_t, 96> decoded{};
    uint32_t min_index = 0;
    uint32_t max_index = 0;

    const GLuint ebo = current_vao().element_buffer;
    if (ebo == 0 || g_state.mappings.find(ebo) == g_state.mappings.end()) {
        ++s.ebo_unmapped;
        return;
    }
    if (!read_indices(type, indices, count, decoded, &min_index, &max_index)) {
        ++s.ebo_out_of_range;
        return;
    }
    ++s.ebo_readable;

    if (count == 6) {
        ++s.quad6_samples;
        if (is_quad6_topology(decoded))
            ++s.quad6_topology;
        else
            ++s.quad6_nonquad;
    }

    bool single = false;
    if (!vertex_inputs_readable(min_index, max_index, basevertex, &single)) return;
    ++s.vertex_readable;
    if (single) ++s.single_vbo;
    ++s.repack_ready;
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

        // Deep feasibility checks are sampled to keep the diagnostic from doing
        // millions of CPU memory walks. Structural/run counters above remain exact.
        if (s.shape_eligible <= kDeepWarmupSamples || (s.shape_eligible % kDeepSampleStride) == 0)
            deep_check(count, type, indices, basevertex);
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
    ++g_state.material_epoch;
    g_orig.bind_texture(target, texture);
}

void probe_glBindBuffer(GLenum target, GLuint buffer) {
    if (target == GL_ARRAY_BUFFER)
        g_state.array_buffer = buffer;
    else if (target == GL_ELEMENT_ARRAY_BUFFER)
        current_vao().element_buffer = buffer;
    g_orig.bind_buffer(target, buffer);
}

void invalidate_bound_mapping(GLenum target) {
    const GLuint buffer = bound_buffer(target);
    if (buffer) g_state.mappings.erase(buffer);
}

void probe_glBufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage) {
    invalidate_bound_mapping(target);
    g_orig.buffer_data(target, size, data, usage);
}

void probe_glBufferStorageEXT(GLenum target, GLsizeiptr size, const void* data, GLbitfield flags) {
    invalidate_bound_mapping(target);
    g_orig.buffer_storage_ext(target, size, data, flags);
}

void* probe_glMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access) {
    void* result = g_orig.map_buffer_range(target, offset, length, access);
    const GLuint buffer = bound_buffer(target);
    if (result != nullptr && buffer != 0 && offset >= 0 && length > 0)
        g_state.mappings[buffer] = mapping_t{static_cast<unsigned char*>(result), offset, length, access};
    return result;
}

GLboolean probe_glUnmapBuffer(GLenum target) {
    const GLuint buffer = bound_buffer(target);
    const GLboolean result = g_orig.unmap_buffer(target);
    if (result && buffer) g_state.mappings.erase(buffer);
    return result;
}

void probe_glDeleteBuffers(GLsizei n, const GLuint* buffers) {
    g_orig.delete_buffers(n, buffers);
    if (buffers == nullptr || n <= 0) return;
    for (GLsizei i = 0; i < n; ++i) {
        const GLuint dead = buffers[i];
        g_state.mappings.erase(dead);
        if (g_state.array_buffer == dead) g_state.array_buffer = 0;
        for (auto& entry : g_state.vaos) {
            if (entry.second.element_buffer == dead) entry.second.element_buffer = 0;
            for (attrib_t& a : entry.second.attribs) {
                if (a.buffer == dead) a.buffer = 0;
            }
        }
    }
}

void probe_glBindVertexArray(GLuint array) {
    g_state.vao = array;
    (void)current_vao();
    g_orig.bind_vertex_array(array);
}

void probe_glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride,
                                 const void* pointer) {
    if (index < kTrackedAttribs) {
        attrib_t& a = current_vao().attribs[index];
        a.described = true;
        a.integer_format = false;
        a.size = size;
        a.type = type;
        a.normalized = normalized;
        a.stride = stride;
        a.pointer = reinterpret_cast<uintptr_t>(pointer);
        a.buffer = g_state.array_buffer;
    }
    g_orig.vertex_attrib_pointer(index, size, type, normalized, stride, pointer);
}

void probe_glVertexAttribIPointer(GLuint index, GLint size, GLenum type, GLsizei stride, const void* pointer) {
    if (index < kTrackedAttribs) {
        attrib_t& a = current_vao().attribs[index];
        a.described = true;
        a.integer_format = true;
        a.size = size;
        a.type = type;
        a.normalized = GL_FALSE;
        a.stride = stride;
        a.pointer = reinterpret_cast<uintptr_t>(pointer);
        a.buffer = g_state.array_buffer;
    }
    g_orig.vertex_attrib_i_pointer(index, size, type, stride, pointer);
}

void probe_glEnableVertexAttribArray(GLuint index) {
    if (index < kTrackedAttribs) current_vao().attribs[index].enabled = true;
    g_orig.enable_vertex_attrib_array(index);
}

void probe_glDisableVertexAttribArray(GLuint index) {
    if (index < kTrackedAttribs) current_vao().attribs[index].enabled = false;
    g_orig.disable_vertex_attrib_array(index);
}

void probe_glVertexAttribDivisor(GLuint index, GLuint divisor) {
    if (index < kTrackedAttribs) current_vao().attribs[index].divisor = divisor;
    g_orig.vertex_attrib_divisor(index, divisor);
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

    g_orig.bind_buffer = capture<glBindBuffer_PTR>(GLES.glBindBuffer);
    g_orig.buffer_data = capture<glBufferData_PTR>(GLES.glBufferData);
    g_orig.buffer_storage_ext = capture<glBufferStorageEXT_PTR>(GLES.glBufferStorageEXT);
    g_orig.map_buffer_range = capture<glMapBufferRange_PTR>(GLES.glMapBufferRange);
    g_orig.unmap_buffer = capture<glUnmapBuffer_PTR>(GLES.glUnmapBuffer);
    g_orig.delete_buffers = capture<glDeleteBuffers_PTR>(GLES.glDeleteBuffers);
    g_orig.bind_vertex_array = capture<glBindVertexArray_PTR>(GLES.glBindVertexArray);
    g_orig.vertex_attrib_pointer = capture<glVertexAttribPointer_PTR>(GLES.glVertexAttribPointer);
    g_orig.vertex_attrib_i_pointer = capture<glVertexAttribIPointer_PTR>(GLES.glVertexAttribIPointer);
    g_orig.enable_vertex_attrib_array = capture<glEnableVertexAttribArray_PTR>(GLES.glEnableVertexAttribArray);
    g_orig.disable_vertex_attrib_array = capture<glDisableVertexAttribArray_PTR>(GLES.glDisableVertexAttribArray);
    g_orig.vertex_attrib_divisor = capture<glVertexAttribDivisor_PTR>(GLES.glVertexAttribDivisor);

    if (g_orig.draw_elements == nullptr || g_orig.use_program == nullptr || g_orig.active_texture == nullptr ||
        g_orig.bind_texture == nullptr || g_orig.bind_buffer == nullptr || g_orig.map_buffer_range == nullptr ||
        g_orig.unmap_buffer == nullptr || g_orig.bind_vertex_array == nullptr ||
        g_orig.vertex_attrib_pointer == nullptr || g_orig.enable_vertex_attrib_array == nullptr ||
        g_orig.disable_vertex_attrib_array == nullptr) {
        LOG_W_FORCE("ZOMDROID_PZ_REPACK_PROBE disabled reason=backend_capability_missing")
        return;
    }

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

    GLES.glBindBuffer = probe_glBindBuffer;
    if (g_orig.buffer_data) GLES.glBufferData = probe_glBufferData;
    if (g_orig.buffer_storage_ext) GLES.glBufferStorageEXT = probe_glBufferStorageEXT;
    GLES.glMapBufferRange = probe_glMapBufferRange;
    GLES.glUnmapBuffer = probe_glUnmapBuffer;
    if (g_orig.delete_buffers) GLES.glDeleteBuffers = probe_glDeleteBuffers;
    GLES.glBindVertexArray = probe_glBindVertexArray;
    GLES.glVertexAttribPointer = probe_glVertexAttribPointer;
    if (g_orig.vertex_attrib_i_pointer) GLES.glVertexAttribIPointer = probe_glVertexAttribIPointer;
    GLES.glEnableVertexAttribArray = probe_glEnableVertexAttribArray;
    GLES.glDisableVertexAttribArray = probe_glDisableVertexAttribArray;
    if (g_orig.vertex_attrib_divisor) GLES.glVertexAttribDivisor = probe_glVertexAttribDivisor;

    LOG_I("ZOMDROID_PZ_REPACK_PROBE enabled=1 mode=backend_observe_only phase=2 tiny_max=96 "
          "deep_sample=warmup4096_then_1of16 ready=ebo_plus_all_enabled_vertex_inputs_cpu_visible "
          "quad6_topology=two_nondegenerate_triangles_four_unique_vertices relaxed_upper=texture_program_only")
}

#else

void mg_pz_repack_probe_install(void) {}

#endif
