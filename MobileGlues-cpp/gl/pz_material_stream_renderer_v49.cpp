// MobileGlues - gl/pz_material_stream_renderer_v49.cpp
// Project Zomboid V4.9 perf-ceiling overlay: extend the V4.8 material stream
// to the exact tile-with-depth shader family while preserving DEPTH/MASK
// bindings and alpha state as segment state. UI correctness remains out of
// scope for this ceiling build; world/depth ordering is not relaxed.

#include "pz_material_stream_renderer.cpp"

#if defined(ZOMDROID_EXPERIMENTAL)

namespace v44_base {
namespace v43_base {
namespace legacy_v41 {
namespace v41_base {
namespace {

struct v49_program_t {
    bool attempted = false;
    bool compatible = false;
    bool has_mask = false;
    unsigned long long generation = 0;
    GLuint app_program = 0;
    GLuint stream_program = 0;

    GLint blend_z = -1;
    GLint blend_to_z = -1;
    GLint depth = -1;
    GLint mask = -1;
    GLint alpha_func = -1;
    GLint alpha_ref = -1;

    GLint depth_unit = 0;
    GLint mask_unit = 0;
    GLint alpha_func_value = 0;
    GLfloat alpha_ref_value = 0.0f;
    GLfloat blend_z_value = 0.0f;
    GLfloat blend_to_z_value = 0.0f;

    GLint stream_depth = -1;
    GLint stream_mask = -1;
    GLint stream_alpha_func = -1;
    GLint stream_alpha_ref = -1;
};

struct v49_collector_key_t {
    bool valid = false;
    GLuint app_program = 0;
    GLuint stream_program = 0;
    size_t bank = 0;
    GLint depth_unit = -1;
    GLuint depth_texture = 0;
    bool has_mask = false;
    GLint mask_unit = -1;
    GLuint mask_texture = 0;
    bool has_alpha_func = false;
    GLint alpha_func = 0;
    bool has_alpha_ref = false;
    GLfloat alpha_ref = 0.0f;
};

struct v49_stats_t {
    unsigned long long programs_ok = 0;
    unsigned long long programs_reject = 0;
    unsigned long long depth_captured = 0;
    unsigned long long depth_fallback = 0;
    unsigned long long aux_segments = 0;
    unsigned long long aux_hard_flush = 0;
    unsigned long long sampler_updates = 0;
    unsigned long long blend_updates = 0;
    unsigned long long alpha_updates = 0;
};

struct v49_wrapped_t {
    glDrawElements_PTR draw_elements = nullptr;
    glBindTexture_PTR bind_texture = nullptr;
    glBindSampler_PTR bind_sampler = nullptr;
    glUniform1f_PTR uniform1f = nullptr;
    glUniform1fv_PTR uniform1fv = nullptr;
    glUniform1i_PTR uniform1i = nullptr;
    glUniform1iv_PTR uniform1iv = nullptr;
    glProgramUniform1f_PTR program_uniform1f = nullptr;
    glProgramUniform1fv_PTR program_uniform1fv = nullptr;
    glProgramUniform1i_PTR program_uniform1i = nullptr;
    glProgramUniform1iv_PTR program_uniform1iv = nullptr;
    glLinkProgram_PTR link_program = nullptr;
    glDeleteProgram_PTR delete_program = nullptr;
};

thread_local std::unordered_map<GLuint, v49_program_t> g_v49_programs;
thread_local v49_collector_key_t g_v49_collector_key;
thread_local v49_stats_t g_v49_stats;
v49_wrapped_t g_v49_wrapped;
bool g_v49_enabled = false;

bool v49_float_equal(GLfloat a, GLfloat b) {
    uint32_t aa = 0, bb = 0;
    static_assert(sizeof(aa) == sizeof(a));
    std::memcpy(&aa, &a, sizeof(a));
    std::memcpy(&bb, &b, sizeof(b));
    return aa == bb;
}

void v49_sync_collector_key() {
    if (!g_material.collector_active || g_material.collector.empty()) g_v49_collector_key = {};
}

void v49_report(bool final) {
    LOG_I("ZOMDROID_PZ_MATERIAL_STREAM_V49 final=%d programs=%llu/%llu depth_captured=%llu depth_fallback=%llu "
          "aux_segments=%llu aux_hard=%llu sampler_updates=%llu blend_updates=%llu alpha_updates=%llu",
          final ? 1 : 0, g_v49_stats.programs_ok, g_v49_stats.programs_reject,
          g_v49_stats.depth_captured, g_v49_stats.depth_fallback, g_v49_stats.aux_segments,
          g_v49_stats.aux_hard_flush, g_v49_stats.sampler_updates, g_v49_stats.blend_updates,
          g_v49_stats.alpha_updates)
}

void v49_maybe_report() {
    const unsigned long long n = g_material.stats.draws_seen;
    if (n == 1 || n == 65536 || (n != 0 && n % 250000ULL == 0)) v49_report(false);
}

bool v49_transform_depth_vertex(const std::string& original, GLint ssbo_binding, std::string* transformed) {
    if (!transformed || ssbo_binding < 0) return false;
    if (!only_uniforms(original, {"ModelViewProjection", "zDepth", "zDepthBlendZ", "zDepthBlendToZ"}))
        return false;

    static const std::regex pos_decl(
        R"(layout\s*\(\s*location\s*=\s*0\s*\)\s*in\s+(?:(?:lowp|mediump|highp)\s+)?vec2\s+vPos\s*;)");
    static const std::regex uv_decl(
        R"(layout\s*\(\s*location\s*=\s*1\s*\)\s*in\s+(?:(?:lowp|mediump|highp)\s+)?vec2\s+vUV\s*;)");
    static const std::regex color_decl(
        R"(layout\s*\(\s*location\s*=\s*2\s*\)\s*in\s+(?:(?:lowp|mediump|highp)\s+)?vec4\s+vCol\s*;)");
    static const std::regex any_input(R"(layout\s*\(\s*location\s*=\s*[0-9]+\s*\)\s*in\s+)");
    static const std::regex mvp_decl(
        R"(uniform\s+(?:(?:lowp|mediump|highp)\s+)?mat4\s+ModelViewProjection\s*;)");
    static const std::regex z_decl(
        R"(uniform\s+(?:(?:lowp|mediump|highp)\s+)?float\s+zDepth\s*;)");
    static const std::regex blend_z_decl(
        R"(uniform\s+(?:(?:lowp|mediump|highp)\s+)?float\s+zDepthBlendZ\s*;)");
    static const std::regex blend_to_decl(
        R"(uniform\s+(?:(?:lowp|mediump|highp)\s+)?float\s+zDepthBlendToZ\s*;)");
    static const std::regex main_decl(R"(void\s+main\s*\(\s*\)\s*\{)");

    if (regex_count(original, any_input) != 3 || regex_count(original, pos_decl) != 1 ||
        regex_count(original, uv_decl) != 1 || regex_count(original, color_decl) != 1 ||
        regex_count(original, mvp_decl) != 1 || regex_count(original, z_decl) != 1 ||
        regex_count(original, blend_z_decl) != 1 || regex_count(original, blend_to_decl) != 1 ||
        regex_count(original, main_decl) != 1)
        return false;

    std::string source = original;
    if (!replace_unique(&source, pos_decl, "vec2 vPos;") ||
        !replace_unique(&source, uv_decl, "vec2 vUV;") ||
        !replace_unique(&source, color_decl, "vec4 vCol;") ||
        !replace_unique(&source, mvp_decl, "mat4 ModelViewProjection;") ||
        !replace_unique(&source, z_decl, "float zDepth;") ||
        !replace_unique(&source, blend_z_decl, "float zDepthBlendZ;") ||
        !replace_unique(&source, blend_to_decl, "float zDepthBlendToZ;"))
        return false;

    const std::string declarations =
        "struct ZomdroidStreamInstance {\n"
        "    vec4 pos_uv[6];\n"
        "    vec4 color[6];\n"
        "    mat4 mvp;\n"
        "    vec4 material;\n"
        "};\n"
        "layout(std430, binding = " + std::to_string(ssbo_binding) +
        ") readonly buffer ZomdroidStreamBlock {\n"
        "    ZomdroidStreamInstance zomdroidInstances[];\n"
        "};\n"
        "flat out highp int zomdroidStreamLayer;\n"
        "flat out highp float zomdroidStreamBlendZ;\n"
        "flat out highp float zomdroidStreamBlendToZ;";
    if (!insert_after_precision(&source, declarations)) return false;

    std::smatch main_match;
    if (!std::regex_search(source, main_match, main_decl)) return false;
    const size_t body = static_cast<size_t>(main_match.position() + main_match.length());
    const std::string setup =
        "\n    ZomdroidStreamInstance zomdroidStream = zomdroidInstances[gl_InstanceID];\n"
        "    int zomdroidStreamVertex = gl_VertexID % 6;\n"
        "    vPos = zomdroidStream.pos_uv[zomdroidStreamVertex].xy;\n"
        "    vUV = zomdroidStream.pos_uv[zomdroidStreamVertex].zw;\n"
        "    vCol = zomdroidStream.color[zomdroidStreamVertex];\n"
        "    ModelViewProjection = zomdroidStream.mvp;\n"
        "    zDepth = zomdroidStream.material.x;\n"
        "    zDepthBlendZ = zomdroidStream.material.y;\n"
        "    zDepthBlendToZ = zomdroidStream.material.z;\n"
        "    zomdroidStreamLayer = int(zomdroidStream.material.w);\n"
        "    zomdroidStreamBlendZ = zDepthBlendZ;\n"
        "    zomdroidStreamBlendToZ = zDepthBlendToZ;\n";
    source.insert(body, setup);
    *transformed = std::move(source);
    return true;
}

bool v49_transform_depth_fragment(const std::string& original, std::string* transformed, bool* has_mask) {
    if (has_mask) *has_mask = false;
    if (!transformed) return false;
    if (!only_uniforms(original, {"DIFFUSE", "DEPTH", "MASK", "zDepthBlendZ", "zDepthBlendToZ",
                                  "zomdroidAlphaFunc", "zomdroidAlphaRef"}))
        return false;

    static const std::regex diffuse_decl(
        R"(uniform\s+(?:(?:lowp|mediump|highp)\s+)?sampler2D\s+DIFFUSE\s*;)");
    static const std::regex depth_decl(
        R"(uniform\s+(?:(?:lowp|mediump|highp)\s+)?sampler2D\s+DEPTH\s*;)");
    static const std::regex mask_decl(
        R"(uniform\s+(?:(?:lowp|mediump|highp)\s+)?sampler2D\s+MASK\s*;)");
    static const std::regex blend_z_decl(
        R"(uniform\s+(?:(?:lowp|mediump|highp)\s+)?float\s+zDepthBlendZ\s*;)");
    static const std::regex blend_to_decl(
        R"(uniform\s+(?:(?:lowp|mediump|highp)\s+)?float\s+zDepthBlendToZ\s*;)");
    static const std::regex any_sampler(R"(\bsampler[A-Za-z0-9_]*\b)");
    static const std::regex main_decl(R"(void\s+main\s*\(\s*\)\s*\{)");

    const size_t mask_count = regex_count(original, mask_decl);
    const size_t sampler_count = regex_count(original, any_sampler);
    if (regex_count(original, diffuse_decl) != 1 || regex_count(original, depth_decl) != 1 ||
        mask_count > 1 || sampler_count != 2 + mask_count ||
        regex_count(original, blend_z_decl) != 1 || regex_count(original, blend_to_decl) != 1 ||
        regex_count(original, main_decl) != 1)
        return false;

    std::string source = original;
    if (!replace_unique(&source, diffuse_decl, "uniform highp sampler2DArray zomdroidStreamDiffuse;") ||
        !replace_unique(&source, blend_z_decl, "float zDepthBlendZ;") ||
        !replace_unique(&source, blend_to_decl, "float zDepthBlendToZ;") ||
        !v42_rewrite_diffuse_texture_call(&source))
        return false;

    const std::string declarations =
        "flat in highp int zomdroidStreamLayer;\n"
        "flat in highp float zomdroidStreamBlendZ;\n"
        "flat in highp float zomdroidStreamBlendToZ;";
    if (!insert_after_precision(&source, declarations)) return false;

    std::smatch main_match;
    if (!std::regex_search(source, main_match, main_decl)) return false;
    const size_t body = static_cast<size_t>(main_match.position() + main_match.length());
    source.insert(body,
                  "\n    zDepthBlendZ = zomdroidStreamBlendZ;\n"
                  "    zDepthBlendToZ = zomdroidStreamBlendToZ;\n");

    if (has_mask) *has_mask = mask_count == 1;
    *transformed = std::move(source);
    return true;
}

void v49_apply_stream_state(const v49_program_t& meta) {
    if (!meta.compatible || meta.stream_program == 0) return;
    const GLuint restore = g_state.program;
    g_orig.use_program(meta.stream_program);
    if (meta.stream_depth >= 0) g_material_backend.uniform1i(meta.stream_depth, meta.depth_unit);
    if (meta.has_mask && meta.stream_mask >= 0) g_material_backend.uniform1i(meta.stream_mask, meta.mask_unit);
    if (meta.stream_alpha_func >= 0) g_material_backend.uniform1i(meta.stream_alpha_func, meta.alpha_func_value);
    if (meta.stream_alpha_ref >= 0 && g_material_wrapped.uniform1f)
        g_material_wrapped.uniform1f(meta.stream_alpha_ref, meta.alpha_ref_value);
    g_orig.use_program(restore);
}

v49_program_t* v49_ensure_depth_program(GLuint program_id) {
    if (!g_v49_enabled || program_id == 0) return nullptr;
    auto cached = g_v49_programs.find(program_id);
    if (cached != g_v49_programs.end()) return cached->second.compatible ? &cached->second : nullptr;

    v49_program_t meta{};
    meta.attempted = true;
    meta.app_program = program_id;

    v41_program_record_t source{};
    if (!v41_registry_program(program_id, &source) || !source.linked || !source.complete || !source.identity_safe ||
        !ensure_limits()) {
        ++g_v49_stats.programs_reject;
        g_v49_programs.emplace(program_id, meta);
        return nullptr;
    }
    meta.generation = source.generation;

    std::string stream_vertex;
    std::string stream_fragment;
    bool has_mask = false;
    if (!v49_transform_depth_vertex(source.vertex, static_cast<GLint>(g_material.ssbo_binding), &stream_vertex) ||
        !v49_transform_depth_fragment(source.fragment, &stream_fragment, &has_mask)) {
        ++g_v49_stats.programs_reject;
        g_v49_programs.emplace(program_id, meta);
        return nullptr;
    }

    material_program_t info{};
    info.classified = true;
    info.stream_program = stream_variant_for(stream_vertex, stream_fragment);
    if (!info.stream_program || !g_v41_wrapped.get_uniformfv || !g_v41_wrapped.get_uniformiv) {
        ++g_v49_stats.programs_reject;
        g_v49_programs.emplace(program_id, meta);
        return nullptr;
    }

    info.mvp = g_material_backend.get_uniform_location(program_id, "ModelViewProjection");
    info.z_depth = g_material_backend.get_uniform_location(program_id, "zDepth");
    info.chunk_depth = -1;
    info.diffuse = g_material_backend.get_uniform_location(program_id, "DIFFUSE");
    info.use_texture = -1;
    info.use_texture_value = 1;

    meta.blend_z = g_material_backend.get_uniform_location(program_id, "zDepthBlendZ");
    meta.blend_to_z = g_material_backend.get_uniform_location(program_id, "zDepthBlendToZ");
    meta.depth = g_material_backend.get_uniform_location(program_id, "DEPTH");
    meta.has_mask = has_mask;
    meta.mask = has_mask ? g_material_backend.get_uniform_location(program_id, "MASK") : -1;
    meta.alpha_func = g_material_backend.get_uniform_location(program_id, "zomdroidAlphaFunc");
    meta.alpha_ref = g_material_backend.get_uniform_location(program_id, "zomdroidAlphaRef");

    meta.stream_program = info.stream_program;
    meta.stream_depth = g_material_backend.get_uniform_location(info.stream_program, "DEPTH");
    meta.stream_mask = has_mask ? g_material_backend.get_uniform_location(info.stream_program, "MASK") : -1;
    meta.stream_alpha_func = g_material_backend.get_uniform_location(info.stream_program, "zomdroidAlphaFunc");
    meta.stream_alpha_ref = g_material_backend.get_uniform_location(info.stream_program, "zomdroidAlphaRef");

    const bool locations_ok = info.mvp >= 0 && info.z_depth >= 0 && info.diffuse >= 0 &&
                              meta.blend_z >= 0 && meta.blend_to_z >= 0 && meta.depth >= 0 &&
                              meta.stream_depth >= 0 && (!has_mask || (meta.mask >= 0 && meta.stream_mask >= 0));
    if (!locations_ok) {
        ++g_v49_stats.programs_reject;
        g_v49_programs.emplace(program_id, meta);
        return nullptr;
    }

    g_v41_wrapped.get_uniformfv(program_id, info.mvp, info.mvp_value.data());
    g_v41_wrapped.get_uniformfv(program_id, info.z_depth, &info.z_depth_value);
    g_v41_wrapped.get_uniformiv(program_id, info.diffuse, &info.diffuse_unit);
    g_v41_wrapped.get_uniformfv(program_id, meta.blend_z, &meta.blend_z_value);
    g_v41_wrapped.get_uniformfv(program_id, meta.blend_to_z, &meta.blend_to_z_value);
    g_v41_wrapped.get_uniformiv(program_id, meta.depth, &meta.depth_unit);
    if (meta.has_mask) g_v41_wrapped.get_uniformiv(program_id, meta.mask, &meta.mask_unit);
    if (meta.alpha_func >= 0) g_v41_wrapped.get_uniformiv(program_id, meta.alpha_func, &meta.alpha_func_value);
    if (meta.alpha_ref >= 0) g_v41_wrapped.get_uniformfv(program_id, meta.alpha_ref, &meta.alpha_ref_value);

    const bool units_ok = info.diffuse_unit >= 0 && info.diffuse_unit < g_material.max_texture_units &&
                          meta.depth_unit >= 0 && meta.depth_unit < g_material.max_texture_units &&
                          (!meta.has_mask || (meta.mask_unit >= 0 && meta.mask_unit < g_material.max_texture_units));
    if (!units_ok) {
        ++g_v49_stats.programs_reject;
        g_v49_programs.emplace(program_id, meta);
        return nullptr;
    }

    info.compatible = true;
    meta.compatible = true;
    g_material.programs[program_id] = info;
    g_v41_local_programs[program_id] = v41_local_program_t{source.generation, v41_reject_t::none};
    ++g_v41_stats.classified_ok;
    ++g_v49_stats.programs_ok;
    auto inserted = g_v49_programs.emplace(program_id, meta).first;
    LOG_I("ZOMDROID_PZ_MATERIAL_STREAM_V49_PROGRAM program=%u result=compatible family=tile_depth mask=%d "
          "units=diffuse:%d/depth:%d/mask:%d alpha=%d/%d",
          program_id, meta.has_mask ? 1 : 0, info.diffuse_unit, meta.depth_unit,
          meta.has_mask ? meta.mask_unit : -1, meta.alpha_func >= 0 ? 1 : 0, meta.alpha_ref >= 0 ? 1 : 0)
    return &inserted->second;
}

bool v49_key_equal(const v49_collector_key_t& a, const v49_collector_key_t& b) {
    return a.valid && b.valid && a.stream_program == b.stream_program && a.bank == b.bank &&
           a.depth_unit == b.depth_unit && a.depth_texture == b.depth_texture &&
           a.has_mask == b.has_mask && (!a.has_mask || (a.mask_unit == b.mask_unit && a.mask_texture == b.mask_texture)) &&
           a.has_alpha_func == b.has_alpha_func && (!a.has_alpha_func || a.alpha_func == b.alpha_func) &&
           a.has_alpha_ref == b.has_alpha_ref && (!a.has_alpha_ref || v49_float_equal(a.alpha_ref, b.alpha_ref));
}

void v49_soft_flush_restore() {
    if (!v44_collector_pending()) {
        v49_sync_collector_key();
        return;
    }
    const v44_restore_snapshot_t snapshot = v44_snapshot_shadow();
    const unsigned long long backend_before = g_material.stats.backend_draws;
    soft_segment_flush();
    if (g_material.stats.backend_draws != backend_before) {
        ++g_v44_stats.soft_emits;
        ++g_v49_stats.aux_segments;
        v44_restore_after_emit(snapshot);
    }
    g_v49_collector_key = {};
}

void v49_hard_flush_restore(const char* reason, bool uniform_reason) {
    if (!v44_collector_pending()) {
        v49_sync_collector_key();
        return;
    }
    const v44_restore_snapshot_t snapshot = v44_snapshot_shadow();
    const unsigned long long hard_before = g_material.stats.hard_segments;
    const unsigned long long backend_before = g_material.stats.backend_draws;
    hard_flush();
    const unsigned long long delta = g_material.stats.hard_segments - hard_before;
    if (delta) {
        ++g_v49_stats.aux_hard_flush;
        if (uniform_reason)
            g_v44_stats.uniform_flushes += delta;
        else
            g_v44_stats.barrier_flushes += delta;
        v44_histogram_add(reason, delta);
    }
    if (g_material.stats.backend_draws != backend_before) v44_restore_after_emit(snapshot);
    g_v49_collector_key = {};
}

void v49_fallback(v41_reject_t reject, GLenum mode, GLsizei count, GLenum type, const void* indices) {
    const bool pending = v44_collector_pending();
    const v44_restore_snapshot_t snapshot = pending ? v44_snapshot_shadow() : v44_restore_snapshot_t{};
    const unsigned long long hard_before = g_material.stats.hard_segments;
    const unsigned long long backend_before = g_material.stats.backend_draws;
    if (pending) hard_flush();
    const unsigned long long hard_delta = g_material.stats.hard_segments - hard_before;
    if (hard_delta) {
        g_v44_stats.fallback_flushes += hard_delta;
        v44_histogram_add("glDrawElements:fallback", hard_delta);
    }
    if (pending && g_material.stats.backend_draws != backend_before) v44_restore_after_emit(snapshot);
    g_v49_collector_key = {};
    ++g_material.stats.fallback_draws;
    ++g_material.stats.backend_draws;
    ++g_v49_stats.depth_fallback;
    v41_record_reject(reject);
    g_v44_wrapped.raw_draw_elements(mode, count, type, indices);
}

void v49_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    v49_sync_collector_key();
    v49_program_t* meta = v49_ensure_depth_program(g_state.program);
    if (!meta || !meta->compatible) {
        g_v49_wrapped.draw_elements(mode, count, type, indices);
        v49_sync_collector_key();
        v49_maybe_report();
        return;
    }

    ++g_material.stats.draws_seen;
    material_program_t* program = v41_find_local_program(g_state.program);
    if (!program || !program->compatible || program->diffuse_unit < 0 || !ensure_limits() ||
        program->diffuse_unit >= g_material.max_texture_units || meta->depth_unit < 0 ||
        meta->depth_unit >= g_material.max_texture_units ||
        (meta->has_mask && (meta->mask_unit < 0 || meta->mask_unit >= g_material.max_texture_units))) {
        v49_fallback(v41_reject_t::uniform_state, mode, count, type, indices);
        v41_maybe_report();
        v44_maybe_report();
        v49_maybe_report();
        return;
    }

    candidate_t candidate{};
    v41_reject_t candidate_reject = v41_reject_t::none;
    if (!v41_build_candidate(mode, count, type, indices, &candidate, &candidate_reject)) {
        v49_fallback(candidate_reject, mode, count, type, indices);
        v41_maybe_report();
        v44_maybe_report();
        v49_maybe_report();
        return;
    }

    const GLuint diffuse_texture = v42_bound_texture(program->diffuse_unit);
    const GLuint depth_texture = v42_bound_texture(meta->depth_unit);
    const GLuint mask_texture = meta->has_mask ? v42_bound_texture(meta->mask_unit) : 0;
    if (!depth_texture || (meta->has_mask && !mask_texture)) {
        v49_fallback(v41_reject_t::texture_state, mode, count, type, indices);
        v41_maybe_report();
        v44_maybe_report();
        v49_maybe_report();
        return;
    }

    size_t bank = 0;
    GLint layer = 0;
    v41_reject_t texture_reject = v41_reject_t::none;
    if (!v42_ensure_texture_layer(program->diffuse_unit, diffuse_texture, &bank, &layer, &texture_reject) ||
        !ensure_stream_objects()) {
        v49_fallback(texture_reject == v41_reject_t::none ? v41_reject_t::stream_objects : texture_reject,
                     mode, count, type, indices);
        v41_maybe_report();
        v44_maybe_report();
        v49_maybe_report();
        return;
    }

    stream_instance_t instance{};
    if (!build_stream_instance(candidate, *program, layer, &instance)) {
        v49_fallback(v41_reject_t::vertex_mapping, mode, count, type, indices);
        v41_maybe_report();
        v44_maybe_report();
        v49_maybe_report();
        return;
    }
    instance.material[0] = program->z_depth_value;
    instance.material[1] = meta->blend_z_value;
    instance.material[2] = meta->blend_to_z_value;
    instance.material[3] = static_cast<float>(layer);

    v49_collector_key_t key{};
    key.valid = true;
    key.app_program = g_state.program;
    key.stream_program = program->stream_program;
    key.bank = bank;
    key.depth_unit = meta->depth_unit;
    key.depth_texture = depth_texture;
    key.has_mask = meta->has_mask;
    key.mask_unit = meta->has_mask ? meta->mask_unit : -1;
    key.mask_texture = mask_texture;
    key.has_alpha_func = meta->alpha_func >= 0 && meta->stream_alpha_func >= 0;
    key.alpha_func = meta->alpha_func_value;
    key.has_alpha_ref = meta->alpha_ref >= 0 && meta->stream_alpha_ref >= 0;
    key.alpha_ref = meta->alpha_ref_value;

    if (g_material.collector_active &&
        (g_material.collector_program != program->stream_program || g_material.collector_bank != bank ||
         !v49_key_equal(g_v49_collector_key, key)))
        v49_soft_flush_restore();

    if (!g_material.collector_active) {
        v49_apply_stream_state(*meta);
        g_material.collector_active = true;
        g_material.collector_program = program->stream_program;
        g_material.collector_bank = bank;
        g_v49_collector_key = key;
    }

    g_material.collector.push_back(instance);
    ++g_material.stats.draws_captured;
    ++g_material.stats.texture_array_hits;
    ++g_material.stats.instances;
    ++g_v49_stats.depth_captured;

    if (g_material.collector.size() >= kCollectorLimit) v49_soft_flush_restore();
    v41_maybe_report();
    v44_maybe_report();
    v49_maybe_report();
}

bool v49_current_meta(v49_program_t** out) {
    if (out) *out = nullptr;
    auto found = g_v49_programs.find(g_state.program);
    if (found == g_v49_programs.end() || !found->second.compatible) return false;
    if (out) *out = &found->second;
    return true;
}

void v49_glBindTexture(GLenum target, GLuint texture) {
    v49_sync_collector_key();
    if (target == GL_TEXTURE_2D && g_v49_collector_key.valid && ensure_limits()) {
        const GLint unit = static_cast<GLint>(g_material.active_texture - GL_TEXTURE0);
        if (unit >= 0 && unit < g_material.max_texture_units) {
            const GLuint current = g_material.bound_2d[static_cast<size_t>(unit)];
            const bool aux = unit == g_v49_collector_key.depth_unit ||
                             (g_v49_collector_key.has_mask && unit == g_v49_collector_key.mask_unit);
            if (aux && current != texture) v49_hard_flush_restore("glBindTexture:depth_mask", false);
        }
    }
    g_v49_wrapped.bind_texture(target, texture);
}

void v49_glBindSampler(GLuint unit, GLuint sampler) {
    v49_sync_collector_key();
    if (g_v49_collector_key.valid &&
        (static_cast<GLint>(unit) == g_v49_collector_key.depth_unit ||
         (g_v49_collector_key.has_mask && static_cast<GLint>(unit) == g_v49_collector_key.mask_unit)))
        v49_hard_flush_restore("glBindSampler:depth_mask", false);
    g_v49_wrapped.bind_sampler(unit, sampler);
}

void v49_update_float(v49_program_t& meta, GLint location, GLfloat value, bool current_program) {
    if (location == meta.blend_z) {
        meta.blend_z_value = value;
        ++g_v49_stats.blend_updates;
        return;
    }
    if (location == meta.blend_to_z) {
        meta.blend_to_z_value = value;
        ++g_v49_stats.blend_updates;
        return;
    }
    if (location == meta.alpha_ref) {
        if (current_program && !v49_float_equal(meta.alpha_ref_value, value))
            v49_hard_flush_restore("v49:alpha_ref", true);
        meta.alpha_ref_value = value;
        ++g_v49_stats.alpha_updates;
    }
}

void v49_update_int(v49_program_t& meta, GLint location, GLint value, bool current_program) {
    if (location == meta.depth) {
        if (current_program && meta.depth_unit != value) v49_hard_flush_restore("v49:depth_sampler", true);
        meta.depth_unit = value;
        ++g_v49_stats.sampler_updates;
        return;
    }
    if (meta.has_mask && location == meta.mask) {
        if (current_program && meta.mask_unit != value) v49_hard_flush_restore("v49:mask_sampler", true);
        meta.mask_unit = value;
        ++g_v49_stats.sampler_updates;
        return;
    }
    if (location == meta.alpha_func) {
        if (current_program && meta.alpha_func_value != value) v49_hard_flush_restore("v49:alpha_func", true);
        meta.alpha_func_value = value;
        ++g_v49_stats.alpha_updates;
    }
}

bool v49_special_float(v49_program_t* meta, GLint location) {
    return meta && (location == meta->blend_z || location == meta->blend_to_z || location == meta->alpha_ref);
}

bool v49_special_int(v49_program_t* meta, GLint location) {
    return meta && (location == meta->depth || (meta->has_mask && location == meta->mask) || location == meta->alpha_func);
}

void v49_glUniform1f(GLint location, GLfloat value) {
    v49_program_t* meta = nullptr;
    if (v49_current_meta(&meta) && v49_special_float(meta, location)) {
        v49_update_float(*meta, location, value, true);
        g_material_wrapped.uniform1f(location, value);
        return;
    }
    g_v49_wrapped.uniform1f(location, value);
}

void v49_glUniform1fv(GLint location, GLsizei count, const GLfloat* value) {
    v49_program_t* meta = nullptr;
    if (count == 1 && value && v49_current_meta(&meta) && v49_special_float(meta, location)) {
        v49_update_float(*meta, location, value[0], true);
        g_v41_wrapped.uniform1fv(location, count, value);
        return;
    }
    g_v49_wrapped.uniform1fv(location, count, value);
}

void v49_glUniform1i(GLint location, GLint value) {
    v49_program_t* meta = nullptr;
    if (v49_current_meta(&meta) && v49_special_int(meta, location)) {
        v49_update_int(*meta, location, value, true);
        g_material_wrapped.uniform1i(location, value);
        return;
    }
    g_v49_wrapped.uniform1i(location, value);
}

void v49_glUniform1iv(GLint location, GLsizei count, const GLint* value) {
    v49_program_t* meta = nullptr;
    if (count == 1 && value && v49_current_meta(&meta) && v49_special_int(meta, location)) {
        v49_update_int(*meta, location, value[0], true);
        g_v41_wrapped.uniform1iv(location, count, value);
        return;
    }
    g_v49_wrapped.uniform1iv(location, count, value);
}

void v49_glProgramUniform1f(GLuint program, GLint location, GLfloat value) {
    auto found = g_v49_programs.find(program);
    if (found != g_v49_programs.end() && found->second.compatible && v49_special_float(&found->second, location)) {
        v49_update_float(found->second, location, value, program == g_state.program);
        g_v41_wrapped.program_uniform1f(program, location, value);
        return;
    }
    g_v49_wrapped.program_uniform1f(program, location, value);
}

void v49_glProgramUniform1fv(GLuint program, GLint location, GLsizei count, const GLfloat* value) {
    auto found = g_v49_programs.find(program);
    if (count == 1 && value && found != g_v49_programs.end() && found->second.compatible &&
        v49_special_float(&found->second, location)) {
        v49_update_float(found->second, location, value[0], program == g_state.program);
        g_v41_wrapped.program_uniform1fv(program, location, count, value);
        return;
    }
    g_v49_wrapped.program_uniform1fv(program, location, count, value);
}

void v49_glProgramUniform1i(GLuint program, GLint location, GLint value) {
    auto found = g_v49_programs.find(program);
    if (found != g_v49_programs.end() && found->second.compatible && v49_special_int(&found->second, location)) {
        v49_update_int(found->second, location, value, program == g_state.program);
        g_v41_wrapped.program_uniform1i(program, location, value);
        return;
    }
    g_v49_wrapped.program_uniform1i(program, location, value);
}

void v49_glProgramUniform1iv(GLuint program, GLint location, GLsizei count, const GLint* value) {
    auto found = g_v49_programs.find(program);
    if (count == 1 && value && found != g_v49_programs.end() && found->second.compatible &&
        v49_special_int(&found->second, location)) {
        v49_update_int(found->second, location, value[0], program == g_state.program);
        g_v41_wrapped.program_uniform1iv(program, location, count, value);
        return;
    }
    g_v49_wrapped.program_uniform1iv(program, location, count, value);
}

void v49_glLinkProgram(GLuint program) {
    g_v49_wrapped.link_program(program);
    g_v49_programs.erase(program);
    if (g_v49_collector_key.valid && g_v49_collector_key.app_program == program) g_v49_collector_key = {};
}

void v49_glDeleteProgram(GLuint program) {
    if (g_v49_collector_key.valid && g_v49_collector_key.app_program == program) v49_soft_flush_restore();
    g_v49_programs.erase(program);
    g_v49_wrapped.delete_program(program);
}

void v49_install_impl() {
    if (!g_v41_enabled || !g_material_enabled || !g_enabled || !g_v44_enabled) return;

    g_v49_wrapped.draw_elements = static_cast<glDrawElements_PTR>(GLES.glDrawElements);
    g_v49_wrapped.bind_texture = static_cast<glBindTexture_PTR>(GLES.glBindTexture);
    g_v49_wrapped.bind_sampler = static_cast<glBindSampler_PTR>(GLES.glBindSampler);
    g_v49_wrapped.uniform1f = static_cast<glUniform1f_PTR>(GLES.glUniform1f);
    g_v49_wrapped.uniform1fv = static_cast<glUniform1fv_PTR>(GLES.glUniform1fv);
    g_v49_wrapped.uniform1i = static_cast<glUniform1i_PTR>(GLES.glUniform1i);
    g_v49_wrapped.uniform1iv = static_cast<glUniform1iv_PTR>(GLES.glUniform1iv);
    g_v49_wrapped.program_uniform1f = static_cast<glProgramUniform1f_PTR>(GLES.glProgramUniform1f);
    g_v49_wrapped.program_uniform1fv = static_cast<glProgramUniform1fv_PTR>(GLES.glProgramUniform1fv);
    g_v49_wrapped.program_uniform1i = static_cast<glProgramUniform1i_PTR>(GLES.glProgramUniform1i);
    g_v49_wrapped.program_uniform1iv = static_cast<glProgramUniform1iv_PTR>(GLES.glProgramUniform1iv);
    g_v49_wrapped.link_program = static_cast<glLinkProgram_PTR>(GLES.glLinkProgram);
    g_v49_wrapped.delete_program = static_cast<glDeleteProgram_PTR>(GLES.glDeleteProgram);

    if (!g_v49_wrapped.draw_elements || !g_v49_wrapped.bind_texture || !g_v49_wrapped.uniform1f ||
        !g_v49_wrapped.uniform1i || !g_v49_wrapped.link_program || !g_v49_wrapped.delete_program) {
        LOG_W_FORCE("ZOMDROID_PZ_MATERIAL_STREAM_V49 disabled reason=wrapper_capture_missing")
        return;
    }

    g_v49_enabled = true;
    GLES.glDrawElements = v49_glDrawElements;
    GLES.glBindTexture = v49_glBindTexture;
    if (g_v49_wrapped.bind_sampler) GLES.glBindSampler = v49_glBindSampler;
    GLES.glUniform1f = v49_glUniform1f;
    if (g_v49_wrapped.uniform1fv) GLES.glUniform1fv = v49_glUniform1fv;
    GLES.glUniform1i = v49_glUniform1i;
    if (g_v49_wrapped.uniform1iv) GLES.glUniform1iv = v49_glUniform1iv;
    if (g_v49_wrapped.program_uniform1f && g_v41_wrapped.program_uniform1f)
        GLES.glProgramUniform1f = v49_glProgramUniform1f;
    if (g_v49_wrapped.program_uniform1fv && g_v41_wrapped.program_uniform1fv)
        GLES.glProgramUniform1fv = v49_glProgramUniform1fv;
    if (g_v49_wrapped.program_uniform1i && g_v41_wrapped.program_uniform1i)
        GLES.glProgramUniform1i = v49_glProgramUniform1i;
    if (g_v49_wrapped.program_uniform1iv && g_v41_wrapped.program_uniform1iv)
        GLES.glProgramUniform1iv = v49_glProgramUniform1iv;
    GLES.glLinkProgram = v49_glLinkProgram;
    GLES.glDeleteProgram = v49_glDeleteProgram;

    LOG_I("ZOMDROID_PZ_MATERIAL_STREAM_V49 enabled=1 revision=4.9 mode=perf_ceiling "
          "family=tile_depth diffuse=array depth=bound mask=bound blend=instance alpha=segment "
          "ui_guard=none stable_untouched=1")
}

} // namespace

void install_v49_internal() { v49_install_impl(); }

} // namespace v41_base
} // namespace legacy_v41
} // namespace v43_base
} // namespace v44_base

void mg_pz_material_stream_v49_install(void) {
    v44_base::v43_base::legacy_v41::v41_base::install_v49_internal();
}

#else

void mg_pz_material_stream_v49_install(void) {}

#endif
