// MobileGlues - gl/pz_material_stream_renderer.cpp
// Project Zomboid experiment V4.1: make the material-stream path independent
// from the legacy V2 program-safety gate. The V4 implementation is retained as
// an implementation base in this translation unit; V4.1 owns program/source
// registry, lazy stream classification and the material-only candidate builder.

#include "pz_repack_renderer.h"
#include "threaded_submission.h"
#include "pz_census.h"
#include "../gles/loader.h"
#include "log.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <regex>
#include <string>
#include <unordered_map>
#include <vector>

namespace v41_base {

// pz_material_stream_renderer_v4_base.cpp is compiled only through this file.
// Its mg_ts callback lives in this private namespace; the real global callback
// at the bottom forwards into it after adding V4.1 lifetime handling.
namespace mg_ts {
using backend_command_class = ::mg_ts::backend_command_class;
}

#include "pz_material_stream_renderer_v4_base.cpp"

#if defined(ZOMDROID_EXPERIMENTAL)

namespace {

enum class v41_reject_t : uint8_t {
    none = 0,
    program_registry,
    program_link,
    identity,
    vertex_family,
    fragment_family,
    compiler,
    uniform_locations,
    uniform_state,
    shape,
    ebo,
    topology,
    vertex_mapping,
    texture_state,
    texture_array,
    stream_objects,
    count,
};

constexpr size_t kV41RejectCount = static_cast<size_t>(v41_reject_t::count);

struct v41_shader_record_t {
    GLenum type = 0;
    std::string source;
    bool source_known = false;
    bool identity_safe = false;
};

struct v41_program_record_t {
    unsigned long long generation = 0;
    bool linked = false;
    bool complete = false;
    bool identity_safe = false;
    std::string vertex;
    std::string fragment;
};

struct v41_registry_t {
    std::mutex mutex;
    unsigned long long next_generation = 1;
    std::unordered_map<GLuint, v41_shader_record_t> shaders;
    std::unordered_map<GLuint, std::vector<GLuint>> attached;
    std::unordered_map<GLuint, v41_program_record_t> programs;
};

struct v41_local_program_t {
    unsigned long long generation = 0;
    v41_reject_t reject = v41_reject_t::program_registry;
};

struct v41_stats_t {
    std::array<unsigned long long, kV41RejectCount> rejects{};
    unsigned long long classified_ok = 0;
};

struct v41_wrapped_t {
    glAttachShader_PTR attach_shader = nullptr;
    glDetachShader_PTR detach_shader = nullptr;
    glUniform1fv_PTR uniform1fv = nullptr;
    glUniform1iv_PTR uniform1iv = nullptr;
    glProgramUniform1i_PTR program_uniform1i = nullptr;
    glProgramUniform1iv_PTR program_uniform1iv = nullptr;
    glProgramUniform1f_PTR program_uniform1f = nullptr;
    glProgramUniform1fv_PTR program_uniform1fv = nullptr;
    glProgramUniformMatrix4fv_PTR program_uniform_matrix4fv = nullptr;
    glGetUniformfv_PTR get_uniformfv = nullptr;
    glGetUniformiv_PTR get_uniformiv = nullptr;
    glBindSampler_PTR bind_sampler = nullptr;
};

v41_registry_t g_v41_registry;
v41_wrapped_t g_v41_wrapped;
bool g_v41_enabled = false;
thread_local std::unordered_map<GLuint, v41_local_program_t> g_v41_local_programs;
thread_local std::vector<GLuint> g_v41_bound_samplers;
thread_local std::vector<unsigned char> g_v41_sampler_known;
thread_local v41_stats_t g_v41_stats;

bool v41_identity_safe(const std::string& source) {
    static const char* sensitive[] = {
        "gl_VertexID", "gl_InstanceID", "gl_PrimitiveID", "gl_DrawID", "gl_BaseVertex", "gl_BaseInstance",
    };
    for (const char* token : sensitive)
        if (source.find(token) != std::string::npos) return false;
    return true;
}

void v41_registry_shader_source(GLuint shader, GLenum type, const std::string* source) {
    std::lock_guard<std::mutex> lock(g_v41_registry.mutex);
    v41_shader_record_t record{};
    record.type = type;
    if (source != nullptr && !source->empty()) {
        record.source = *source;
        record.source_known = true;
        record.identity_safe = v41_identity_safe(record.source);
    }
    g_v41_registry.shaders[shader] = std::move(record);
}

void v41_registry_attach(GLuint program, GLuint shader) {
    std::lock_guard<std::mutex> lock(g_v41_registry.mutex);
    auto& attached = g_v41_registry.attached[program];
    if (std::find(attached.begin(), attached.end(), shader) == attached.end()) attached.push_back(shader);
    g_v41_registry.programs.erase(program);
}

void v41_registry_detach(GLuint program, GLuint shader) {
    std::lock_guard<std::mutex> lock(g_v41_registry.mutex);
    auto found = g_v41_registry.attached.find(program);
    if (found != g_v41_registry.attached.end()) {
        auto& attached = found->second;
        attached.erase(std::remove(attached.begin(), attached.end(), shader), attached.end());
    }
    g_v41_registry.programs.erase(program);
}

void v41_registry_link(GLuint program, bool linked) {
    std::lock_guard<std::mutex> lock(g_v41_registry.mutex);
    v41_program_record_t record{};
    record.generation = g_v41_registry.next_generation++;
    record.linked = linked;
    record.identity_safe = true;

    const auto attached = g_v41_registry.attached.find(program);
    if (linked && attached != g_v41_registry.attached.end() && attached->second.size() == 2) {
        bool have_vertex = false;
        bool have_fragment = false;
        bool complete = true;
        for (GLuint shader : attached->second) {
            const auto source = g_v41_registry.shaders.find(shader);
            if (source == g_v41_registry.shaders.end() || !source->second.source_known) {
                complete = false;
                break;
            }
            record.identity_safe = record.identity_safe && source->second.identity_safe;
            if (source->second.type == GL_VERTEX_SHADER && !have_vertex) {
                record.vertex = source->second.source;
                have_vertex = true;
            } else if (source->second.type == GL_FRAGMENT_SHADER && !have_fragment) {
                record.fragment = source->second.source;
                have_fragment = true;
            } else {
                complete = false;
                break;
            }
        }
        record.complete = complete && have_vertex && have_fragment;
    }
    g_v41_registry.programs[program] = std::move(record);
}

void v41_registry_delete_program(GLuint program) {
    std::lock_guard<std::mutex> lock(g_v41_registry.mutex);
    g_v41_registry.attached.erase(program);
    g_v41_registry.programs.erase(program);
}

bool v41_registry_program(GLuint program, v41_program_record_t* output) {
    if (output == nullptr) return false;
    std::lock_guard<std::mutex> lock(g_v41_registry.mutex);
    const auto found = g_v41_registry.programs.find(program);
    if (found == g_v41_registry.programs.end()) return false;
    *output = found->second;
    return true;
}

void v41_registry_counts(unsigned long long* shaders, unsigned long long* programs) {
    std::lock_guard<std::mutex> lock(g_v41_registry.mutex);
    if (shaders) *shaders = static_cast<unsigned long long>(g_v41_registry.shaders.size());
    if (programs) *programs = static_cast<unsigned long long>(g_v41_registry.programs.size());
}

void v41_set_local_failure(GLuint program, unsigned long long generation, v41_reject_t reason) {
    material_program_t info{};
    info.classified = true;
    info.compatible = false;
    g_material.programs[program] = info;
    g_v41_local_programs[program] = v41_local_program_t{generation, reason};
}

material_program_t* v41_find_local_program(GLuint program) {
    const auto local = g_v41_local_programs.find(program);
    if (local == g_v41_local_programs.end()) return nullptr;
    const auto found = g_material.programs.find(program);
    return found == g_material.programs.end() ? nullptr : &found->second;
}

v41_reject_t v41_program_reject(GLuint program) {
    const auto found = g_v41_local_programs.find(program);
    return found == g_v41_local_programs.end() ? v41_reject_t::program_registry : found->second.reject;
}

material_program_t* v41_ensure_program(GLuint program) {
    if (program == 0) return nullptr;
    if (material_program_t* existing = v41_find_local_program(program)) return existing;

    v41_program_record_t source{};
    if (!v41_registry_program(program, &source)) {
        v41_set_local_failure(program, 0, v41_reject_t::program_registry);
        return v41_find_local_program(program);
    }
    if (!source.linked) {
        v41_set_local_failure(program, source.generation, v41_reject_t::program_link);
        return v41_find_local_program(program);
    }
    if (!source.complete) {
        v41_set_local_failure(program, source.generation, v41_reject_t::program_registry);
        return v41_find_local_program(program);
    }
    if (!source.identity_safe) {
        v41_set_local_failure(program, source.generation, v41_reject_t::identity);
        return v41_find_local_program(program);
    }
    if (!ensure_limits()) {
        v41_set_local_failure(program, source.generation, v41_reject_t::uniform_state);
        return v41_find_local_program(program);
    }

    std::string stream_vertex;
    if (!transform_vertex_source(source.vertex, static_cast<GLint>(g_material.ssbo_binding), &stream_vertex)) {
        v41_set_local_failure(program, source.generation, v41_reject_t::vertex_family);
        return v41_find_local_program(program);
    }
    std::string stream_fragment;
    if (!transform_fragment_source(source.fragment, &stream_fragment)) {
        v41_set_local_failure(program, source.generation, v41_reject_t::fragment_family);
        return v41_find_local_program(program);
    }

    material_program_t info{};
    info.classified = true;
    info.stream_program = stream_variant_for(stream_vertex, stream_fragment);
    if (!info.stream_program) {
        v41_set_local_failure(program, source.generation, v41_reject_t::compiler);
        return v41_find_local_program(program);
    }

    info.mvp = g_material_backend.get_uniform_location(program, "ModelViewProjection");
    info.z_depth = g_material_backend.get_uniform_location(program, "zDepth");
    info.chunk_depth = g_material_backend.get_uniform_location(program, "chunkDepth");
    info.diffuse = g_material_backend.get_uniform_location(program, "DIFFUSE");
    info.use_texture = g_material_backend.get_uniform_location(program, "useTexture");
    if (info.mvp < 0 || info.z_depth < 0 || info.chunk_depth < 0 || info.diffuse < 0 || info.use_texture < 0) {
        v41_set_local_failure(program, source.generation, v41_reject_t::uniform_locations);
        return v41_find_local_program(program);
    }
    if (!g_v41_wrapped.get_uniformfv || !g_v41_wrapped.get_uniformiv) {
        v41_set_local_failure(program, source.generation, v41_reject_t::uniform_state);
        return v41_find_local_program(program);
    }

    GLfloat scalar = 0.0f;
    GLint integer = 0;
    g_v41_wrapped.get_uniformfv(program, info.mvp, info.mvp_value.data());
    g_v41_wrapped.get_uniformfv(program, info.z_depth, &scalar);
    info.z_depth_value = scalar;
    g_v41_wrapped.get_uniformfv(program, info.chunk_depth, &scalar);
    info.chunk_depth_value = scalar;
    g_v41_wrapped.get_uniformiv(program, info.diffuse, &integer);
    info.diffuse_unit = integer;
    g_v41_wrapped.get_uniformiv(program, info.use_texture, &integer);
    info.use_texture_value = integer;
    info.compatible = true;

    g_material.programs[program] = info;
    g_v41_local_programs[program] = v41_local_program_t{source.generation, v41_reject_t::none};
    ++g_v41_stats.classified_ok;
    return &g_material.programs[program];
}

bool v41_attribute_type_supported(GLenum type) {
    switch (type) {
    case GL_FLOAT:
    case GL_HALF_FLOAT:
    case GL_UNSIGNED_BYTE:
    case GL_BYTE:
    case GL_UNSIGNED_SHORT:
    case GL_SHORT:
        return true;
    default:
        return false;
    }
}

bool v41_validate_attribute(const attrib_t& attribute, unsigned components,
                            const std::array<uint16_t, 6>& indices) {
    if (!attribute.enabled || !attribute.described || attribute.integer_format || attribute.buffer == 0 ||
        attribute.divisor != 0 || attribute.size != static_cast<GLint>(components) || attribute.stride < 0 ||
        !v41_attribute_type_supported(attribute.type))
        return false;
    const unsigned scalar = scalar_size(attribute.type);
    if (scalar == 0) return false;
    const uint64_t element_bytes = static_cast<uint64_t>(scalar) * components;
    const uint64_t stride = attribute.stride ? static_cast<uint64_t>(attribute.stride) : element_bytes;
    for (uint16_t index : indices) {
        const uint64_t pointer = static_cast<uint64_t>(attribute.pointer);
        if (stride != 0 && static_cast<uint64_t>(index) >
                               (std::numeric_limits<uint64_t>::max() - pointer) / stride)
            return false;
        const uint64_t offset = pointer + static_cast<uint64_t>(index) * stride;
        const unsigned char* bytes = nullptr;
        if (!mapped_range(attribute.buffer, offset, element_bytes, &bytes)) return false;
    }
    return true;
}

bool v41_build_candidate(GLenum mode, GLsizei count, GLenum type, const void* indices, candidate_t* output,
                         v41_reject_t* reject) {
    if (reject) *reject = v41_reject_t::none;
    if (output == nullptr || mode != GL_TRIANGLES || count != 6 || type != GL_UNSIGNED_SHORT) {
        if (reject) *reject = v41_reject_t::shape;
        return false;
    }

    const vao_t& vao = current_vao();
    if (vao.unsupported_vertex_model || vao.element_buffer == 0) {
        if (reject) *reject = vao.element_buffer == 0 ? v41_reject_t::ebo : v41_reject_t::vertex_mapping;
        return false;
    }

    const uintptr_t index_offset = reinterpret_cast<uintptr_t>(indices);
    const unsigned char* index_bytes = nullptr;
    if (!mapped_range(vao.element_buffer, static_cast<uint64_t>(index_offset), 12, &index_bytes)) {
        if (reject) *reject = v41_reject_t::ebo;
        return false;
    }

    candidate_t candidate{};
    candidate.program = g_state.program;
    candidate.ebo = vao.element_buffer;
    candidate.index_offset = index_offset;
    candidate.attribs = vao.attribs;
    for (size_t i = 0; i < candidate.indices.size(); ++i)
        std::memcpy(&candidate.indices[i], index_bytes + i * sizeof(uint16_t), sizeof(uint16_t));
    if (!quad6_topology(candidate.indices)) {
        if (reject) *reject = v41_reject_t::topology;
        return false;
    }

    if (!v41_validate_attribute(candidate.attribs[0], 2, candidate.indices) ||
        !v41_validate_attribute(candidate.attribs[1], 2, candidate.indices) ||
        !v41_validate_attribute(candidate.attribs[2], 4, candidate.indices)) {
        if (reject) *reject = v41_reject_t::vertex_mapping;
        return false;
    }

    *output = std::move(candidate);
    return true;
}

void v41_ensure_sampler_shadow() {
    if (!ensure_limits()) return;
    const size_t size = static_cast<size_t>(g_material.max_texture_units);
    if (g_v41_bound_samplers.size() != size) {
        g_v41_bound_samplers.assign(size, 0);
        g_v41_sampler_known.assign(size, 0);
    }
}

bool v41_sampler_is_zero(GLint unit) {
    if (!ensure_limits() || unit < 0 || unit >= g_material.max_texture_units) return false;
    v41_ensure_sampler_shadow();
    const size_t index = static_cast<size_t>(unit);
    if (!g_v41_sampler_known[index]) {
        GLint sampler = 0;
        g_material_backend.get_integer_i_v(GL_SAMPLER_BINDING, static_cast<GLuint>(unit), &sampler);
        g_v41_bound_samplers[index] = static_cast<GLuint>(sampler);
        g_v41_sampler_known[index] = 1;
    }
    return g_v41_bound_samplers[index] == 0;
}

bool v41_ensure_texture_layer(GLint unit, GLuint texture, size_t* bank, GLint* layer, v41_reject_t* reject) {
    if (reject) *reject = v41_reject_t::texture_state;
    if (!bank || !layer || texture == 0 || !v41_sampler_is_zero(unit)) return false;

    const unsigned long long generation = texture_generation(texture);
    const auto existing = g_material.texture_layers.find(texture);
    if (existing != g_material.texture_layers.end() && existing->second.generation == generation &&
        existing->second.bank < g_material.banks.size()) {
        *bank = existing->second.bank;
        *layer = existing->second.layer;
        return true;
    }

    texture_key_t key{};
    if (!query_texture_key(unit, texture, &key)) return false;
    if (!ensure_texture_layer(unit, texture, bank, layer)) {
        if (reject) *reject = v41_reject_t::texture_array;
        return false;
    }
    if (reject) *reject = v41_reject_t::none;
    return true;
}

void v41_record_reject(v41_reject_t reject) {
    const size_t index = static_cast<size_t>(reject);
    if (index < g_v41_stats.rejects.size()) ++g_v41_stats.rejects[index];
}

void v41_report_rejects(bool final) {
    unsigned long long shader_count = 0;
    unsigned long long program_count = 0;
    v41_registry_counts(&shader_count, &program_count);
    const auto& r = g_v41_stats.rejects;
    LOG_I("ZOMDROID_PZ_MATERIAL_STREAM_V41 final=%d classified_ok=%llu "
          "reject=registry:%llu/link:%llu/identity:%llu/vs:%llu/fs:%llu/compiler:%llu/locations:%llu/"
          "uniform:%llu/shape:%llu/ebo:%llu/topology:%llu/vertex:%llu/texture:%llu/array:%llu/objects:%llu "
          "registry=%llu/%llu",
          final ? 1 : 0, g_v41_stats.classified_ok,
          r[static_cast<size_t>(v41_reject_t::program_registry)],
          r[static_cast<size_t>(v41_reject_t::program_link)],
          r[static_cast<size_t>(v41_reject_t::identity)],
          r[static_cast<size_t>(v41_reject_t::vertex_family)],
          r[static_cast<size_t>(v41_reject_t::fragment_family)],
          r[static_cast<size_t>(v41_reject_t::compiler)],
          r[static_cast<size_t>(v41_reject_t::uniform_locations)],
          r[static_cast<size_t>(v41_reject_t::uniform_state)],
          r[static_cast<size_t>(v41_reject_t::shape)],
          r[static_cast<size_t>(v41_reject_t::ebo)],
          r[static_cast<size_t>(v41_reject_t::topology)],
          r[static_cast<size_t>(v41_reject_t::vertex_mapping)],
          r[static_cast<size_t>(v41_reject_t::texture_state)],
          r[static_cast<size_t>(v41_reject_t::texture_array)],
          r[static_cast<size_t>(v41_reject_t::stream_objects)], shader_count, program_count)
}

void v41_maybe_report() {
    const unsigned long long n = g_material.stats.draws_seen;
    if (n == 1 || n == 1024 || n == 65536 || (n != 0 && n % 250000ULL == 0)) {
        report_material_stream();
        v41_report_rejects(false);
    }
}

void v41_fallback(v41_reject_t reject, GLenum mode, GLsizei count, GLenum type, const void* indices) {
    hard_flush();
    ++g_material.stats.fallback_draws;
    ++g_material.stats.backend_draws;
    v41_record_reject(reject);
    g_orig.draw_elements(mode, count, type, indices);
}

void v41_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    ++g_material.stats.draws_seen;

    material_program_t* program = v41_ensure_program(g_state.program);
    if (!program || !program->compatible) {
        v41_fallback(v41_program_reject(g_state.program), mode, count, type, indices);
        v41_maybe_report();
        return;
    }
    if (program->use_texture_value != 1 || program->diffuse_unit < 0 || !ensure_limits() ||
        program->diffuse_unit >= g_material.max_texture_units) {
        v41_fallback(v41_reject_t::uniform_state, mode, count, type, indices);
        v41_maybe_report();
        return;
    }

    candidate_t candidate{};
    v41_reject_t candidate_reject = v41_reject_t::none;
    if (!v41_build_candidate(mode, count, type, indices, &candidate, &candidate_reject)) {
        v41_fallback(candidate_reject, mode, count, type, indices);
        v41_maybe_report();
        return;
    }

    const GLuint texture = g_material.bound_2d[static_cast<size_t>(program->diffuse_unit)];
    size_t bank = 0;
    GLint layer = 0;
    v41_reject_t texture_reject = v41_reject_t::none;
    if (!v41_ensure_texture_layer(program->diffuse_unit, texture, &bank, &layer, &texture_reject)) {
        v41_fallback(texture_reject, mode, count, type, indices);
        v41_maybe_report();
        return;
    }
    if (!ensure_stream_objects()) {
        v41_fallback(v41_reject_t::stream_objects, mode, count, type, indices);
        v41_maybe_report();
        return;
    }

    stream_instance_t instance{};
    if (!build_stream_instance(candidate, *program, layer, &instance)) {
        v41_fallback(v41_reject_t::vertex_mapping, mode, count, type, indices);
        v41_maybe_report();
        return;
    }

    if (g_material.collector_active &&
        (g_material.collector_program != program->stream_program || g_material.collector_bank != bank))
        soft_segment_flush();

    if (!g_material.collector_active) {
        g_material.collector_active = true;
        g_material.collector_program = program->stream_program;
        g_material.collector_bank = bank;
    }

    g_material.collector.push_back(instance);
    ++g_material.stats.draws_captured;
    ++g_material.stats.texture_array_hits;
    ++g_material.stats.instances;
    if (g_material.collector.size() >= kCollectorLimit) soft_segment_flush();
    v41_maybe_report();
}

void v41_glUseProgram(GLuint program) {
    if (g_material.collector_active && !g_material.collector.empty()) {
        material_program_t* next = v41_find_local_program(program);
        if (!next || !next->compatible || next->stream_program != g_material.collector_program)
            soft_segment_flush();
    }
    g_material_wrapped.use_program(program);
    (void)v41_ensure_program(program);
}

void v41_glShaderSource(GLuint shader, GLsizei count, const GLchar* const* strings, const GLint* lengths) {
    g_material_wrapped.shader_source(shader, count, strings, lengths);
    std::string source;
    const bool known = collect_shader_source(count, strings, lengths, &source);
    GLint type = 0;
    if (g_material_backend.get_shader_iv) g_material_backend.get_shader_iv(shader, GL_SHADER_TYPE, &type);
    v41_registry_shader_source(shader, static_cast<GLenum>(type), known ? &source : nullptr);
}

void v41_glAttachShader(GLuint program, GLuint shader) {
    g_v41_wrapped.attach_shader(program, shader);
    v41_registry_attach(program, shader);
    g_v41_local_programs.erase(program);
    g_material.programs.erase(program);
}

void v41_glDetachShader(GLuint program, GLuint shader) {
    g_v41_wrapped.detach_shader(program, shader);
    v41_registry_detach(program, shader);
    g_v41_local_programs.erase(program);
    g_material.programs.erase(program);
}

void v41_glLinkProgram(GLuint program) {
    g_material_wrapped.link_program(program);
    GLint linked = GL_FALSE;
    g_material_backend.get_program_iv(program, GL_LINK_STATUS, &linked);
    v41_registry_link(program, linked == GL_TRUE);
    g_v41_local_programs.erase(program);
    g_material.programs.erase(program);
}

void v41_glDeleteProgram(GLuint program) {
    if (g_material.collector_active && g_state.program == program) soft_segment_flush();
    g_v41_local_programs.erase(program);
    g_material.programs.erase(program);
    v41_registry_delete_program(program);
    g_material_wrapped.delete_program(program);
}

void v41_glUniform1fv(GLint location, GLsizei count, const GLfloat* value) {
    material_program_t* program = v41_find_local_program(g_state.program);
    if (program && program->compatible && location >= 0) {
        if (count == 1 && value != nullptr && location == program->z_depth)
            program->z_depth_value = value[0];
        else if (count == 1 && value != nullptr && location == program->chunk_depth)
            program->chunk_depth_value = value[0];
        else if (g_material.collector_active)
            hard_flush();
    }
    g_v41_wrapped.uniform1fv(location, count, value);
}

void v41_glUniform1iv(GLint location, GLsizei count, const GLint* value) {
    material_program_t* program = v41_find_local_program(g_state.program);
    if (program && program->compatible && location >= 0) {
        if (count == 1 && value != nullptr && location == program->diffuse)
            program->diffuse_unit = value[0];
        else if (count == 1 && value != nullptr && location == program->use_texture)
            program->use_texture_value = value[0];
        else if (g_material.collector_active)
            hard_flush();
    }
    g_v41_wrapped.uniform1iv(location, count, value);
}

void v41_update_program_uniform_i(GLuint program_id, GLint location, GLint value) {
    material_program_t* program = v41_find_local_program(program_id);
    if (!program || !program->compatible || location < 0) return;
    if (location == program->diffuse)
        program->diffuse_unit = value;
    else if (location == program->use_texture)
        program->use_texture_value = value;
    else if (program_id == g_state.program && g_material.collector_active)
        hard_flush();
}

void v41_update_program_uniform_f(GLuint program_id, GLint location, GLfloat value) {
    material_program_t* program = v41_find_local_program(program_id);
    if (!program || !program->compatible || location < 0) return;
    if (location == program->z_depth)
        program->z_depth_value = value;
    else if (location == program->chunk_depth)
        program->chunk_depth_value = value;
    else if (program_id == g_state.program && g_material.collector_active)
        hard_flush();
}

void v41_glProgramUniform1i(GLuint program, GLint location, GLint value) {
    g_v41_wrapped.program_uniform1i(program, location, value);
    v41_update_program_uniform_i(program, location, value);
}

void v41_glProgramUniform1iv(GLuint program, GLint location, GLsizei count, const GLint* value) {
    g_v41_wrapped.program_uniform1iv(program, location, count, value);
    if (count == 1 && value != nullptr)
        v41_update_program_uniform_i(program, location, value[0]);
    else if (program == g_state.program && v41_find_local_program(program) && g_material.collector_active)
        hard_flush();
}

void v41_glProgramUniform1f(GLuint program, GLint location, GLfloat value) {
    g_v41_wrapped.program_uniform1f(program, location, value);
    v41_update_program_uniform_f(program, location, value);
}

void v41_glProgramUniform1fv(GLuint program, GLint location, GLsizei count, const GLfloat* value) {
    g_v41_wrapped.program_uniform1fv(program, location, count, value);
    if (count == 1 && value != nullptr)
        v41_update_program_uniform_f(program, location, value[0]);
    else if (program == g_state.program && v41_find_local_program(program) && g_material.collector_active)
        hard_flush();
}

void v41_glProgramUniformMatrix4fv(GLuint program_id, GLint location, GLsizei count, GLboolean transpose,
                                   const GLfloat* value) {
    g_v41_wrapped.program_uniform_matrix4fv(program_id, location, count, transpose, value);
    material_program_t* program = v41_find_local_program(program_id);
    if (!program || !program->compatible || location < 0) return;
    if (location == program->mvp && count == 1 && transpose == GL_FALSE && value != nullptr)
        std::copy(value, value + 16, program->mvp_value.begin());
    else if (program_id == g_state.program && g_material.collector_active)
        hard_flush();
}

void v41_glBindSampler(GLuint unit, GLuint sampler) {
    g_v41_wrapped.bind_sampler(unit, sampler);
    if (!ensure_limits() || unit >= static_cast<GLuint>(g_material.max_texture_units)) return;
    v41_ensure_sampler_shadow();
    g_v41_bound_samplers[unit] = sampler;
    g_v41_sampler_known[unit] = 1;
}

void v41_reset_local_context() {
    g_v41_local_programs.clear();
    g_v41_bound_samplers.clear();
    g_v41_sampler_known.clear();
}

bool v41_capture_wrappers() {
    g_v41_wrapped.attach_shader = static_cast<glAttachShader_PTR>(GLES.glAttachShader);
    g_v41_wrapped.detach_shader = static_cast<glDetachShader_PTR>(GLES.glDetachShader);
    g_v41_wrapped.uniform1fv = static_cast<glUniform1fv_PTR>(GLES.glUniform1fv);
    g_v41_wrapped.uniform1iv = static_cast<glUniform1iv_PTR>(GLES.glUniform1iv);
    g_v41_wrapped.program_uniform1i = static_cast<glProgramUniform1i_PTR>(GLES.glProgramUniform1i);
    g_v41_wrapped.program_uniform1iv = static_cast<glProgramUniform1iv_PTR>(GLES.glProgramUniform1iv);
    g_v41_wrapped.program_uniform1f = static_cast<glProgramUniform1f_PTR>(GLES.glProgramUniform1f);
    g_v41_wrapped.program_uniform1fv = static_cast<glProgramUniform1fv_PTR>(GLES.glProgramUniform1fv);
    g_v41_wrapped.program_uniform_matrix4fv =
        static_cast<glProgramUniformMatrix4fv_PTR>(GLES.glProgramUniformMatrix4fv);
    g_v41_wrapped.get_uniformfv = static_cast<glGetUniformfv_PTR>(GLES.glGetUniformfv);
    g_v41_wrapped.get_uniformiv = static_cast<glGetUniformiv_PTR>(GLES.glGetUniformiv);
    g_v41_wrapped.bind_sampler = static_cast<glBindSampler_PTR>(GLES.glBindSampler);

    return g_v41_wrapped.attach_shader && g_v41_wrapped.detach_shader && g_v41_wrapped.uniform1fv &&
           g_v41_wrapped.uniform1iv && g_v41_wrapped.get_uniformfv && g_v41_wrapped.get_uniformiv;
}

void v41_install_impl() {
    // Install the V4 implementation base first so its buffer/VAO/texture state
    // shadows and stream emitter are available. V4.1 immediately replaces the
    // program and draw entry points that depended on V2 program_safe().
    mg_pz_repack_renderer_install();
    if (!g_material_enabled || !g_enabled) return;
    if (!v41_capture_wrappers()) {
        LOG_W_FORCE("ZOMDROID_PZ_MATERIAL_STREAM_V41 disabled reason=wrapper_capture_missing")
        return;
    }

    GLES.glDrawElements = v41_glDrawElements;
    GLES.glUseProgram = v41_glUseProgram;
    GLES.glShaderSource = v41_glShaderSource;
    GLES.glAttachShader = v41_glAttachShader;
    GLES.glDetachShader = v41_glDetachShader;
    GLES.glLinkProgram = v41_glLinkProgram;
    GLES.glDeleteProgram = v41_glDeleteProgram;
    GLES.glUniform1fv = v41_glUniform1fv;
    GLES.glUniform1iv = v41_glUniform1iv;
    if (g_v41_wrapped.program_uniform1i) GLES.glProgramUniform1i = v41_glProgramUniform1i;
    if (g_v41_wrapped.program_uniform1iv) GLES.glProgramUniform1iv = v41_glProgramUniform1iv;
    if (g_v41_wrapped.program_uniform1f) GLES.glProgramUniform1f = v41_glProgramUniform1f;
    if (g_v41_wrapped.program_uniform1fv) GLES.glProgramUniform1fv = v41_glProgramUniform1fv;
    if (g_v41_wrapped.program_uniform_matrix4fv)
        GLES.glProgramUniformMatrix4fv = v41_glProgramUniformMatrix4fv;
    if (g_v41_wrapped.bind_sampler) GLES.glBindSampler = v41_glBindSampler;

    g_v41_enabled = true;
    LOG_I("ZOMDROID_PZ_MATERIAL_STREAM_V41 enabled=1 revision=4.1 registry=process_global "
          "candidate=material_no_program_gate uniform_init=driver_query texture_cache=generation "
          "backend_draws=total handoff_restore=none")
}

void v41_before_command_impl(::mg_ts::backend_command_class classification) {
    // Preserve the V4/V2 state-shadow lifecycle, but keep shader/program source
    // metadata process-global. No TLS handoff/restore is performed in V4.1.
    mg_ts::pz_repack_before_backend_command(classification);
    if (!g_v41_enabled) return;
    if (classification == ::mg_ts::backend_command_class::context_adopt) {
        v41_reset_local_context();
        return;
    }
    if (classification == ::mg_ts::backend_command_class::context_release) {
        v41_reset_local_context();
        report_material_stream();
        v41_report_rejects(true);
    }
}

} // namespace

void install_v41_internal() { v41_install_impl(); }
void before_v41_internal(::mg_ts::backend_command_class classification) { v41_before_command_impl(classification); }

#else

void install_v41_internal() { mg_pz_repack_renderer_install(); }

#endif

} // namespace v41_base

#if defined(ZOMDROID_EXPERIMENTAL)
namespace mg_ts {
void pz_repack_before_backend_command(backend_command_class classification) {
    v41_base::before_v41_internal(classification);
}
} // namespace mg_ts
#endif

void mg_pz_repack_renderer_install(void) {
    v41_base::install_v41_internal();
}
