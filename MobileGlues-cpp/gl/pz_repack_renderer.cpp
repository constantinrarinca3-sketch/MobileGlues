// MobileGlues - gl/pz_repack_renderer.cpp
// Project Zomboid experiment: collapse compatible tiny indexed quad draws into
// one deindexed backend triangle stream. This lives below the stable frontend;
// unsupported state always falls back to the original GLES draw.

#include "pz_repack_renderer.h"

#if defined(ZOMDROID_EXPERIMENTAL)

#include "threaded_submission.h"
#include "pz_census.h"
#include "../gles/loader.h"
#include "log.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr unsigned kTrackedAttribs = 32;
constexpr size_t kRingSlots = 4;
constexpr size_t kRingBytes = 8U * 1024U * 1024U;
constexpr size_t kShaderTextLimit = 1024U * 1024U;

struct originals_t {
    glDrawElements_PTR draw_elements = nullptr;
    glDrawArrays_PTR draw_arrays = nullptr;
    glBindBuffer_PTR bind_buffer = nullptr;
    glBufferData_PTR buffer_data = nullptr;
    glBufferSubData_PTR buffer_sub_data = nullptr;
    glBufferStorageEXT_PTR buffer_storage_ext = nullptr;
    glMapBufferRange_PTR map_buffer_range = nullptr;
    glUnmapBuffer_PTR unmap_buffer = nullptr;
    glDeleteBuffers_PTR delete_buffers = nullptr;
    glGenBuffers_PTR gen_buffers = nullptr;
    glMemoryBarrier_PTR memory_barrier = nullptr;
    glFenceSync_PTR fence_sync = nullptr;
    glClientWaitSync_PTR client_wait_sync = nullptr;
    glDeleteSync_PTR delete_sync = nullptr;

    glBindVertexArray_PTR bind_vertex_array = nullptr;
    glGenVertexArrays_PTR gen_vertex_arrays = nullptr;
    glDeleteVertexArrays_PTR delete_vertex_arrays = nullptr;
    glVertexAttribPointer_PTR vertex_attrib_pointer = nullptr;
    glVertexAttribIPointer_PTR vertex_attrib_i_pointer = nullptr;
    glEnableVertexAttribArray_PTR enable_vertex_attrib_array = nullptr;
    glDisableVertexAttribArray_PTR disable_vertex_attrib_array = nullptr;
    glVertexAttribDivisor_PTR vertex_attrib_divisor = nullptr;
    glBindVertexBuffer_PTR bind_vertex_buffer = nullptr;
    glVertexAttribFormat_PTR vertex_attrib_format = nullptr;
    glVertexAttribIFormat_PTR vertex_attrib_i_format = nullptr;
    glVertexAttribBinding_PTR vertex_attrib_binding = nullptr;
    glVertexBindingDivisor_PTR vertex_binding_divisor = nullptr;

    glUseProgram_PTR use_program = nullptr;
    glShaderSource_PTR shader_source = nullptr;
    glAttachShader_PTR attach_shader = nullptr;
    glDetachShader_PTR detach_shader = nullptr;
    glLinkProgram_PTR link_program = nullptr;
    glDeleteProgram_PTR delete_program = nullptr;
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
    bool unsupported_vertex_model = false;
    std::array<attrib_t, kTrackedAttribs> attribs{};
};

struct packed_attrib_t {
    bool enabled = false;
    bool integer_format = false;
    GLint size = 0;
    GLenum type = 0;
    GLboolean normalized = GL_FALSE;
    uint32_t bytes = 0;
    uint32_t offset = 0;
};

struct packed_layout_t {
    std::array<packed_attrib_t, kTrackedAttribs> attribs{};
    uint32_t stride = 0;
};

struct candidate_t {
    GLuint program = 0;
    GLuint ebo = 0;
    GLuint source_vbo = 0;
    uintptr_t index_offset = 0;
    std::array<uint16_t, 6> indices{};
    std::array<attrib_t, kTrackedAttribs> attribs{};
    packed_layout_t layout{};
};

struct shader_t {
    bool source_known = false;
    bool vertex_identity_sensitive = true;
};

struct program_t {
    std::vector<GLuint> attached;
    bool linked_known = false;
    bool linked_safe = false;
};

struct ring_slot_t {
    GLuint buffer = 0;
    unsigned char* pointer = nullptr;
    size_t cursor = 0;
    GLsync fence = nullptr;
};

struct batch_t {
    bool active = false;
    size_t slot = 0;
    size_t start = 0;
    GLsizei vertices = 0;
    unsigned long long source_draws = 0;
    GLuint program = 0;
    packed_layout_t layout{};
};

struct stats_t {
    unsigned long long source_draws = 0;
    unsigned long long candidates = 0;
    unsigned long long deferred = 0;
    unsigned long long pairs = 0;
    unsigned long long batches = 0;
    unsigned long long batched_sources = 0;
    unsigned long long eliminated = 0;
    unsigned long long replays = 0;
    unsigned long long passthrough = 0;
    unsigned long long max_batch = 0;
    unsigned long long packed_bytes = 0;
    unsigned long long ring_allocations = 0;
    unsigned long long ring_rotations = 0;
    unsigned long long ring_busy = 0;
    unsigned long long fences = 0;
    unsigned long long signaled = 0;
    unsigned long long miss_program = 0;
    unsigned long long miss_shape = 0;
    unsigned long long miss_ebo = 0;
    unsigned long long miss_topology = 0;
    unsigned long long miss_vertex = 0;
    unsigned long long miss_layout = 0;
};

struct renderer_state_t {
    bool worker_context = false;
    GLuint array_buffer = 0;
    GLuint vao = 0;
    GLuint program = 0;
    std::unordered_map<GLuint, mapping_t> mappings;
    std::unordered_map<GLuint, vao_t> vaos;
    std::unordered_map<GLuint, shader_t> shaders;
    std::unordered_map<GLuint, program_t> programs;

    bool has_pending = false;
    candidate_t pending{};
    batch_t batch{};

    std::array<ring_slot_t, kRingSlots> ring{};
    size_t ring_current = 0;
    bool ring_current_valid = false;

    GLuint replay_vao = 0;
    GLuint batch_vao = 0;
    std::array<attrib_t, kTrackedAttribs> replay_config{};
    std::array<bool, kTrackedAttribs> replay_enabled{};
    bool replay_config_valid = false;
    packed_layout_t batch_layout{};
    GLuint batch_layout_buffer = 0;
    std::array<bool, kTrackedAttribs> batch_enabled{};
    bool batch_layout_valid = false;

    stats_t stats{};
};

originals_t g_orig;
bool g_enabled = false;
thread_local renderer_state_t g_state;

vao_t& current_vao() { return g_state.vaos[g_state.vao]; }

GLuint bound_buffer(GLenum target) {
    if (target == GL_ARRAY_BUFFER) return g_state.array_buffer;
    if (target == GL_ELEMENT_ARRAY_BUFFER) return current_vao().element_buffer;
    return 0;
}

size_t align_up(size_t value, size_t alignment) {
    if (alignment <= 1) return value;
    const size_t remainder = value % alignment;
    if (remainder == 0) return value;
    if (value > std::numeric_limits<size_t>::max() - (alignment - remainder))
        return std::numeric_limits<size_t>::max();
    return value + (alignment - remainder);
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

unsigned attrib_alignment(const attrib_t& a) {
    if (a.type == GL_INT_2_10_10_10_REV || a.type == GL_UNSIGNED_INT_2_10_10_10_REV) return 4;
    return std::min(4U, std::max(1U, scalar_size(a.type)));
}

bool safe_mapping(GLuint buffer, const mapping_t** mapping) {
    const auto found = g_state.mappings.find(buffer);
    if (found == g_state.mappings.end() || found->second.pointer == nullptr || found->second.length <= 0)
        return false;
    const GLbitfield required = GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    if ((found->second.access & required) != required) return false;
    *mapping = &found->second;
    return true;
}

bool mapped_range(GLuint buffer, uint64_t byte_offset, uint64_t bytes, const unsigned char** out) {
    const mapping_t* map = nullptr;
    if (!safe_mapping(buffer, &map) || map->offset < 0 || map->length < 0) return false;
    const uint64_t begin = static_cast<uint64_t>(map->offset);
    const uint64_t end = begin + static_cast<uint64_t>(map->length);
    if (end < begin || byte_offset < begin || bytes > end - byte_offset) return false;
    *out = map->pointer + static_cast<size_t>(byte_offset - begin);
    return true;
}

bool same_layout(const packed_layout_t& a, const packed_layout_t& b) {
    if (a.stride != b.stride) return false;
    for (size_t i = 0; i < kTrackedAttribs; ++i) {
        const packed_attrib_t& x = a.attribs[i];
        const packed_attrib_t& y = b.attribs[i];
        if (x.enabled != y.enabled) return false;
        if (!x.enabled) continue;
        if (x.integer_format != y.integer_format || x.size != y.size || x.type != y.type ||
            x.normalized != y.normalized || x.bytes != y.bytes || x.offset != y.offset)
            return false;
    }
    return true;
}

bool same_replay_attrib(const attrib_t& a, const attrib_t& b) {
    return a.described == b.described && a.integer_format == b.integer_format && a.size == b.size &&
           a.type == b.type && a.normalized == b.normalized && a.stride == b.stride &&
           a.pointer == b.pointer && a.buffer == b.buffer && a.divisor == b.divisor;
}

bool quad6_topology(const std::array<uint16_t, 6>& idx) {
    for (uint16_t value : idx)
        if (value == std::numeric_limits<uint16_t>::max()) return false;
    if (idx[0] == idx[1] || idx[0] == idx[2] || idx[1] == idx[2]) return false;
    if (idx[3] == idx[4] || idx[3] == idx[5] || idx[4] == idx[5]) return false;

    std::array<uint16_t, 6> values = idx;
    std::sort(values.begin(), values.end());
    unsigned unique = 1;
    for (size_t i = 1; i < values.size(); ++i)
        if (values[i] != values[i - 1]) ++unique;
    if (unique != 4) return false;

    unsigned shared = 0;
    for (unsigned a = 0; a < 3; ++a)
        for (unsigned b = 3; b < 6; ++b)
            if (idx[a] == idx[b]) ++shared;
    return shared == 2;
}

bool program_safe(GLuint program) {
    const auto found = g_state.programs.find(program);
    return program != 0 && found != g_state.programs.end() && found->second.linked_known &&
           found->second.linked_safe;
}

bool source_has_identity_builtin(GLsizei count, const GLchar* const* strings, const GLint* lengths,
                                 bool* known) {
    *known = false;
    if (count <= 0 || strings == nullptr) return true;
    std::string source;
    try {
        for (GLsizei i = 0; i < count; ++i) {
            if (strings[i] == nullptr) return true;
            size_t bytes = 0;
            if (lengths != nullptr && lengths[i] >= 0)
                bytes = static_cast<size_t>(lengths[i]);
            else
                bytes = std::strlen(strings[i]);
            if (bytes > kShaderTextLimit || source.size() > kShaderTextLimit - bytes) return true;
            source.append(strings[i], bytes);
        }
    } catch (...) {
        return true;
    }
    *known = true;
    static const char* sensitive[] = {"gl_VertexID", "gl_PrimitiveID", "gl_DrawID", "gl_BaseVertex",
                                      "gl_BaseInstance"};
    for (const char* token : sensitive)
        if (source.find(token) != std::string::npos) return true;
    return false;
}

bool ensure_replay_vao() {
    if (g_state.replay_vao != 0) return true;
    if (g_orig.gen_vertex_arrays == nullptr) return false;
    g_orig.gen_vertex_arrays(1, &g_state.replay_vao);
    g_state.replay_config_valid = false;
    return g_state.replay_vao != 0;
}

bool ensure_batch_vao() {
    if (g_state.batch_vao != 0) return true;
    if (g_orig.gen_vertex_arrays == nullptr) return false;
    g_orig.gen_vertex_arrays(1, &g_state.batch_vao);
    g_state.batch_layout_valid = false;
    return g_state.batch_vao != 0;
}

bool build_candidate(GLenum mode, GLsizei count, GLenum type, const void* indices, candidate_t* out) {
    if (mode != GL_TRIANGLES || count != 6 || type != GL_UNSIGNED_SHORT) {
        ++g_state.stats.miss_shape;
        return false;
    }
    if (!program_safe(g_state.program)) {
        ++g_state.stats.miss_program;
        return false;
    }

    const vao_t& vao = current_vao();
    if (vao.unsupported_vertex_model || vao.element_buffer == 0) {
        ++g_state.stats.miss_ebo;
        return false;
    }

    const uintptr_t index_offset = reinterpret_cast<uintptr_t>(indices);
    const unsigned char* index_bytes = nullptr;
    if (!mapped_range(vao.element_buffer, static_cast<uint64_t>(index_offset), 12, &index_bytes)) {
        ++g_state.stats.miss_ebo;
        return false;
    }

    candidate_t candidate{};
    candidate.program = g_state.program;
    candidate.ebo = vao.element_buffer;
    candidate.index_offset = index_offset;
    for (size_t i = 0; i < candidate.indices.size(); ++i)
        std::memcpy(&candidate.indices[i], index_bytes + i * sizeof(uint16_t), sizeof(uint16_t));
    if (!quad6_topology(candidate.indices)) {
        ++g_state.stats.miss_topology;
        return false;
    }

    bool any = false;
    GLuint source_vbo = 0;
    size_t packed = 0;
    for (size_t i = 0; i < kTrackedAttribs; ++i) {
        const attrib_t& a = vao.attribs[i];
        candidate.attribs[i] = a;
        if (!a.enabled) continue;
        any = true;
        if (!a.described || a.buffer == 0 || a.divisor != 0 || a.stride < 0) {
            ++g_state.stats.miss_vertex;
            return false;
        }
        if (source_vbo == 0)
            source_vbo = a.buffer;
        else if (source_vbo != a.buffer) {
            ++g_state.stats.miss_vertex;
            return false;
        }

        const unsigned bytes = attrib_element_bytes(a);
        const unsigned alignment = attrib_alignment(a);
        if (bytes == 0 || alignment == 0) {
            ++g_state.stats.miss_layout;
            return false;
        }
        packed = align_up(packed, alignment);
        if (packed == std::numeric_limits<size_t>::max() || packed > UINT32_MAX - bytes) {
            ++g_state.stats.miss_layout;
            return false;
        }
        packed_attrib_t& p = candidate.layout.attribs[i];
        p.enabled = true;
        p.integer_format = a.integer_format;
        p.size = a.size;
        p.type = a.type;
        p.normalized = a.normalized;
        p.bytes = bytes;
        p.offset = static_cast<uint32_t>(packed);
        packed += bytes;

        const uint64_t stride = a.stride ? static_cast<uint64_t>(a.stride) : bytes;
        for (uint16_t index : candidate.indices) {
            const uint64_t pointer = static_cast<uint64_t>(a.pointer);
            if (static_cast<uint64_t>(index) > (std::numeric_limits<uint64_t>::max() - pointer) / stride) {
                ++g_state.stats.miss_vertex;
                return false;
            }
            const uint64_t offset = pointer + static_cast<uint64_t>(index) * stride;
            const unsigned char* unused = nullptr;
            if (!mapped_range(a.buffer, offset, bytes, &unused)) {
                ++g_state.stats.miss_vertex;
                return false;
            }
        }
    }
    if (!any || source_vbo == 0) {
        ++g_state.stats.miss_vertex;
        return false;
    }
    packed = align_up(packed, 4);
    if (packed == 0 || packed == std::numeric_limits<size_t>::max() || packed > UINT32_MAX) {
        ++g_state.stats.miss_layout;
        return false;
    }
    candidate.layout.stride = static_cast<uint32_t>(packed);
    candidate.source_vbo = source_vbo;
    *out = candidate;
    return true;
}

bool pack_candidate(const candidate_t& candidate, unsigned char* destination) {
    if (destination == nullptr || candidate.layout.stride == 0) return false;
    for (size_t vertex = 0; vertex < candidate.indices.size(); ++vertex) {
        unsigned char* out = destination + vertex * candidate.layout.stride;
        const uint16_t index = candidate.indices[vertex];
        for (size_t i = 0; i < kTrackedAttribs; ++i) {
            const packed_attrib_t& p = candidate.layout.attribs[i];
            if (!p.enabled) continue;
            const attrib_t& a = candidate.attribs[i];
            const uint64_t stride = a.stride ? static_cast<uint64_t>(a.stride) : p.bytes;
            const uint64_t offset = static_cast<uint64_t>(a.pointer) + static_cast<uint64_t>(index) * stride;
            const unsigned char* source = nullptr;
            if (!mapped_range(a.buffer, offset, p.bytes, &source)) return false;
            std::memcpy(out + p.offset, source, p.bytes);
        }
    }
    return true;
}

void configure_replay_vao(const candidate_t& candidate) {
    g_orig.bind_vertex_array(g_state.replay_vao);
    g_orig.bind_buffer(GL_ARRAY_BUFFER, candidate.source_vbo);
    for (GLuint i = 0; i < kTrackedAttribs; ++i) {
        const attrib_t& wanted = candidate.attribs[i];
        if (wanted.enabled) {
            const bool pointer_changed = !g_state.replay_config_valid || !g_state.replay_enabled[i] ||
                                         !same_replay_attrib(g_state.replay_config[i], wanted);
            if (pointer_changed) {
                const void* pointer = reinterpret_cast<const void*>(wanted.pointer);
                if (wanted.integer_format)
                    g_orig.vertex_attrib_i_pointer(i, wanted.size, wanted.type, wanted.stride, pointer);
                else
                    g_orig.vertex_attrib_pointer(i, wanted.size, wanted.type, wanted.normalized, wanted.stride, pointer);
                if (g_orig.vertex_attrib_divisor) g_orig.vertex_attrib_divisor(i, 0);
            }
            if (!g_state.replay_config_valid || !g_state.replay_enabled[i])
                g_orig.enable_vertex_attrib_array(i);
            g_state.replay_config[i] = wanted;
            g_state.replay_enabled[i] = true;
        } else if (g_state.replay_config_valid && g_state.replay_enabled[i]) {
            g_orig.disable_vertex_attrib_array(i);
            g_state.replay_enabled[i] = false;
        }
    }
    g_state.replay_config_valid = true;
    g_orig.bind_buffer(GL_ELEMENT_ARRAY_BUFFER, candidate.ebo);
}

void replay_candidate(const candidate_t& candidate) {
    if (!ensure_replay_vao()) {
        // This path is practically unreachable because a candidate is never
        // deferred before the replay VAO exists. Keep the fallback explicit.
        ++g_state.stats.passthrough;
        g_orig.draw_elements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT,
                             reinterpret_cast<const void*>(candidate.index_offset));
        return;
    }
    const GLuint restore_vao = g_state.vao;
    const GLuint restore_array = g_state.array_buffer;
    configure_replay_vao(candidate);
    g_orig.draw_elements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT,
                         reinterpret_cast<const void*>(candidate.index_offset));
    g_orig.bind_vertex_array(restore_vao);
    g_orig.bind_buffer(GL_ARRAY_BUFFER, restore_array);
    ++g_state.stats.replays;
}

bool allocate_ring_slot(size_t index) {
    if (index >= kRingSlots || g_orig.gen_buffers == nullptr || g_orig.buffer_storage_ext == nullptr ||
        g_orig.map_buffer_range == nullptr)
        return false;
    ring_slot_t& slot = g_state.ring[index];
    if (slot.buffer != 0) return true;

    const GLuint restore_array = g_state.array_buffer;
    g_orig.gen_buffers(1, &slot.buffer);
    if (slot.buffer == 0) return false;
    g_orig.bind_buffer(GL_ARRAY_BUFFER, slot.buffer);
    const GLbitfield storage =
        GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT | GL_DYNAMIC_STORAGE_BIT;
    g_orig.buffer_storage_ext(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(kRingBytes), nullptr, storage);
    slot.pointer = static_cast<unsigned char*>(g_orig.map_buffer_range(
        GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(kRingBytes),
        GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT));
    g_orig.bind_buffer(GL_ARRAY_BUFFER, restore_array);
    if (slot.pointer == nullptr) {
        g_orig.delete_buffers(1, &slot.buffer);
        slot = {};
        return false;
    }
    slot.cursor = 0;
    ++g_state.stats.ring_allocations;
    return true;
}

bool fence_ring_slot(size_t index) {
    ring_slot_t& slot = g_state.ring[index];
    if (slot.buffer == 0 || slot.cursor == 0 || slot.fence != nullptr) return true;
    slot.fence = g_orig.fence_sync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    if (slot.fence == nullptr) return false;
    ++g_state.stats.fences;
    return true;
}

bool retire_ring_slot(size_t index) {
    ring_slot_t& slot = g_state.ring[index];
    if (slot.buffer == 0) return false;
    if (slot.fence == nullptr) return slot.cursor == 0;
    const GLenum result = g_orig.client_wait_sync(slot.fence, 0, 0);
    if (result != GL_ALREADY_SIGNALED && result != GL_CONDITION_SATISFIED) return false;
    g_orig.delete_sync(slot.fence);
    slot.fence = nullptr;
    slot.cursor = 0;
    ++g_state.stats.signaled;
    return true;
}

bool select_ring_region(size_t bytes, size_t alignment, size_t* slot_index, size_t* start,
                        unsigned char** pointer) {
    if (bytes == 0 || bytes > kRingBytes || alignment == 0) return false;
    if (!g_state.ring_current_valid) {
        if (!allocate_ring_slot(0)) return false;
        g_state.ring_current = 0;
        g_state.ring_current_valid = true;
    }

    ring_slot_t* current = &g_state.ring[g_state.ring_current];
    size_t aligned = align_up(current->cursor, alignment);
    if (aligned != std::numeric_limits<size_t>::max() && aligned <= kRingBytes && bytes <= kRingBytes - aligned) {
        *slot_index = g_state.ring_current;
        *start = aligned;
        *pointer = current->pointer + aligned;
        current->cursor = aligned + bytes;
        return true;
    }

    if (!fence_ring_slot(g_state.ring_current)) {
        ++g_state.stats.ring_busy;
        return false;
    }

    for (size_t step = 1; step <= kRingSlots; ++step) {
        const size_t index = (g_state.ring_current + step) % kRingSlots;
        ring_slot_t& slot = g_state.ring[index];
        bool ready = false;
        if (slot.buffer == 0)
            ready = allocate_ring_slot(index);
        else
            ready = retire_ring_slot(index);
        if (!ready) continue;
        g_state.ring_current = index;
        g_state.ring_current_valid = true;
        ++g_state.stats.ring_rotations;
        aligned = align_up(slot.cursor, alignment);
        if (aligned == std::numeric_limits<size_t>::max() || aligned > kRingBytes || bytes > kRingBytes - aligned)
            continue;
        *slot_index = index;
        *start = aligned;
        *pointer = slot.pointer + aligned;
        slot.cursor = aligned + bytes;
        return true;
    }

    ++g_state.stats.ring_busy;
    return false;
}

void configure_batch_vao(const packed_layout_t& layout, GLuint buffer) {
    g_orig.bind_vertex_array(g_state.batch_vao);
    const bool rebuild = !g_state.batch_layout_valid || g_state.batch_layout_buffer != buffer ||
                         !same_layout(g_state.batch_layout, layout);
    if (!rebuild) return;

    g_orig.bind_buffer(GL_ARRAY_BUFFER, buffer);
    for (GLuint i = 0; i < kTrackedAttribs; ++i) {
        const packed_attrib_t& p = layout.attribs[i];
        if (p.enabled) {
            const void* pointer = reinterpret_cast<const void*>(static_cast<uintptr_t>(p.offset));
            if (p.integer_format)
                g_orig.vertex_attrib_i_pointer(i, p.size, p.type, static_cast<GLsizei>(layout.stride), pointer);
            else
                g_orig.vertex_attrib_pointer(i, p.size, p.type, p.normalized,
                                             static_cast<GLsizei>(layout.stride), pointer);
            if (g_orig.vertex_attrib_divisor) g_orig.vertex_attrib_divisor(i, 0);
            if (!g_state.batch_layout_valid || !g_state.batch_enabled[i])
                g_orig.enable_vertex_attrib_array(i);
            g_state.batch_enabled[i] = true;
        } else if (g_state.batch_layout_valid && g_state.batch_enabled[i]) {
            g_orig.disable_vertex_attrib_array(i);
            g_state.batch_enabled[i] = false;
        }
    }
    g_state.batch_layout = layout;
    g_state.batch_layout_buffer = buffer;
    g_state.batch_layout_valid = true;
}

void emit_batch() {
    if (!g_state.batch.active) return;
    const batch_t batch = g_state.batch;
    g_state.batch = {};
    if (!ensure_batch_vao() || batch.slot >= kRingSlots || g_state.ring[batch.slot].buffer == 0 ||
        batch.layout.stride == 0) {
        // A batch VAO failure can only occur before the first batch is activated;
        // keep this guard to avoid a malformed draw if the context is torn down.
        return;
    }

    const GLuint restore_vao = g_state.vao;
    const GLuint restore_array = g_state.array_buffer;
    g_orig.memory_barrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT);
    configure_batch_vao(batch.layout, g_state.ring[batch.slot].buffer);
    const size_t first_value = batch.start / batch.layout.stride;
    if (first_value <= static_cast<size_t>(std::numeric_limits<GLint>::max()))
        g_orig.draw_arrays(GL_TRIANGLES, static_cast<GLint>(first_value), batch.vertices);
    g_orig.bind_vertex_array(restore_vao);
    g_orig.bind_buffer(GL_ARRAY_BUFFER, restore_array);

    ++g_state.stats.batches;
    g_state.stats.batched_sources += batch.source_draws;
    if (batch.source_draws > 0) g_state.stats.eliminated += batch.source_draws - 1;
    g_state.stats.max_batch = std::max(g_state.stats.max_batch, batch.source_draws);
}

void flush_deferred() {
    if (g_state.batch.active) emit_batch();
    if (g_state.has_pending) {
        const candidate_t pending = g_state.pending;
        g_state.has_pending = false;
        replay_candidate(pending);
    }
}

bool start_pair(const candidate_t& second) {
    if (!g_state.has_pending || g_state.pending.program != second.program ||
        !same_layout(g_state.pending.layout, second.layout))
        return false;
    if (!ensure_batch_vao()) return false;

    const size_t stride = second.layout.stride;
    if (stride == 0 || stride > std::numeric_limits<size_t>::max() / 12U) return false;
    const size_t bytes = stride * 12U;
    size_t slot = 0;
    size_t start = 0;
    unsigned char* destination = nullptr;
    if (!select_ring_region(bytes, stride, &slot, &start, &destination)) return false;
    if (!pack_candidate(g_state.pending, destination) ||
        !pack_candidate(second, destination + stride * 6U))
        return false;

    g_state.stats.packed_bytes += bytes;
    g_state.batch.active = true;
    g_state.batch.slot = slot;
    g_state.batch.start = start;
    g_state.batch.vertices = 12;
    g_state.batch.source_draws = 2;
    g_state.batch.program = second.program;
    g_state.batch.layout = second.layout;
    g_state.has_pending = false;
    ++g_state.stats.pairs;
    return true;
}

bool append_batch(const candidate_t& candidate) {
    if (!g_state.batch.active || g_state.batch.program != candidate.program ||
        !same_layout(g_state.batch.layout, candidate.layout))
        return false;
    ring_slot_t& slot = g_state.ring[g_state.batch.slot];
    const size_t stride = candidate.layout.stride;
    const size_t bytes = stride * 6U;
    const size_t expected = g_state.batch.start + static_cast<size_t>(g_state.batch.vertices) * stride;
    if (g_state.batch.slot != g_state.ring_current || slot.cursor != expected ||
        expected > kRingBytes || bytes > kRingBytes - expected)
        return false;
    if (!pack_candidate(candidate, slot.pointer + expected)) return false;
    slot.cursor += bytes;
    if (g_state.batch.vertices > std::numeric_limits<GLsizei>::max() - 6) return false;
    g_state.batch.vertices += 6;
    ++g_state.batch.source_draws;
    g_state.stats.packed_bytes += bytes;
    return true;
}

void report_renderer() {
    const stats_t& s = g_state.stats;
    const double candidate_pct = s.source_draws
                                     ? 100.0 * static_cast<double>(s.candidates) / static_cast<double>(s.source_draws)
                                     : 0.0;
    const double eliminated_pct = s.source_draws
                                      ? 100.0 * static_cast<double>(s.eliminated) / static_cast<double>(s.source_draws)
                                      : 0.0;
    const double average = s.batches
                               ? static_cast<double>(s.batched_sources) / static_cast<double>(s.batches)
                               : 0.0;
    LOG_I("ZOMDROID_PZ_REPACK_RENDERER draws=%llu candidates=%llu/%.2f%% deferred=%llu pairs=%llu "
          "batches=%llu sources=%llu eliminated=%llu/%.2f%% avg_batch=%.2f max_batch=%llu replay=%llu pass=%llu "
          "packed=%lluB ring=%llu/%llu/busy%llu fences=%llu/%llu "
          "miss=%llu/%llu/%llu/%llu/%llu/%llu",
          s.source_draws, s.candidates, candidate_pct, s.deferred, s.pairs, s.batches, s.batched_sources,
          s.eliminated, eliminated_pct, average, s.max_batch, s.replays, s.passthrough, s.packed_bytes,
          s.ring_allocations, s.ring_rotations, s.ring_busy, s.fences, s.signaled,
          s.miss_program, s.miss_shape, s.miss_ebo, s.miss_topology, s.miss_vertex, s.miss_layout)
}

void maybe_report_renderer() {
    const unsigned long long n = g_state.stats.source_draws;
    if (n == 1 || n == 1024 || n == 65536 || (n != 0 && n % 250000ULL == 0)) report_renderer();
}

void renderer_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    if (!g_enabled || !g_state.worker_context) {
        g_orig.draw_elements(mode, count, type, indices);
        return;
    }

    ++g_state.stats.source_draws;
    candidate_t candidate{};
    if (!build_candidate(mode, count, type, indices, &candidate)) {
        flush_deferred();
        ++g_state.stats.passthrough;
        g_orig.draw_elements(mode, count, type, indices);
        maybe_report_renderer();
        return;
    }
    ++g_state.stats.candidates;

    if (g_state.batch.active) {
        if (!append_batch(candidate)) {
            emit_batch();
            if (!ensure_replay_vao()) {
                ++g_state.stats.passthrough;
                g_orig.draw_elements(mode, count, type, indices);
            } else {
                g_state.pending = candidate;
                g_state.has_pending = true;
                ++g_state.stats.deferred;
            }
        }
        maybe_report_renderer();
        return;
    }

    if (g_state.has_pending) {
        if (!start_pair(candidate)) {
            const candidate_t old = g_state.pending;
            g_state.has_pending = false;
            replay_candidate(old);
            if (!ensure_replay_vao()) {
                ++g_state.stats.passthrough;
                g_orig.draw_elements(mode, count, type, indices);
            } else {
                g_state.pending = candidate;
                g_state.has_pending = true;
                ++g_state.stats.deferred;
            }
        }
        maybe_report_renderer();
        return;
    }

    if (!ensure_replay_vao()) {
        ++g_state.stats.passthrough;
        g_orig.draw_elements(mode, count, type, indices);
    } else {
        g_state.pending = candidate;
        g_state.has_pending = true;
        ++g_state.stats.deferred;
    }
    maybe_report_renderer();
}

void renderer_glBindBuffer(GLenum target, GLuint buffer) {
    if (g_state.worker_context && target != GL_ARRAY_BUFFER && target != GL_ELEMENT_ARRAY_BUFFER) flush_deferred();
    if (target == GL_ARRAY_BUFFER)
        g_state.array_buffer = buffer;
    else if (target == GL_ELEMENT_ARRAY_BUFFER)
        current_vao().element_buffer = buffer;
    g_orig.bind_buffer(target, buffer);
}

void invalidate_bound_mapping(GLenum target) {
    const GLuint buffer = bound_buffer(target);
    if (buffer != 0) g_state.mappings.erase(buffer);
}

void renderer_glBufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage) {
    invalidate_bound_mapping(target);
    g_orig.buffer_data(target, size, data, usage);
}

void renderer_glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data) {
    // Driver-side subdata can be DMA-backed; stop treating an old persistent CPU
    // pointer as authoritative for that store until a later map establishes it.
    invalidate_bound_mapping(target);
    g_orig.buffer_sub_data(target, offset, size, data);
}

void renderer_glBufferStorageEXT(GLenum target, GLsizeiptr size, const void* data, GLbitfield flags) {
    invalidate_bound_mapping(target);
    g_orig.buffer_storage_ext(target, size, data, flags);
}

void* renderer_glMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access) {
    void* result = g_orig.map_buffer_range(target, offset, length, access);
    const GLuint buffer = bound_buffer(target);
    if (result != nullptr && buffer != 0 && offset >= 0 && length > 0)
        g_state.mappings[buffer] = mapping_t{static_cast<unsigned char*>(result), offset, length, access};
    return result;
}

GLboolean renderer_glUnmapBuffer(GLenum target) {
    const GLuint buffer = bound_buffer(target);
    const GLboolean result = g_orig.unmap_buffer(target);
    if (result == GL_TRUE && buffer != 0) g_state.mappings.erase(buffer);
    return result;
}

void renderer_glDeleteBuffers(GLsizei n, const GLuint* buffers) {
    g_orig.delete_buffers(n, buffers);
    if (buffers == nullptr || n <= 0) return;
    for (GLsizei i = 0; i < n; ++i) {
        const GLuint dead = buffers[i];
        g_state.mappings.erase(dead);
        if (g_state.array_buffer == dead) g_state.array_buffer = 0;
        for (auto& entry : g_state.vaos) {
            if (entry.second.element_buffer == dead) entry.second.element_buffer = 0;
            for (attrib_t& a : entry.second.attribs)
                if (a.buffer == dead) a.buffer = 0;
        }
    }
}

void renderer_glBindVertexArray(GLuint array) {
    g_state.vao = array;
    (void)current_vao();
    g_orig.bind_vertex_array(array);
}

void renderer_glDeleteVertexArrays(GLsizei n, const GLuint* arrays) {
    g_orig.delete_vertex_arrays(n, arrays);
    if (arrays == nullptr || n <= 0) return;
    for (GLsizei i = 0; i < n; ++i) {
        g_state.vaos.erase(arrays[i]);
        if (g_state.vao == arrays[i]) g_state.vao = 0;
    }
}

void mark_untracked_vertex_model(GLuint index) {
    if (index >= kTrackedAttribs) current_vao().unsupported_vertex_model = true;
}

void renderer_glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride,
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
    } else {
        mark_untracked_vertex_model(index);
    }
    g_orig.vertex_attrib_pointer(index, size, type, normalized, stride, pointer);
}

void renderer_glVertexAttribIPointer(GLuint index, GLint size, GLenum type, GLsizei stride, const void* pointer) {
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
    } else {
        mark_untracked_vertex_model(index);
    }
    g_orig.vertex_attrib_i_pointer(index, size, type, stride, pointer);
}

void renderer_glEnableVertexAttribArray(GLuint index) {
    if (index < kTrackedAttribs)
        current_vao().attribs[index].enabled = true;
    else
        mark_untracked_vertex_model(index);
    g_orig.enable_vertex_attrib_array(index);
}

void renderer_glDisableVertexAttribArray(GLuint index) {
    if (index < kTrackedAttribs)
        current_vao().attribs[index].enabled = false;
    else
        mark_untracked_vertex_model(index);
    g_orig.disable_vertex_attrib_array(index);
}

void renderer_glVertexAttribDivisor(GLuint index, GLuint divisor) {
    if (index < kTrackedAttribs)
        current_vao().attribs[index].divisor = divisor;
    else
        mark_untracked_vertex_model(index);
    g_orig.vertex_attrib_divisor(index, divisor);
}

void renderer_glBindVertexBuffer(GLuint bindingindex, GLuint buffer, GLintptr offset, GLsizei stride) {
    current_vao().unsupported_vertex_model = true;
    g_orig.bind_vertex_buffer(bindingindex, buffer, offset, stride);
}

void renderer_glVertexAttribFormat(GLuint attribindex, GLint size, GLenum type, GLboolean normalized,
                                   GLuint relativeoffset) {
    current_vao().unsupported_vertex_model = true;
    g_orig.vertex_attrib_format(attribindex, size, type, normalized, relativeoffset);
}

void renderer_glVertexAttribIFormat(GLuint attribindex, GLint size, GLenum type, GLuint relativeoffset) {
    current_vao().unsupported_vertex_model = true;
    g_orig.vertex_attrib_i_format(attribindex, size, type, relativeoffset);
}

void renderer_glVertexAttribBinding(GLuint attribindex, GLuint bindingindex) {
    current_vao().unsupported_vertex_model = true;
    g_orig.vertex_attrib_binding(attribindex, bindingindex);
}

void renderer_glVertexBindingDivisor(GLuint bindingindex, GLuint divisor) {
    current_vao().unsupported_vertex_model = true;
    g_orig.vertex_binding_divisor(bindingindex, divisor);
}

void renderer_glUseProgram(GLuint program) {
    g_state.program = program;
    g_orig.use_program(program);
}

void renderer_glShaderSource(GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length) {
    bool known = false;
    const bool sensitive = source_has_identity_builtin(count, string, length, &known);
    g_state.shaders[shader] = shader_t{known, sensitive};
    g_orig.shader_source(shader, count, string, length);
}

void renderer_glAttachShader(GLuint program, GLuint shader) {
    program_t& p = g_state.programs[program];
    if (std::find(p.attached.begin(), p.attached.end(), shader) == p.attached.end()) p.attached.push_back(shader);
    g_orig.attach_shader(program, shader);
}

void renderer_glDetachShader(GLuint program, GLuint shader) {
    auto found = g_state.programs.find(program);
    if (found != g_state.programs.end()) {
        auto& attached = found->second.attached;
        attached.erase(std::remove(attached.begin(), attached.end(), shader), attached.end());
    }
    g_orig.detach_shader(program, shader);
}

void renderer_glLinkProgram(GLuint program) {
    g_orig.link_program(program);
    program_t& p = g_state.programs[program];
    bool known = !p.attached.empty();
    bool safe = known;
    for (GLuint shader : p.attached) {
        const auto found = g_state.shaders.find(shader);
        if (found == g_state.shaders.end() || !found->second.source_known) {
            known = false;
            safe = false;
            break;
        }
        if (found->second.vertex_identity_sensitive) safe = false;
    }
    p.linked_known = known;
    p.linked_safe = known && safe;
}

void renderer_glDeleteProgram(GLuint program) {
    g_orig.delete_program(program);
    g_state.programs.erase(program);
}

void cleanup_worker_context() {
    flush_deferred();
    if (!g_state.worker_context) return;
    for (ring_slot_t& slot : g_state.ring) {
        if (slot.fence && g_orig.delete_sync) g_orig.delete_sync(slot.fence);
        slot.fence = nullptr;
        if (slot.buffer && g_orig.delete_buffers) g_orig.delete_buffers(1, &slot.buffer);
        slot = {};
    }
    if (g_state.replay_vao && g_orig.delete_vertex_arrays) g_orig.delete_vertex_arrays(1, &g_state.replay_vao);
    if (g_state.batch_vao && g_orig.delete_vertex_arrays) g_orig.delete_vertex_arrays(1, &g_state.batch_vao);
    const stats_t saved_stats = g_state.stats;
    g_state = {};
    g_state.stats = saved_stats;
}

template <typename Pointer, typename Slot> Pointer capture(const Slot& slot) {
    return static_cast<Pointer>(slot);
}

} // namespace

namespace mg_ts {
void pz_repack_before_backend_command(backend_command_class classification) {
    if (!g_enabled) return;
    if (classification == backend_command_class::context_adopt) {
        // New current context: start from no assumptions about driver object names.
        const stats_t saved_stats = g_state.stats;
        g_state = {};
        g_state.stats = saved_stats;
        g_state.worker_context = true;
        return;
    }
    if (classification == backend_command_class::context_release) {
        cleanup_worker_context();
        return;
    }
    g_state.worker_context = true;
    if (classification == backend_command_class::barrier) flush_deferred();
}
} // namespace mg_ts

void mg_pz_repack_renderer_install(void) {
    const char* value = std::getenv("MOBILEGLUES_PZ_REPACK_RENDERER");
    if (value == nullptr || std::strcmp(value, "1") != 0) return;
    if (!mg_pz_threaded_submission_active) {
        LOG_W_FORCE("ZOMDROID_PZ_REPACK_RENDERER disabled reason=threaded_submission_required")
        return;
    }

    g_orig.draw_elements = capture<glDrawElements_PTR>(GLES.glDrawElements);
    g_orig.draw_arrays = capture<glDrawArrays_PTR>(GLES.glDrawArrays);
    g_orig.bind_buffer = capture<glBindBuffer_PTR>(GLES.glBindBuffer);
    g_orig.buffer_data = capture<glBufferData_PTR>(GLES.glBufferData);
    g_orig.buffer_sub_data = capture<glBufferSubData_PTR>(GLES.glBufferSubData);
    g_orig.buffer_storage_ext = capture<glBufferStorageEXT_PTR>(GLES.glBufferStorageEXT);
    g_orig.map_buffer_range = capture<glMapBufferRange_PTR>(GLES.glMapBufferRange);
    g_orig.unmap_buffer = capture<glUnmapBuffer_PTR>(GLES.glUnmapBuffer);
    g_orig.delete_buffers = capture<glDeleteBuffers_PTR>(GLES.glDeleteBuffers);
    g_orig.gen_buffers = capture<glGenBuffers_PTR>(GLES.glGenBuffers);
    g_orig.memory_barrier = capture<glMemoryBarrier_PTR>(GLES.glMemoryBarrier);
    g_orig.fence_sync = capture<glFenceSync_PTR>(GLES.glFenceSync);
    g_orig.client_wait_sync = capture<glClientWaitSync_PTR>(GLES.glClientWaitSync);
    g_orig.delete_sync = capture<glDeleteSync_PTR>(GLES.glDeleteSync);

    g_orig.bind_vertex_array = capture<glBindVertexArray_PTR>(GLES.glBindVertexArray);
    g_orig.gen_vertex_arrays = capture<glGenVertexArrays_PTR>(GLES.glGenVertexArrays);
    g_orig.delete_vertex_arrays = capture<glDeleteVertexArrays_PTR>(GLES.glDeleteVertexArrays);
    g_orig.vertex_attrib_pointer = capture<glVertexAttribPointer_PTR>(GLES.glVertexAttribPointer);
    g_orig.vertex_attrib_i_pointer = capture<glVertexAttribIPointer_PTR>(GLES.glVertexAttribIPointer);
    g_orig.enable_vertex_attrib_array = capture<glEnableVertexAttribArray_PTR>(GLES.glEnableVertexAttribArray);
    g_orig.disable_vertex_attrib_array = capture<glDisableVertexAttribArray_PTR>(GLES.glDisableVertexAttribArray);
    g_orig.vertex_attrib_divisor = capture<glVertexAttribDivisor_PTR>(GLES.glVertexAttribDivisor);
    g_orig.bind_vertex_buffer = capture<glBindVertexBuffer_PTR>(GLES.glBindVertexBuffer);
    g_orig.vertex_attrib_format = capture<glVertexAttribFormat_PTR>(GLES.glVertexAttribFormat);
    g_orig.vertex_attrib_i_format = capture<glVertexAttribIFormat_PTR>(GLES.glVertexAttribIFormat);
    g_orig.vertex_attrib_binding = capture<glVertexAttribBinding_PTR>(GLES.glVertexAttribBinding);
    g_orig.vertex_binding_divisor = capture<glVertexBindingDivisor_PTR>(GLES.glVertexBindingDivisor);

    g_orig.use_program = capture<glUseProgram_PTR>(GLES.glUseProgram);
    g_orig.shader_source = capture<glShaderSource_PTR>(GLES.glShaderSource);
    g_orig.attach_shader = capture<glAttachShader_PTR>(GLES.glAttachShader);
    g_orig.detach_shader = capture<glDetachShader_PTR>(GLES.glDetachShader);
    g_orig.link_program = capture<glLinkProgram_PTR>(GLES.glLinkProgram);
    g_orig.delete_program = capture<glDeleteProgram_PTR>(GLES.glDeleteProgram);

    if (!g_orig.draw_elements || !g_orig.draw_arrays || !g_orig.bind_buffer || !g_orig.buffer_data ||
        !g_orig.buffer_sub_data || !g_orig.buffer_storage_ext || !g_orig.map_buffer_range || !g_orig.unmap_buffer ||
        !g_orig.delete_buffers || !g_orig.gen_buffers || !g_orig.memory_barrier || !g_orig.fence_sync ||
        !g_orig.client_wait_sync || !g_orig.delete_sync || !g_orig.bind_vertex_array || !g_orig.gen_vertex_arrays ||
        !g_orig.delete_vertex_arrays || !g_orig.vertex_attrib_pointer || !g_orig.vertex_attrib_i_pointer ||
        !g_orig.enable_vertex_attrib_array || !g_orig.disable_vertex_attrib_array || !g_orig.vertex_attrib_divisor ||
        !g_orig.use_program || !g_orig.shader_source || !g_orig.attach_shader || !g_orig.detach_shader ||
        !g_orig.link_program || !g_orig.delete_program) {
        LOG_W_FORCE("ZOMDROID_PZ_REPACK_RENDERER disabled reason=backend_capability_missing")
        return;
    }

    GLES.glDrawElements = renderer_glDrawElements;
    GLES.glBindBuffer = renderer_glBindBuffer;
    GLES.glBufferData = renderer_glBufferData;
    GLES.glBufferSubData = renderer_glBufferSubData;
    GLES.glBufferStorageEXT = renderer_glBufferStorageEXT;
    GLES.glMapBufferRange = renderer_glMapBufferRange;
    GLES.glUnmapBuffer = renderer_glUnmapBuffer;
    GLES.glDeleteBuffers = renderer_glDeleteBuffers;
    GLES.glBindVertexArray = renderer_glBindVertexArray;
    GLES.glDeleteVertexArrays = renderer_glDeleteVertexArrays;
    GLES.glVertexAttribPointer = renderer_glVertexAttribPointer;
    GLES.glVertexAttribIPointer = renderer_glVertexAttribIPointer;
    GLES.glEnableVertexAttribArray = renderer_glEnableVertexAttribArray;
    GLES.glDisableVertexAttribArray = renderer_glDisableVertexAttribArray;
    GLES.glVertexAttribDivisor = renderer_glVertexAttribDivisor;
    if (g_orig.bind_vertex_buffer) GLES.glBindVertexBuffer = renderer_glBindVertexBuffer;
    if (g_orig.vertex_attrib_format) GLES.glVertexAttribFormat = renderer_glVertexAttribFormat;
    if (g_orig.vertex_attrib_i_format) GLES.glVertexAttribIFormat = renderer_glVertexAttribIFormat;
    if (g_orig.vertex_attrib_binding) GLES.glVertexAttribBinding = renderer_glVertexAttribBinding;
    if (g_orig.vertex_binding_divisor) GLES.glVertexBindingDivisor = renderer_glVertexBindingDivisor;
    GLES.glUseProgram = renderer_glUseProgram;
    GLES.glShaderSource = renderer_glShaderSource;
    GLES.glAttachShader = renderer_glAttachShader;
    GLES.glDetachShader = renderer_glDetachShader;
    GLES.glLinkProgram = renderer_glLinkProgram;
    GLES.glDeleteProgram = renderer_glDeleteProgram;

    g_enabled = true;
    LOG_I("ZOMDROID_PZ_REPACK_RENDERER enabled=1 mode=quad6_u16_deindex persistent_ring=4x8MiB "
          "barriers=all_non_geometry shader_identity_guard=1 quad_index_cache=compatible requires=threaded_submission")
}

#else

void mg_pz_repack_renderer_install(void) {}

#endif
