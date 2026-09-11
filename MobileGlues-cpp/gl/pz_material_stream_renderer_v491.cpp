// MobileGlues - gl/pz_material_stream_renderer_v491.cpp
// Project Zomboid V4.9.1 perf-ceiling overlay: fix the tile-depth alpha-state
// contract discovered by V4.9 runtime telemetry. V4.9 is retained verbatim;
// this layer adds zomdroidAlphaEnabled as ordered segment state and reports
// precise classifier reject stages. UI correctness remains out of scope.

#include "pz_material_stream_renderer_v49.cpp"

#if defined(ZOMDROID_EXPERIMENTAL)

namespace v44_base {
namespace v43_base {
namespace legacy_v41 {
namespace v41_base {
namespace {

struct v491_program_t {
    bool attempted = false;
    bool compatible = false;
    unsigned long long generation = 0;
    GLint alpha_enabled = -1;
    GLint stream_alpha_enabled = -1;
    GLint alpha_enabled_value = 0;
};

struct v491_wrapped_t {
    glUniform1i_PTR uniform1i = nullptr;
    glUniform1iv_PTR uniform1iv = nullptr;
    glProgramUniform1i_PTR program_uniform1i = nullptr;
    glProgramUniform1iv_PTR program_uniform1iv = nullptr;
    glLinkProgram_PTR link_program = nullptr;
    glDeleteProgram_PTR delete_program = nullptr;
};

thread_local std::unordered_map<GLuint, v491_program_t> g_v491_programs;
thread_local bool g_v491_collector_alpha_valid = false;
thread_local GLint g_v491_collector_alpha_enabled = 0;
v491_wrapped_t g_v491_wrapped;
bool g_v491_enabled = false;

void v491_sync_collector_state() {
    v49_sync_collector_key();
    if (!g_material.collector_active || g_material.collector.empty()) {
        g_v491_collector_alpha_valid = false;
        g_v491_collector_alpha_enabled = 0;
    }
}

void v491_reject(GLuint program, const char* stage, const char* reason) {
    LOG_I("ZOMDROID_PZ_MATERIAL_STREAM_V491_PROGRAM program=%u result=reject stage=%s reason=%s",
          program, stage ? stage : "unknown", reason ? reason : "unknown")
}

bool v491_transform_depth_fragment(const std::string& original, std::string* transformed, bool* has_mask,
                                   const char** reject_reason) {
    if (has_mask) *has_mask = false;
    if (reject_reason) *reject_reason = "none";
    if (!transformed) {
        if (reject_reason) *reject_reason = "output";
        return false;
    }
    if (!only_uniforms(original, {"DIFFUSE", "DEPTH", "MASK", "zDepthBlendZ", "zDepthBlendToZ",
                                  "zomdroidAlphaEnabled", "zomdroidAlphaFunc", "zomdroidAlphaRef"})) {
        if (reject_reason) *reject_reason = "uniforms";
        return false;
    }

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
        regex_count(original, main_decl) != 1) {
        if (reject_reason) *reject_reason = "contract";
        return false;
    }

    std::string source = original;
    if (!replace_unique(&source, diffuse_decl, "uniform highp sampler2DArray zomdroidStreamDiffuse;") ||
        !replace_unique(&source, blend_z_decl, "float zDepthBlendZ;") ||
        !replace_unique(&source, blend_to_decl, "float zDepthBlendToZ;") ||
        !v42_rewrite_diffuse_texture_call(&source)) {
        if (reject_reason) *reject_reason = "rewrite";
        return false;
    }

    const std::string declarations =
        "flat in highp int zomdroidStreamLayer;\n"
        "flat in highp float zomdroidStreamBlendZ;\n"
        "flat in highp float zomdroidStreamBlendToZ;";
    if (!insert_after_precision(&source, declarations)) {
        if (reject_reason) *reject_reason = "precision_inject";
        return false;
    }

    std::smatch main_match;
    if (!std::regex_search(source, main_match, main_decl)) {
        if (reject_reason) *reject_reason = "main";
        return false;
    }
    const size_t body = static_cast<size_t>(main_match.position() + main_match.length());
    source.insert(body,
                  "\n    zDepthBlendZ = zomdroidStreamBlendZ;\n"
                  "    zDepthBlendToZ = zomdroidStreamBlendToZ;\n");

    if (has_mask) *has_mask = mask_count == 1;
    *transformed = std::move(source);
    return true;
}

v49_program_t* v491_ensure_depth_program(GLuint program_id, v491_program_t** extra_out) {
    if (extra_out) *extra_out = nullptr;
    if (!g_v491_enabled || program_id == 0) return nullptr;

    auto extra_cached = g_v491_programs.find(program_id);
    if (extra_cached != g_v491_programs.end()) {
        auto base_cached = g_v49_programs.find(program_id);
        if (extra_cached->second.compatible && base_cached != g_v49_programs.end() && base_cached->second.compatible) {
            if (extra_out) *extra_out = &extra_cached->second;
            return &base_cached->second;
        }
        return nullptr;
    }

    v491_program_t extra{};
    extra.attempted = true;
    v49_program_t meta{};
    meta.attempted = true;
    meta.app_program = program_id;

    v41_program_record_t source{};
    if (!v41_registry_program(program_id, &source)) {
        ++g_v49_stats.programs_reject;
        v491_reject(program_id, "registry", "missing");
        g_v491_programs.emplace(program_id, extra);
        return nullptr;
    }
    if (!source.linked || !source.complete || !source.identity_safe || !ensure_limits()) {
        ++g_v49_stats.programs_reject;
        v491_reject(program_id, "registry", !source.linked ? "unlinked" : (!source.complete ? "incomplete" :
                    (!source.identity_safe ? "identity" : "limits")));
        g_v491_programs.emplace(program_id, extra);
        return nullptr;
    }
    meta.generation = source.generation;
    extra.generation = source.generation;

    std::string stream_vertex;
    if (!v49_transform_depth_vertex(source.vertex, static_cast<GLint>(g_material.ssbo_binding), &stream_vertex)) {
        ++g_v49_stats.programs_reject;
        v491_reject(program_id, "vs", "depth_contract");
        g_v491_programs.emplace(program_id, extra);
        return nullptr;
    }

    std::string stream_fragment;
    bool has_mask = false;
    const char* fs_reject = "unknown";
    if (!v491_transform_depth_fragment(source.fragment, &stream_fragment, &has_mask, &fs_reject)) {
        ++g_v49_stats.programs_reject;
        v491_reject(program_id, "fs", fs_reject);
        g_v491_programs.emplace(program_id, extra);
        return nullptr;
    }

    material_program_t info{};
    info.classified = true;
    info.stream_program = stream_variant_for(stream_vertex, stream_fragment);
    if (!info.stream_program) {
        ++g_v49_stats.programs_reject;
        v491_reject(program_id, "compiler", "stream_variant");
        g_v491_programs.emplace(program_id, extra);
        return nullptr;
    }
    if (!g_v41_wrapped.get_uniformfv || !g_v41_wrapped.get_uniformiv) {
        ++g_v49_stats.programs_reject;
        v491_reject(program_id, "uniform_state", "query_missing");
        g_v491_programs.emplace(program_id, extra);
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
    extra.alpha_enabled = g_material_backend.get_uniform_location(program_id, "zomdroidAlphaEnabled");
    meta.alpha_func = g_material_backend.get_uniform_location(program_id, "zomdroidAlphaFunc");
    meta.alpha_ref = g_material_backend.get_uniform_location(program_id, "zomdroidAlphaRef");

    meta.stream_program = info.stream_program;
    meta.stream_depth = g_material_backend.get_uniform_location(info.stream_program, "DEPTH");
    meta.stream_mask = has_mask ? g_material_backend.get_uniform_location(info.stream_program, "MASK") : -1;
    extra.stream_alpha_enabled = g_material_backend.get_uniform_location(info.stream_program, "zomdroidAlphaEnabled");
    meta.stream_alpha_func = g_material_backend.get_uniform_location(info.stream_program, "zomdroidAlphaFunc");
    meta.stream_alpha_ref = g_material_backend.get_uniform_location(info.stream_program, "zomdroidAlphaRef");

    const bool base_locations_ok = info.mvp >= 0 && info.z_depth >= 0 && info.diffuse >= 0 &&
                                   meta.blend_z >= 0 && meta.blend_to_z >= 0 && meta.depth >= 0 &&
                                   meta.stream_depth >= 0 &&
                                   (!has_mask || (meta.mask >= 0 && meta.stream_mask >= 0));
    const bool app_alpha_any = extra.alpha_enabled >= 0 || meta.alpha_func >= 0 || meta.alpha_ref >= 0;
    const bool stream_alpha_any = extra.stream_alpha_enabled >= 0 || meta.stream_alpha_func >= 0 ||
                                  meta.stream_alpha_ref >= 0;
    const bool alpha_locations_ok = (!app_alpha_any && !stream_alpha_any) ||
                                    (extra.alpha_enabled >= 0 && meta.alpha_func >= 0 && meta.alpha_ref >= 0 &&
                                     extra.stream_alpha_enabled >= 0 && meta.stream_alpha_func >= 0 &&
                                     meta.stream_alpha_ref >= 0);
    if (!base_locations_ok || !alpha_locations_ok) {
        ++g_v49_stats.programs_reject;
        v491_reject(program_id, "locations", !base_locations_ok ? "base" : "alpha_triple");
        g_v491_programs.emplace(program_id, extra);
        return nullptr;
    }

    g_v41_wrapped.get_uniformfv(program_id, info.mvp, info.mvp_value.data());
    g_v41_wrapped.get_uniformfv(program_id, info.z_depth, &info.z_depth_value);
    g_v41_wrapped.get_uniformiv(program_id, info.diffuse, &info.diffuse_unit);
    g_v41_wrapped.get_uniformfv(program_id, meta.blend_z, &meta.blend_z_value);
    g_v41_wrapped.get_uniformfv(program_id, meta.blend_to_z, &meta.blend_to_z_value);
    g_v41_wrapped.get_uniformiv(program_id, meta.depth, &meta.depth_unit);
    if (meta.has_mask) g_v41_wrapped.get_uniformiv(program_id, meta.mask, &meta.mask_unit);
    if (extra.alpha_enabled >= 0)
        g_v41_wrapped.get_uniformiv(program_id, extra.alpha_enabled, &extra.alpha_enabled_value);
    if (meta.alpha_func >= 0) g_v41_wrapped.get_uniformiv(program_id, meta.alpha_func, &meta.alpha_func_value);
    if (meta.alpha_ref >= 0) g_v41_wrapped.get_uniformfv(program_id, meta.alpha_ref, &meta.alpha_ref_value);

    const bool units_ok = info.diffuse_unit >= 0 && info.diffuse_unit < g_material.max_texture_units &&
                          meta.depth_unit >= 0 && meta.depth_unit < g_material.max_texture_units &&
                          (!meta.has_mask || (meta.mask_unit >= 0 && meta.mask_unit < g_material.max_texture_units));
    if (!units_ok) {
        ++g_v49_stats.programs_reject;
        v491_reject(program_id, "units", "sampler_range");
        g_v491_programs.emplace(program_id, extra);
        return nullptr;
    }

    info.compatible = true;
    meta.compatible = true;
    extra.compatible = true;
    g_material.programs[program_id] = info;
    g_v41_local_programs[program_id] = v41_local_program_t{source.generation, v41_reject_t::none};
    ++g_v41_stats.classified_ok;
    ++g_v49_stats.programs_ok;
    auto base_inserted = g_v49_programs.insert_or_assign(program_id, meta).first;
    auto extra_inserted = g_v491_programs.emplace(program_id, extra).first;
    LOG_I("ZOMDROID_PZ_MATERIAL_STREAM_V491_PROGRAM program=%u result=compatible family=tile_depth mask=%d "
          "units=diffuse:%d/depth:%d/mask:%d alpha=%d/%d/%d",
          program_id, meta.has_mask ? 1 : 0, info.diffuse_unit, meta.depth_unit,
          meta.has_mask ? meta.mask_unit : -1, extra.alpha_enabled >= 0 ? 1 : 0,
          meta.alpha_func >= 0 ? 1 : 0, meta.alpha_ref >= 0 ? 1 : 0)
    if (extra_out) *extra_out = &extra_inserted->second;
    return &base_inserted->second;
}

void v491_apply_alpha_enabled(const v49_program_t& meta, const v491_program_t& extra) {
    if (!meta.compatible || meta.stream_program == 0 || extra.stream_alpha_enabled < 0) return;
    const GLuint restore = g_state.program;
    g_orig.use_program(meta.stream_program);
    g_material_backend.uniform1i(extra.stream_alpha_enabled, extra.alpha_enabled_value);
    g_orig.use_program(restore);
}

void v491_soft_flush_restore() {
    v49_soft_flush_restore();
    g_v491_collector_alpha_valid = false;
    g_v491_collector_alpha_enabled = 0;
}

void v491_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    v491_sync_collector_state();
    v491_program_t* extra = nullptr;
    v49_program_t* meta = v491_ensure_depth_program(g_state.program, &extra);
    if (!meta || !meta->compatible || !extra || !extra->compatible) {
        // Bypass the V4.9 classifier on misses. Its old alpha contract is known
        // to reject this family; the captured predecessor is the proven V4.8/V4.4 route.
        g_v49_wrapped.draw_elements(mode, count, type, indices);
        v491_sync_collector_state();
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

    const bool alpha_enabled_relevant = extra->alpha_enabled >= 0 && extra->stream_alpha_enabled >= 0;
    const bool alpha_enabled_mismatch = g_material.collector_active && alpha_enabled_relevant &&
        (!g_v491_collector_alpha_valid || g_v491_collector_alpha_enabled != extra->alpha_enabled_value);
    if (g_material.collector_active &&
        (g_material.collector_program != program->stream_program || g_material.collector_bank != bank ||
         !v49_key_equal(g_v49_collector_key, key) || alpha_enabled_mismatch))
        v491_soft_flush_restore();

    if (!g_material.collector_active) {
        v49_apply_stream_state(*meta);
        v491_apply_alpha_enabled(*meta, *extra);
        g_material.collector_active = true;
        g_material.collector_program = program->stream_program;
        g_material.collector_bank = bank;
        g_v49_collector_key = key;
        g_v491_collector_alpha_valid = alpha_enabled_relevant;
        g_v491_collector_alpha_enabled = extra->alpha_enabled_value;
    }

    g_material.collector.push_back(instance);
    ++g_material.stats.draws_captured;
    ++g_material.stats.texture_array_hits;
    ++g_material.stats.instances;
    ++g_v49_stats.depth_captured;

    if (g_material.collector.size() >= kCollectorLimit) v491_soft_flush_restore();
    v41_maybe_report();
    v44_maybe_report();
    v49_maybe_report();
}

bool v491_current_extra(v491_program_t** out) {
    if (out) *out = nullptr;
    auto found = g_v491_programs.find(g_state.program);
    if (found == g_v491_programs.end() || !found->second.compatible) return false;
    if (out) *out = &found->second;
    return true;
}

void v491_update_alpha_enabled(v491_program_t& extra, GLint value, bool current_program) {
    v491_sync_collector_state();
    if (current_program && extra.alpha_enabled_value != value) {
        v49_hard_flush_restore("v491:alpha_enabled", true);
        g_v491_collector_alpha_valid = false;
        g_v491_collector_alpha_enabled = 0;
    }
    extra.alpha_enabled_value = value;
    ++g_v49_stats.alpha_updates;
}

void v491_glUniform1i(GLint location, GLint value) {
    v491_program_t* extra = nullptr;
    if (v491_current_extra(&extra) && location == extra->alpha_enabled) {
        v491_update_alpha_enabled(*extra, value, true);
        g_material_wrapped.uniform1i(location, value);
        return;
    }
    g_v491_wrapped.uniform1i(location, value);
}

void v491_glUniform1iv(GLint location, GLsizei count, const GLint* value) {
    v491_program_t* extra = nullptr;
    if (count == 1 && value && v491_current_extra(&extra) && location == extra->alpha_enabled) {
        v491_update_alpha_enabled(*extra, value[0], true);
        g_v41_wrapped.uniform1iv(location, count, value);
        return;
    }
    g_v491_wrapped.uniform1iv(location, count, value);
}

void v491_glProgramUniform1i(GLuint program, GLint location, GLint value) {
    auto found = g_v491_programs.find(program);
    if (found != g_v491_programs.end() && found->second.compatible && location == found->second.alpha_enabled) {
        v491_update_alpha_enabled(found->second, value, program == g_state.program);
        g_v41_wrapped.program_uniform1i(program, location, value);
        return;
    }
    g_v491_wrapped.program_uniform1i(program, location, value);
}

void v491_glProgramUniform1iv(GLuint program, GLint location, GLsizei count, const GLint* value) {
    auto found = g_v491_programs.find(program);
    if (count == 1 && value && found != g_v491_programs.end() && found->second.compatible &&
        location == found->second.alpha_enabled) {
        v491_update_alpha_enabled(found->second, value[0], program == g_state.program);
        g_v41_wrapped.program_uniform1iv(program, location, count, value);
        return;
    }
    g_v491_wrapped.program_uniform1iv(program, location, count, value);
}

void v491_glLinkProgram(GLuint program) {
    g_v491_wrapped.link_program(program);
    g_v491_programs.erase(program);
    g_v491_collector_alpha_valid = false;
}

void v491_glDeleteProgram(GLuint program) {
    g_v491_programs.erase(program);
    g_v491_wrapped.delete_program(program);
    g_v491_collector_alpha_valid = false;
}

void v491_install_impl() {
    ::mg_pz_material_stream_v49_install();
    if (!g_v49_enabled || !g_v41_enabled || !g_material_enabled || !g_enabled || !g_v44_enabled) return;

    g_v491_wrapped.uniform1i = static_cast<glUniform1i_PTR>(GLES.glUniform1i);
    g_v491_wrapped.uniform1iv = static_cast<glUniform1iv_PTR>(GLES.glUniform1iv);
    g_v491_wrapped.program_uniform1i = static_cast<glProgramUniform1i_PTR>(GLES.glProgramUniform1i);
    g_v491_wrapped.program_uniform1iv = static_cast<glProgramUniform1iv_PTR>(GLES.glProgramUniform1iv);
    g_v491_wrapped.link_program = static_cast<glLinkProgram_PTR>(GLES.glLinkProgram);
    g_v491_wrapped.delete_program = static_cast<glDeleteProgram_PTR>(GLES.glDeleteProgram);

    if (!g_v491_wrapped.uniform1i || !g_v491_wrapped.link_program || !g_v491_wrapped.delete_program) {
        LOG_W_FORCE("ZOMDROID_PZ_MATERIAL_STREAM_V491 disabled reason=wrapper_capture_missing")
        return;
    }

    g_v491_enabled = true;
    GLES.glDrawElements = v491_glDrawElements;
    GLES.glUniform1i = v491_glUniform1i;
    if (g_v491_wrapped.uniform1iv && g_v41_wrapped.uniform1iv) GLES.glUniform1iv = v491_glUniform1iv;
    if (g_v491_wrapped.program_uniform1i && g_v41_wrapped.program_uniform1i)
        GLES.glProgramUniform1i = v491_glProgramUniform1i;
    if (g_v491_wrapped.program_uniform1iv && g_v41_wrapped.program_uniform1iv)
        GLES.glProgramUniform1iv = v491_glProgramUniform1iv;
    GLES.glLinkProgram = v491_glLinkProgram;
    GLES.glDeleteProgram = v491_glDeleteProgram;

    LOG_I("ZOMDROID_PZ_MATERIAL_STREAM_V491 enabled=1 revision=4.9.1 mode=perf_ceiling "
          "base=v49 fix=alpha_enabled_segment alpha=enabled+func+ref reject_telemetry=stage_reason "
          "depth_order=preserved ui_guard=none stable_untouched=1")
}

} // namespace

void install_v491_internal() { v491_install_impl(); }

} // namespace v41_base
} // namespace legacy_v41
} // namespace v43_base
} // namespace v44_base

void mg_pz_material_stream_v491_install(void) {
    v44_base::v43_base::legacy_v41::v41_base::install_v491_internal();
}

#else

void mg_pz_material_stream_v491_install(void) {}

#endif
