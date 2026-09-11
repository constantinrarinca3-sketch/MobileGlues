// MobileGlues - gl/pz_material_stream_renderer.cpp
// Project Zomboid experiment V4.3: restore the backend material-stream shadow
// from the actual worker-context state after context adoption. V4.2 stays
// verbatim as the implementation base; this layer only rehydrates current
// program/VAO/attributes and already-existing persistent coherent mappings.

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
#include <vector>

#if !defined(ZOMDROID_EXPERIMENTAL)
#include "pz_material_stream_renderer_v42_base.cpp"
#else

namespace v43_base {
namespace mg_ts {
using backend_command_class = ::mg_ts::backend_command_class;
}
#include "pz_material_stream_renderer_v42_base.cpp"
} // namespace v43_base

namespace v43_base {
namespace legacy_v41 {
namespace v41_base {
namespace {

struct v43_resync_stats_t {
    unsigned long long attempts = 0;
    unsigned long long adopt_attempts = 0;
    unsigned long long lazy_attempts = 0;
    unsigned long long mapping_hits = 0;
    unsigned long long mapping_misses = 0;
};

thread_local v43_resync_stats_t g_v43_resync_stats;
thread_local unsigned long long g_v43_last_lazy_draw = 0;
thread_local bool g_v43_lazy_attempted = false;

bool v43_query_attribute(GLuint index, attrib_t* output) {
    if (!output || !GLES.glGetVertexAttribiv || !GLES.glGetVertexAttribPointerv) return false;

    GLint enabled = GL_FALSE;
    GLint size = 0;
    GLint stride = 0;
    GLint type = 0;
    GLint normalized = GL_FALSE;
    GLint buffer = 0;
    GLint divisor = 0;
    GLint integer_format = GL_FALSE;
    void* pointer = nullptr;

    GLES.glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled);
    GLES.glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_SIZE, &size);
    GLES.glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &stride);
    GLES.glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_TYPE, &type);
    GLES.glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &normalized);
    GLES.glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &buffer);
    GLES.glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_DIVISOR, &divisor);
    GLES.glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_INTEGER, &integer_format);
    GLES.glGetVertexAttribPointerv(index, GL_VERTEX_ATTRIB_ARRAY_POINTER, &pointer);

    attrib_t attribute{};
    attribute.enabled = enabled == GL_TRUE;
    attribute.described = size >= 1 && size <= 4 && type != 0 && buffer > 0 && stride >= 0;
    attribute.integer_format = integer_format == GL_TRUE;
    attribute.size = size;
    attribute.type = static_cast<GLenum>(type);
    attribute.normalized = normalized == GL_TRUE ? GL_TRUE : GL_FALSE;
    attribute.stride = static_cast<GLsizei>(std::max(0, stride));
    attribute.pointer = reinterpret_cast<uintptr_t>(pointer);
    attribute.buffer = buffer > 0 ? static_cast<GLuint>(buffer) : 0;
    attribute.divisor = divisor > 0 ? static_cast<GLuint>(divisor) : 0;
    *output = attribute;
    return true;
}

bool v43_rehydrate_mapping(GLuint buffer) {
    if (buffer == 0) return false;
    const mapping_t* known = nullptr;
    if (safe_mapping(buffer, &known)) return true;
    if (!g_orig.bind_buffer || !GLES.glGetBufferParameteriv || !GLES.glGetBufferParameteri64v ||
        !GLES.glGetBufferPointerv || !g_material_backend.get_integerv)
        return false;

    GLint restore = 0;
    g_material_backend.get_integerv(GL_COPY_READ_BUFFER_BINDING, &restore);
    g_orig.bind_buffer(GL_COPY_READ_BUFFER, buffer);

    GLint mapped = GL_FALSE;
    GLint access = 0;
    GLint64 size = 0;
    GLint64 map_offset = 0;
    GLint64 map_length = 0;
    void* pointer = nullptr;
    GLES.glGetBufferParameteriv(GL_COPY_READ_BUFFER, GL_BUFFER_MAPPED, &mapped);
    GLES.glGetBufferParameteriv(GL_COPY_READ_BUFFER, GL_BUFFER_ACCESS_FLAGS, &access);
    GLES.glGetBufferParameteri64v(GL_COPY_READ_BUFFER, GL_BUFFER_SIZE, &size);
    GLES.glGetBufferParameteri64v(GL_COPY_READ_BUFFER, GL_BUFFER_MAP_OFFSET, &map_offset);
    GLES.glGetBufferParameteri64v(GL_COPY_READ_BUFFER, GL_BUFFER_MAP_LENGTH, &map_length);
    GLES.glGetBufferPointerv(GL_COPY_READ_BUFFER, GL_BUFFER_MAP_POINTER, &pointer);

    g_orig.bind_buffer(GL_COPY_READ_BUFFER, restore > 0 ? static_cast<GLuint>(restore) : 0);

    const GLbitfield required = GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    const bool range_ok = size > 0 && map_offset >= 0 && map_length > 0 && map_offset <= size && map_length <= size - map_offset &&
                          map_offset <= static_cast<GLint64>(std::numeric_limits<GLintptr>::max()) &&
                          map_length <= static_cast<GLint64>(std::numeric_limits<GLsizeiptr>::max());
    if (mapped != GL_TRUE || pointer == nullptr || (static_cast<GLbitfield>(access) & required) != required || !range_ok) {
        ++g_v43_resync_stats.mapping_misses;
        return false;
    }

    g_state.mappings[buffer] = mapping_t{static_cast<unsigned char*>(pointer), static_cast<GLintptr>(map_offset),
                                         static_cast<GLsizeiptr>(map_length), static_cast<GLbitfield>(access)};
    ++g_v43_resync_stats.mapping_hits;
    return true;
}

unsigned v43_rehydrate_required_mappings(const vao_t& vao) {
    unsigned hits = 0;
    if (vao.element_buffer != 0 && v43_rehydrate_mapping(vao.element_buffer)) ++hits;
    std::array<GLuint, 3> seen{};
    size_t seen_count = 0;
    for (GLuint index = 0; index < 3; ++index) {
        const attrib_t& attribute = vao.attribs[index];
        if (!attribute.enabled || !attribute.described || attribute.buffer == 0) continue;
        bool duplicate = false;
        for (size_t i = 0; i < seen_count; ++i)
            if (seen[i] == attribute.buffer) duplicate = true;
        if (duplicate) continue;
        seen[seen_count++] = attribute.buffer;
        if (v43_rehydrate_mapping(attribute.buffer)) ++hits;
    }
    return hits;
}

bool v43_resync_driver_state(const char* reason) {
    ++g_v43_resync_stats.attempts;
    if (reason && std::strcmp(reason, "adopt") == 0)
        ++g_v43_resync_stats.adopt_attempts;
    else
        ++g_v43_resync_stats.lazy_attempts;

    if (!g_material_backend.get_integerv || !GLES.glGetVertexAttribiv || !GLES.glGetVertexAttribPointerv) return false;

    GLint program = 0;
    GLint vao = 0;
    GLint array_buffer = 0;
    GLint element_buffer = 0;
    GLint active_texture = GL_TEXTURE0;
    g_material_backend.get_integerv(GL_CURRENT_PROGRAM, &program);
    g_material_backend.get_integerv(GL_VERTEX_ARRAY_BINDING, &vao);
    g_material_backend.get_integerv(GL_ARRAY_BUFFER_BINDING, &array_buffer);
    g_material_backend.get_integerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &element_buffer);
    g_material_backend.get_integerv(GL_ACTIVE_TEXTURE, &active_texture);

    g_state.program = program > 0 ? static_cast<GLuint>(program) : 0;
    g_state.vao = vao > 0 ? static_cast<GLuint>(vao) : 0;
    g_state.array_buffer = array_buffer > 0 ? static_cast<GLuint>(array_buffer) : 0;
    if (active_texture >= static_cast<GLint>(GL_TEXTURE0)) g_material.active_texture = static_cast<GLenum>(active_texture);

    vao_t& state = current_vao();
    state.element_buffer = element_buffer > 0 ? static_cast<GLuint>(element_buffer) : 0;

    unsigned attributes = 0;
    for (GLuint index = 0; index < 3; ++index) {
        attrib_t queried{};
        if (v43_query_attribute(index, &queried)) {
            state.attribs[index] = queried;
            if (queried.enabled && queried.described) ++attributes;
        }
    }
    const unsigned mappings = v43_rehydrate_required_mappings(state);
    const bool ready = g_state.program != 0 && state.element_buffer != 0 && attributes == 3 && mappings >= 1;

    const unsigned long long attempt = g_v43_resync_stats.attempts;
    if (attempt <= 8 || attempt == 1024 || attempt == 65536) {
        LOG_I("ZOMDROID_PZ_MATERIAL_STREAM_V43_RESYNC attempt=%llu reason=%s program=%u vao=%u ebo=%u attrs=%u "
              "maps=%u map_hit=%llu map_miss=%llu ready=%d",
              attempt, reason ? reason : "unknown", g_state.program, g_state.vao, state.element_buffer, attributes,
              mappings, g_v43_resync_stats.mapping_hits, g_v43_resync_stats.mapping_misses, ready ? 1 : 0)
    }
    return ready;
}

bool v43_candidate_shadow_incomplete() {
    if (g_state.program == 0) return true;
    const vao_t& vao = current_vao();
    if (vao.element_buffer == 0) return true;
    const mapping_t* mapping = nullptr;
    if (!safe_mapping(vao.element_buffer, &mapping)) return true;
    for (GLuint index = 0; index < 3; ++index) {
        const attrib_t& attribute = vao.attribs[index];
        if (!attribute.enabled || !attribute.described || attribute.buffer == 0 || attribute.divisor != 0) return true;
        if (!safe_mapping(attribute.buffer, &mapping)) return true;
    }
    return false;
}

void v43_lazy_resync_if_needed() {
    if (g_state.program == 0 && g_material_backend.get_integerv) {
        GLint program = 0;
        g_material_backend.get_integerv(GL_CURRENT_PROGRAM, &program);
        if (program > 0) g_state.program = static_cast<GLuint>(program);
    }

    material_program_t* program = v42_ensure_program(g_state.program);
    if (!program || !program->compatible || !v43_candidate_shadow_incomplete()) return;

    const unsigned long long seen = g_material.stats.draws_seen;
    if (g_v43_lazy_attempted && seen >= g_v43_last_lazy_draw && seen - g_v43_last_lazy_draw < 1024ULL) return;
    g_v43_lazy_attempted = true;
    g_v43_last_lazy_draw = seen;
    (void)v43_resync_driver_state("lazy");
}

void v43_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    v43_lazy_resync_if_needed();
    v42_glDrawElements(mode, count, type, indices);
}

void v43_install_impl() {
    ::v43_base::mg_pz_repack_renderer_install();
    if (!g_v41_enabled || !g_material_enabled || !g_enabled) return;
    GLES.glDrawElements = v43_glDrawElements;
    LOG_I("ZOMDROID_PZ_MATERIAL_STREAM_V43 enabled=1 revision=4.3 resync=driver_state+persistent_map "
          "candidate=v42 goal=capture stable_untouched=1")
}

void v43_before_command_impl(::mg_ts::backend_command_class classification) {
    ::v43_base::mg_ts::pz_repack_before_backend_command(classification);
    if (!g_v41_enabled) return;
    if (classification == ::mg_ts::backend_command_class::context_adopt) {
        g_v43_lazy_attempted = false;
        g_v43_last_lazy_draw = g_material.stats.draws_seen;
        (void)v43_resync_driver_state("adopt");
    }
}

} // namespace

void install_v43_internal() { v43_install_impl(); }
void before_v43_internal(::mg_ts::backend_command_class classification) { v43_before_command_impl(classification); }

} // namespace v41_base
} // namespace legacy_v41
} // namespace v43_base

namespace mg_ts {
void pz_repack_before_backend_command(backend_command_class classification) {
    v43_base::legacy_v41::v41_base::before_v43_internal(classification);
}
} // namespace mg_ts

void mg_pz_repack_renderer_install(void) {
    v43_base::legacy_v41::v41_base::install_v43_internal();
}

#endif // ZOMDROID_EXPERIMENTAL
