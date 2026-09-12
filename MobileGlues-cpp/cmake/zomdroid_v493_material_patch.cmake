# V4.9.3 experimental-only material-stream patch.
#
# The historical V4 emitter stays untouched in the repository. The experimental
# configure step invokes this file from the V4.9.2 texture generator and patches
# the checked-out V4 base in place before compilation. Every edit is anchored to
# the exact V4.9.2 source; a drift fails closed instead of silently changing a
# different renderer revision.

if(NOT DEFINED INPUT)
    message(FATAL_ERROR "zomdroid_v493_material_patch.cmake requires INPUT from the texture generator")
endif()

get_filename_component(_v493_gl_dir "${INPUT}" DIRECTORY)
set(_v493_base "${_v493_gl_dir}/pz_material_stream_renderer_v4_base.cpp")
if(NOT EXISTS "${_v493_base}")
    message(FATAL_ERROR "V4.9.3 material base not found: ${_v493_base}")
endif()

file(READ "${_v493_base}" _v493_src)
string(FIND "${_v493_src}" "ZOMDROID_V493_AUX_SIDECAR" _v493_already)
if(NOT _v493_already EQUAL -1)
    return()
endif()

macro(v493_replace _label _old_var _new_var)
    string(FIND "${_v493_src}" "${${_old_var}}" _v493_pos)
    if(_v493_pos EQUAL -1)
        message(FATAL_ERROR "V4.9.3 material patch anchor '${_label}' not found; refusing an unverified generated source")
    endif()
    string(REPLACE "${${_old_var}}" "${${_new_var}}" _v493_src "${_v493_src}")
endmacro()

set(_v493_old_instance [[struct stream_instance_t {
    float pos_uv[6][4];
    float color[6][4];
    float mvp[16];
    float material[4]; // zDepth, chunkDepth, texture layer, useTexture
};
static_assert(sizeof(stream_instance_t) == 272, "std430 stream instance layout changed");]])
set(_v493_new_instance [[struct stream_instance_t {
    float pos_uv[6][4];
    float color[6][4];
    float mvp[16];
    float material[4]; // zDepth, chunkDepth, texture layer, useTexture
};
static_assert(sizeof(stream_instance_t) == 272, "std430 stream instance layout changed");

// ZOMDROID_V493_AUX_SIDECAR: depth-only secondary UV payload. Keeping this
// separate preserves the 272-byte no-depth instance layout and its bandwidth.
struct stream_aux_instance_t {
    float uv[6][4]; // xy=location 3, zw=location 4 when present
};
static_assert(sizeof(stream_aux_instance_t) == 96, "std430 aux stream instance layout changed");]])
v493_replace("instance-sidecar" _v493_old_instance _v493_new_instance)

set(_v493_old_state [[    GLuint instance_buffer = 0;
    GLuint stream_vao = 0;

    bool collector_active = false;
    GLuint collector_program = 0;
    size_t collector_bank = 0;
    std::vector<stream_instance_t> collector;]])
set(_v493_new_state [[    GLuint instance_buffer = 0;
    GLuint stream_vao = 0;
    GLuint aux_instance_buffer = 0;
    GLint aux_ssbo_binding = -1;

    bool collector_active = false;
    GLuint collector_program = 0;
    size_t collector_bank = 0;
    std::vector<stream_instance_t> collector;
    bool aux_collector_active = false;
    std::vector<stream_aux_instance_t> aux_collector;]])
v493_replace("material-state" _v493_old_state _v493_new_state)

set(_v493_old_limits [[    g_material.internal_texture_unit = g_material.max_texture_units - 1;
    g_material.ssbo_binding = static_cast<GLuint>(g_material.max_ssbo_bindings - 1);
    g_material.bound_2d.assign(static_cast<size_t>(g_material.max_texture_units), 0);]])
set(_v493_new_limits [[    g_material.internal_texture_unit = g_material.max_texture_units - 1;
    g_material.ssbo_binding = static_cast<GLuint>(g_material.max_ssbo_bindings - 1);
    // The auxiliary binding is borrowed only while an instanced depth segment is
    // emitted, then restored exactly like the primary SSBO binding.
    g_material.aux_ssbo_binding = g_material.max_ssbo_bindings >= 2 ? g_material.max_ssbo_bindings - 2 : -1;
    g_material.bound_2d.assign(static_cast<size_t>(g_material.max_texture_units), 0);]])
v493_replace("aux-binding" _v493_old_limits _v493_new_limits)

set(_v493_old_objects [[bool ensure_stream_objects() {
    if (g_material.instance_buffer == 0) g_orig.gen_buffers(1, &g_material.instance_buffer);
    if (g_material.stream_vao == 0) g_orig.gen_vertex_arrays(1, &g_material.stream_vao);
    return g_material.instance_buffer != 0 && g_material.stream_vao != 0;
}]])
set(_v493_new_objects [[bool ensure_stream_objects() {
    if (g_material.instance_buffer == 0) g_orig.gen_buffers(1, &g_material.instance_buffer);
    if (g_material.stream_vao == 0) g_orig.gen_vertex_arrays(1, &g_material.stream_vao);
    return g_material.instance_buffer != 0 && g_material.stream_vao != 0;
}

bool ensure_aux_stream_objects() {
    if (!ensure_limits() || g_material.aux_ssbo_binding < 0 ||
        static_cast<GLuint>(g_material.aux_ssbo_binding) == g_material.ssbo_binding)
        return false;
    if (g_material.aux_instance_buffer == 0) g_orig.gen_buffers(1, &g_material.aux_instance_buffer);
    return g_material.aux_instance_buffer != 0;
}

void reset_aux_collector() {
    g_material.aux_collector_active = false;
    g_material.aux_collector.clear();
}]])
v493_replace("aux-objects" _v493_old_objects _v493_new_objects)

set(_v493_old_emit_head [[void emit_collector() {
    if (!g_material.collector_active || g_material.collector.empty()) {
        g_material.collector_active = false;
        g_material.collector.clear();
        return;
    }
    if (!ensure_stream_objects() || g_material.collector_bank >= g_material.banks.size()) {
        g_material.collector_active = false;
        g_material.collector.clear();
        return;
    }

    const GLuint restore_program = g_state.program;]])
set(_v493_new_emit_head [[void emit_collector() {
    if (!g_material.collector_active || g_material.collector.empty()) {
        g_material.collector_active = false;
        g_material.collector.clear();
        reset_aux_collector();
        return;
    }
    const bool aux_active = g_material.aux_collector_active;
    if (!ensure_stream_objects() || g_material.collector_bank >= g_material.banks.size() ||
        (aux_active && (g_material.aux_collector.size() != g_material.collector.size() ||
                        !ensure_aux_stream_objects()))) {
        LOG_W_FORCE("ZOMDROID_PZ_MATERIAL_STREAM_V493 aux_emit_invariant main=%zu aux=%zu active=%d",
                    g_material.collector.size(), g_material.aux_collector.size(), aux_active ? 1 : 0)
        g_material.collector_active = false;
        g_material.collector.clear();
        reset_aux_collector();
        return;
    }

    const GLuint restore_program = g_state.program;]])
v493_replace("emit-head" _v493_old_emit_head _v493_new_emit_head)

set(_v493_old_emit_upload [[    GLint restore_ssbo_generic = 0;
    GLint restore_ssbo_indexed = 0;
    GLint64 restore_ssbo_start = 0;
    GLint64 restore_ssbo_size = 0;
    g_material_backend.get_integerv(GL_SHADER_STORAGE_BUFFER_BINDING, &restore_ssbo_generic);
    g_material_backend.get_integer_i_v(GL_SHADER_STORAGE_BUFFER_BINDING, g_material.ssbo_binding,
                                        &restore_ssbo_indexed);
    g_material_backend.get_integer64_i_v(GL_SHADER_STORAGE_BUFFER_START, g_material.ssbo_binding,
                                          &restore_ssbo_start);
    g_material_backend.get_integer64_i_v(GL_SHADER_STORAGE_BUFFER_SIZE, g_material.ssbo_binding,
                                          &restore_ssbo_size);

    g_material_backend.active_texture(GL_TEXTURE0 + static_cast<GLenum>(g_material.internal_texture_unit));
    GLint restore_array = 0;
    g_material_backend.get_integerv(GL_TEXTURE_BINDING_2D_ARRAY, &restore_array);
    g_material_backend.bind_texture(GL_TEXTURE_2D_ARRAY, g_material.banks[g_material.collector_bank].texture);

    const size_t bytes = g_material.collector.size() * sizeof(stream_instance_t);
    g_orig.bind_buffer(GL_SHADER_STORAGE_BUFFER, g_material.instance_buffer);
    g_orig.buffer_data(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(bytes), g_material.collector.data(),
                       GL_STREAM_DRAW);
    g_material_backend.bind_buffer_base(GL_SHADER_STORAGE_BUFFER, g_material.ssbo_binding, g_material.instance_buffer);
    g_orig.use_program(g_material.collector_program);]])
set(_v493_new_emit_upload [[    GLint restore_ssbo_generic = 0;
    GLint restore_ssbo_indexed = 0;
    GLint64 restore_ssbo_start = 0;
    GLint64 restore_ssbo_size = 0;
    GLint restore_aux_indexed = 0;
    GLint64 restore_aux_start = 0;
    GLint64 restore_aux_size = 0;
    g_material_backend.get_integerv(GL_SHADER_STORAGE_BUFFER_BINDING, &restore_ssbo_generic);
    g_material_backend.get_integer_i_v(GL_SHADER_STORAGE_BUFFER_BINDING, g_material.ssbo_binding,
                                        &restore_ssbo_indexed);
    g_material_backend.get_integer64_i_v(GL_SHADER_STORAGE_BUFFER_START, g_material.ssbo_binding,
                                          &restore_ssbo_start);
    g_material_backend.get_integer64_i_v(GL_SHADER_STORAGE_BUFFER_SIZE, g_material.ssbo_binding,
                                          &restore_ssbo_size);
    if (aux_active) {
        const GLuint aux_binding = static_cast<GLuint>(g_material.aux_ssbo_binding);
        g_material_backend.get_integer_i_v(GL_SHADER_STORAGE_BUFFER_BINDING, aux_binding, &restore_aux_indexed);
        g_material_backend.get_integer64_i_v(GL_SHADER_STORAGE_BUFFER_START, aux_binding, &restore_aux_start);
        g_material_backend.get_integer64_i_v(GL_SHADER_STORAGE_BUFFER_SIZE, aux_binding, &restore_aux_size);
    }

    g_material_backend.active_texture(GL_TEXTURE0 + static_cast<GLenum>(g_material.internal_texture_unit));
    GLint restore_array = 0;
    g_material_backend.get_integerv(GL_TEXTURE_BINDING_2D_ARRAY, &restore_array);
    g_material_backend.bind_texture(GL_TEXTURE_2D_ARRAY, g_material.banks[g_material.collector_bank].texture);

    const size_t bytes = g_material.collector.size() * sizeof(stream_instance_t);
    const size_t aux_bytes = aux_active ? g_material.aux_collector.size() * sizeof(stream_aux_instance_t) : 0;
    g_orig.bind_buffer(GL_SHADER_STORAGE_BUFFER, g_material.instance_buffer);
    g_orig.buffer_data(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(bytes), g_material.collector.data(),
                       GL_STREAM_DRAW);
    g_material_backend.bind_buffer_base(GL_SHADER_STORAGE_BUFFER, g_material.ssbo_binding, g_material.instance_buffer);
    if (aux_active) {
        g_orig.bind_buffer(GL_SHADER_STORAGE_BUFFER, g_material.aux_instance_buffer);
        g_orig.buffer_data(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(aux_bytes),
                           g_material.aux_collector.data(), GL_STREAM_DRAW);
        g_material_backend.bind_buffer_base(GL_SHADER_STORAGE_BUFFER,
                                            static_cast<GLuint>(g_material.aux_ssbo_binding),
                                            g_material.aux_instance_buffer);
    }
    g_orig.use_program(g_material.collector_program);]])
v493_replace("emit-upload" _v493_old_emit_upload _v493_new_emit_upload)

set(_v493_old_emit_restore [[    if (restore_ssbo_indexed != 0 && restore_ssbo_size > 0)
        g_material_backend.bind_buffer_range(GL_SHADER_STORAGE_BUFFER, g_material.ssbo_binding,
                                             static_cast<GLuint>(restore_ssbo_indexed),
                                             static_cast<GLintptr>(restore_ssbo_start),
                                             static_cast<GLsizeiptr>(restore_ssbo_size));
    else
        g_material_backend.bind_buffer_base(GL_SHADER_STORAGE_BUFFER, g_material.ssbo_binding,
                                            static_cast<GLuint>(restore_ssbo_indexed));
    g_orig.bind_buffer(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(restore_ssbo_generic));
    g_material_backend.bind_texture(GL_TEXTURE_2D_ARRAY, static_cast<GLuint>(restore_array));
    g_material_backend.active_texture(restore_active);

    const unsigned long long count = static_cast<unsigned long long>(g_material.collector.size());
    ++g_material.stats.backend_draws;
    g_material.stats.draws_eliminated += count > 0 ? count - 1 : 0;
    g_material.stats.packed_bytes += bytes;
    g_material.collector_active = false;
    g_material.collector.clear();]])
set(_v493_new_emit_restore [[    if (restore_ssbo_indexed != 0 && restore_ssbo_size > 0)
        g_material_backend.bind_buffer_range(GL_SHADER_STORAGE_BUFFER, g_material.ssbo_binding,
                                             static_cast<GLuint>(restore_ssbo_indexed),
                                             static_cast<GLintptr>(restore_ssbo_start),
                                             static_cast<GLsizeiptr>(restore_ssbo_size));
    else
        g_material_backend.bind_buffer_base(GL_SHADER_STORAGE_BUFFER, g_material.ssbo_binding,
                                            static_cast<GLuint>(restore_ssbo_indexed));
    if (aux_active) {
        const GLuint aux_binding = static_cast<GLuint>(g_material.aux_ssbo_binding);
        if (restore_aux_indexed != 0 && restore_aux_size > 0)
            g_material_backend.bind_buffer_range(GL_SHADER_STORAGE_BUFFER, aux_binding,
                                                 static_cast<GLuint>(restore_aux_indexed),
                                                 static_cast<GLintptr>(restore_aux_start),
                                                 static_cast<GLsizeiptr>(restore_aux_size));
        else
            g_material_backend.bind_buffer_base(GL_SHADER_STORAGE_BUFFER, aux_binding,
                                                static_cast<GLuint>(restore_aux_indexed));
    }
    g_orig.bind_buffer(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(restore_ssbo_generic));
    g_material_backend.bind_texture(GL_TEXTURE_2D_ARRAY, static_cast<GLuint>(restore_array));
    g_material_backend.active_texture(restore_active);

    const unsigned long long count = static_cast<unsigned long long>(g_material.collector.size());
    ++g_material.stats.backend_draws;
    g_material.stats.draws_eliminated += count > 0 ? count - 1 : 0;
    g_material.stats.packed_bytes += bytes + aux_bytes;
    g_material.collector_active = false;
    g_material.collector.clear();
    reset_aux_collector();]])
v493_replace("emit-restore" _v493_old_emit_restore _v493_new_emit_restore)

set(_v493_old_cleanup [[    if (g_material.instance_buffer) g_orig.delete_buffers(1, &g_material.instance_buffer);
    if (g_material.stream_vao) g_orig.delete_vertex_arrays(1, &g_material.stream_vao);]])
set(_v493_new_cleanup [[    if (g_material.instance_buffer) g_orig.delete_buffers(1, &g_material.instance_buffer);
    if (g_material.aux_instance_buffer) g_orig.delete_buffers(1, &g_material.aux_instance_buffer);
    if (g_material.stream_vao) g_orig.delete_vertex_arrays(1, &g_material.stream_vao);]])
v493_replace("cleanup" _v493_old_cleanup _v493_new_cleanup)

file(WRITE "${_v493_base}" "${_v493_src}")
message(STATUS "Applied V4.9.3 depth auxiliary-UV sidecar patch to experimental material emitter")
