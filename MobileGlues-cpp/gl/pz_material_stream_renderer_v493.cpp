// MobileGlues - gl/pz_material_stream_renderer_v493.cpp
// Project Zomboid V4.9.3: carry the proven tile-depth auxiliary vertex UVs
// through a depth-only SSBO sidecar. V4.9.2 remains the fallback/probe route.
// Eligibility stays exact: location 3 must be vec2, location 4 is accepted only
// for the MASK family, and all original ordering/state segmentation is retained.

#include "pz_material_stream_renderer_v492.cpp"

#if defined(ZOMDROID_EXPERIMENTAL)

namespace v44_base {
namespace v43_base {
namespace legacy_v41 {
namespace v41_base {
namespace {

struct v493_program_t {
    bool attempted = false;
    bool compatible = false;
    unsigned long long generation = 0;
    unsigned aux_count = 0;
    GLuint seeded_vao = std::numeric_limits<GLuint>::max();
};

struct v493_wrapped_t {
    glDrawElements_PTR draw_elements = nullptr;
    glLinkProgram_PTR link_program = nullptr;
    glDeleteProgram_PTR delete_program = nullptr;
};

struct v493_stats_t {
    unsigned long long tile_depth_draws = 0;
    unsigned long long no_depth_passthrough = 0;
};

thread_local std::unordered_map<GLuint, v493_program_t> g_v493_programs;
thread_local v493_stats_t g_v493_stats;
v493_wrapped_t g_v493_wrapped;
bool g_v493_enabled = false;

void v493_maybe_report_world_only() {
    const unsigned long long total = g_v493_stats.tile_depth_draws + g_v493_stats.no_depth_passthrough;
    if (total == 1 || total == 65536 || (total != 0 && total % 250000ULL == 0))
        LOG_I("ZOMDROID_PZ_MATERIAL_STREAM_V493_WORLD_ONLY tile_depth=%llu no_depth_passthrough=%llu",
              g_v493_stats.tile_depth_draws, g_v493_stats.no_depth_passthrough)
}

void v493_passthrough_no_depth(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    // V4.9.3 is the first layer with the exact PZ tile-depth shader contract.
    // A miss here must not fall through to V4.8, which would capture the broad
    // no-depth family (predominantly menus/UI in the measured workload).
    // Flush an existing world collector first to preserve draw ordering, then
    // submit the application draw through the original renderer path.
    v491_sync_collector_state();
    if (v44_collector_pending()) {
        v49_hard_flush_restore("v493:world_only_passthrough", false);
        v491_sync_collector_state();
    }

    ++g_material.stats.draws_seen;
    ++g_material.stats.fallback_draws;
    ++g_material.stats.backend_draws;
    ++g_v493_stats.no_depth_passthrough;
    g_v44_wrapped.raw_draw_elements(mode, count, type, indices);
    v41_maybe_report();
    v44_maybe_report();
    v49_maybe_report();
    v493_maybe_report_world_only();
}

void v493_reject(GLuint program, const char* stage, const char* reason) {
    LOG_I("ZOMDROID_PZ_MATERIAL_STREAM_V493_PROGRAM program=%u result=reject stage=%s reason=%s",
          program, stage ? stage : "unknown", reason ? reason : "unknown")
}

bool v493_transform_depth_vertex(const std::string& original, GLint ssbo_binding, GLint aux_binding,
                                 std::string* transformed, unsigned* aux_count, const char** reject_reason) {
    if (aux_count) *aux_count = 0;
    if (reject_reason) *reject_reason = "none";
    if (!transformed || ssbo_binding < 0 || aux_binding < 0 || ssbo_binding == aux_binding) {
        if (reject_reason) *reject_reason = "bindings";
        return false;
    }
    if (!only_uniforms(original, {"ModelViewProjection", "zDepth", "zDepthBlendZ", "zDepthBlendToZ"})) {
        if (reject_reason) *reject_reason = "uniforms";
        return false;
    }

    static const std::regex pos_decl(
        R"(layout\s*\(\s*location\s*=\s*0\s*\)\s*in\s+(?:(?:lowp|mediump|highp)\s+)?vec2\s+vPos\s*;)");
    static const std::regex uv_decl(
        R"(layout\s*\(\s*location\s*=\s*1\s*\)\s*in\s+(?:(?:lowp|mediump|highp)\s+)?vec2\s+vUV\s*;)");
    static const std::regex color_decl(
        R"(layout\s*\(\s*location\s*=\s*2\s*\)\s*in\s+(?:(?:lowp|mediump|highp)\s+)?vec4\s+vCol\s*;)");
    static const std::regex aux3_decl(
        R"(layout\s*\(\s*location\s*=\s*3\s*\)\s*in\s+(?:(?:lowp|mediump|highp)\s+)?vec2\s+([A-Za-z_]\w*)\s*;)");
    static const std::regex aux4_decl(
        R"(layout\s*\(\s*location\s*=\s*4\s*\)\s*in\s+(?:(?:lowp|mediump|highp)\s+)?vec2\s+([A-Za-z_]\w*)\s*;)");
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

    const size_t inputs = regex_count(original, any_input);
    if ((inputs != 4 && inputs != 5) || regex_count(original, pos_decl) != 1 ||
        regex_count(original, uv_decl) != 1 || regex_count(original, color_decl) != 1 ||
        regex_count(original, aux3_decl) != 1 || (inputs == 5 ? regex_count(original, aux4_decl) != 1
                                                               : regex_count(original, aux4_decl) != 0)) {
        if (reject_reason) *reject_reason = "inputs";
        return false;
    }
    if (regex_count(original, mvp_decl) != 1 || regex_count(original, z_decl) != 1 ||
        regex_count(original, blend_z_decl) != 1 || regex_count(original, blend_to_decl) != 1 ||
        regex_count(original, main_decl) != 1) {
        if (reject_reason) *reject_reason = "uniform_contract";
        return false;
    }

    std::smatch aux3_match;
    std::smatch aux4_match;
    if (!std::regex_search(original, aux3_match, aux3_decl) || aux3_match.size() < 2) {
        if (reject_reason) *reject_reason = "aux3_name";
        return false;
    }
    const std::string aux3_name = aux3_match[1].str();
    std::string aux4_name;
    if (inputs == 5) {
        if (!std::regex_search(original, aux4_match, aux4_decl) || aux4_match.size() < 2) {
            if (reject_reason) *reject_reason = "aux4_name";
            return false;
        }
        aux4_name = aux4_match[1].str();
    }

    std::string source = original;
    if (!replace_unique(&source, pos_decl, "vec2 vPos;") ||
        !replace_unique(&source, uv_decl, "vec2 vUV;") ||
        !replace_unique(&source, color_decl, "vec4 vCol;") ||
        !replace_unique(&source, aux3_decl, "vec2 " + aux3_name + ";") ||
        (inputs == 5 && !replace_unique(&source, aux4_decl, "vec2 " + aux4_name + ";")) ||
        !replace_unique(&source, mvp_decl, "mat4 ModelViewProjection;") ||
        !replace_unique(&source, z_decl, "float zDepth;") ||
        !replace_unique(&source, blend_z_decl, "float zDepthBlendZ;") ||
        !replace_unique(&source, blend_to_decl, "float zDepthBlendToZ;")) {
        if (reject_reason) *reject_reason = "rewrite";
        return false;
    }

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
        "struct ZomdroidDepthAuxInstance {\n"
        "    vec4 uv[6];\n"
        "};\n"
        "layout(std430, binding = " + std::to_string(aux_binding) +
        ") readonly buffer ZomdroidDepthAuxBlock {\n"
        "    ZomdroidDepthAuxInstance zomdroidDepthAux[];\n"
        "};\n"
        "flat out highp int zomdroidStreamLayer;\n"
        "flat out highp float zomdroidStreamBlendZ;\n"
        "flat out highp float zomdroidStreamBlendToZ;";
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
    std::string setup =
        "\n    ZomdroidStreamInstance zomdroidStream = zomdroidInstances[gl_InstanceID];\n"
        "    ZomdroidDepthAuxInstance zomdroidAux = zomdroidDepthAux[gl_InstanceID];\n"
        "    int zomdroidStreamVertex = gl_VertexID % 6;\n"
        "    vPos = zomdroidStream.pos_uv[zomdroidStreamVertex].xy;\n"
        "    vUV = zomdroidStream.pos_uv[zomdroidStreamVertex].zw;\n"
        "    vCol = zomdroidStream.color[zomdroidStreamVertex];\n"
        "    " + aux3_name + " = zomdroidAux.uv[zomdroidStreamVertex].xy;\n";
    if (inputs == 5) setup += "    " + aux4_name + " = zomdroidAux.uv[zomdroidStreamVertex].zw;\n";
    setup +=
        "    ModelViewProjection = zomdroidStream.mvp;\n"
        "    zDepth = zomdroidStream.material.x;\n"
        "    zDepthBlendZ = zomdroidStream.material.y;\n"
        "    zDepthBlendToZ = zomdroidStream.material.z;\n"
        "    zomdroidStreamLayer = int(zomdroidStream.material.w);\n"
        "    zomdroidStreamBlendZ = zomdroidStream.material.y;\n"
        "    zomdroidStreamBlendToZ = zomdroidStream.material.z;\n";
    source.insert(body, setup);

    if (aux_count) *aux_count = static_cast<unsigned>(inputs - 3);
    *transformed = std::move(source);
    return true;
}

v49_program_t* v493_ensure_depth_program(GLuint program_id, v491_program_t** extra_out, v493_program_t** own_out) {
    if (extra_out) *extra_out = nullptr;
    if (own_out) *own_out = nullptr;
    if (!g_v493_enabled || program_id == 0) return nullptr;

    auto own_cached = g_v493_programs.find(program_id);
    if (own_cached != g_v493_programs.end()) {
        if (!own_cached->second.compatible) return nullptr;
        material_program_t* local = v41_find_local_program(program_id);
        auto base_cached = g_v49_programs.find(program_id);
        auto extra_cached = g_v491_programs.find(program_id);
        if (local && local->compatible && base_cached != g_v49_programs.end() && base_cached->second.compatible &&
            extra_cached != g_v491_programs.end() && extra_cached->second.compatible) {
            if (extra_out) *extra_out = &extra_cached->second;
            if (own_out) *own_out = &own_cached->second;
            return &base_cached->second;
        }
        g_v493_programs.erase(own_cached);
        g_v49_programs.erase(program_id);
        g_v491_programs.erase(program_id);
    }

    v493_program_t own{};
    own.attempted = true;
    v491_program_t extra{};
    extra.attempted = true;
    v49_program_t meta{};
    meta.attempted = true;
    meta.app_program = program_id;

    v41_program_record_t source{};
    if (!v41_registry_program(program_id, &source) || !source.linked || !source.complete || !source.identity_safe ||
        !ensure_limits()) {
        g_v493_programs.emplace(program_id, own);
        return nullptr;
    }
    const bool depth_candidate = source.vertex.find("zDepthBlendToZ") != std::string::npos &&
                                 source.fragment.find("DEPTH") != std::string::npos;
    if (!depth_candidate) {
        g_v493_programs.emplace(program_id, own);
        return nullptr;
    }
    if (g_material.aux_ssbo_binding < 0) {
        v493_reject(program_id, "limits", "aux_ssbo_binding");
        g_v493_programs.emplace(program_id, own);
        return nullptr;
    }

    own.generation = source.generation;
    meta.generation = source.generation;
    extra.generation = source.generation;

    std::string stream_vertex;
    const char* vs_reject = "unknown";
    if (!v493_transform_depth_vertex(source.vertex, static_cast<GLint>(g_material.ssbo_binding),
                                     g_material.aux_ssbo_binding, &stream_vertex, &own.aux_count, &vs_reject)) {
        v493_reject(program_id, "vs", vs_reject);
        g_v493_programs.emplace(program_id, own);
        return nullptr;
    }

    std::string stream_fragment;
    bool has_mask = false;
    const char* fs_reject = "unknown";
    if (!v491_transform_depth_fragment(source.fragment, &stream_fragment, &has_mask, &fs_reject)) {
        v493_reject(program_id, "fs", fs_reject);
        g_v493_programs.emplace(program_id, own);
        return nullptr;
    }
    const unsigned expected_aux = has_mask ? 2U : 1U;
    if (own.aux_count != expected_aux) {
        v493_reject(program_id, "contract", has_mask ? "mask_requires_two_aux" : "depth_requires_one_aux");
        g_v493_programs.emplace(program_id, own);
        return nullptr;
    }

    material_program_t info{};
    info.classified = true;
    info.stream_program = stream_variant_for(stream_vertex, stream_fragment);
    if (!info.stream_program) {
        v493_reject(program_id, "compiler", "stream_variant");
        g_v493_programs.emplace(program_id, own);
        return nullptr;
    }
    if (!g_v41_wrapped.get_uniformfv || !g_v41_wrapped.get_uniformiv) {
        v493_reject(program_id, "uniform_state", "query_missing");
        g_v493_programs.emplace(program_id, own);
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
        v493_reject(program_id, "locations", !base_locations_ok ? "base" : "alpha_triple");
        g_v493_programs.emplace(program_id, own);
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
        v493_reject(program_id, "units", "sampler_range");
        g_v493_programs.emplace(program_id, own);
        return nullptr;
    }

    info.compatible = true;
    meta.compatible = true;
    extra.compatible = true;
    own.compatible = true;
    g_material.programs[program_id] = info;
    g_v41_local_programs[program_id] = v41_local_program_t{source.generation, v41_reject_t::none};
    ++g_v41_stats.classified_ok;
    ++g_v49_stats.programs_ok;
    auto base_inserted = g_v49_programs.insert_or_assign(program_id, meta).first;
    auto extra_inserted = g_v491_programs.insert_or_assign(program_id, extra).first;
    auto own_inserted = g_v493_programs.insert_or_assign(program_id, own).first;
    LOG_I("ZOMDROID_PZ_MATERIAL_STREAM_V493_PROGRAM program=%u result=compatible family=tile_depth aux=%u mask=%d "
          "units=diffuse:%d/depth:%d/mask:%d alpha=%d/%d/%d",
          program_id, own.aux_count, meta.has_mask ? 1 : 0, info.diffuse_unit, meta.depth_unit,
          meta.has_mask ? meta.mask_unit : -1, extra.alpha_enabled >= 0 ? 1 : 0,
          meta.alpha_func >= 0 ? 1 : 0, meta.alpha_ref >= 0 ? 1 : 0)
    if (extra_out) *extra_out = &extra_inserted->second;
    if (own_out) *own_out = &own_inserted->second;
    return &base_inserted->second;
}

bool v493_prepare_aux_shadow(v493_program_t& own) {
    if (own.aux_count < 1 || own.aux_count > 2) return false;
    vao_t& vao = current_vao();
    const bool reseed = own.seeded_vao != g_state.vao;
    for (unsigned n = 0; n < own.aux_count; ++n) {
        const GLuint index = 3U + n;
        if (reseed) {
            attrib_t queried{};
            if (!v43_query_attribute(index, &queried)) return false;
            vao.attribs[index] = queried;
        }
        const attrib_t& attribute = vao.attribs[index];
        if (!attribute.enabled || !attribute.described || attribute.buffer == 0 || attribute.divisor != 0) return false;
        const mapping_t* mapping = nullptr;
        if (!safe_mapping(attribute.buffer, &mapping) && !v43_rehydrate_mapping(attribute.buffer)) return false;
    }
    own.seeded_vao = g_state.vao;
    return true;
}

bool v493_build_aux_instance(const candidate_t& candidate, unsigned aux_count, stream_aux_instance_t* output) {
    if (!output || aux_count < 1 || aux_count > 2) return false;
    stream_aux_instance_t aux{};
    for (size_t vertex = 0; vertex < 6; ++vertex) {
        float uv3[2]{};
        float uv4[2]{};
        if (!decode_attribute(candidate, 3, 2, vertex, uv3)) return false;
        aux.uv[vertex][0] = uv3[0];
        aux.uv[vertex][1] = uv3[1];
        if (aux_count == 2) {
            if (!decode_attribute(candidate, 4, 2, vertex, uv4)) return false;
            aux.uv[vertex][2] = uv4[0];
            aux.uv[vertex][3] = uv4[1];
        }
    }
    *output = aux;
    return true;
}

void v493_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    v491_sync_collector_state();
    v491_program_t* extra = nullptr;
    v493_program_t* own = nullptr;
    v49_program_t* meta = v493_ensure_depth_program(g_state.program, &extra, &own);
    if (!meta || !meta->compatible || !extra || !extra->compatible || !own || !own->compatible) {
        v493_passthrough_no_depth(mode, count, type, indices);
        return;
    }

    ++g_v493_stats.tile_depth_draws;
    ++g_material.stats.draws_seen;
    material_program_t* program = v41_find_local_program(g_state.program);
    if (!program || !program->compatible || program->diffuse_unit < 0 || !ensure_limits() ||
        program->diffuse_unit >= g_material.max_texture_units || meta->depth_unit < 0 ||
        meta->depth_unit >= g_material.max_texture_units ||
        (meta->has_mask && (meta->mask_unit < 0 || meta->mask_unit >= g_material.max_texture_units)) ||
        !ensure_aux_stream_objects()) {
        v49_fallback(v41_reject_t::uniform_state, mode, count, type, indices);
        v41_maybe_report();
        v44_maybe_report();
        v49_maybe_report();
        return;
    }

    if (!v493_prepare_aux_shadow(*own)) {
        v49_fallback(v41_reject_t::vertex_mapping, mode, count, type, indices);
        v41_maybe_report();
        v44_maybe_report();
        v49_maybe_report();
        return;
    }

    candidate_t candidate{};
    v41_reject_t candidate_reject = v41_reject_t::none;
    if (!v41_build_candidate(mode, count, type, indices, &candidate, &candidate_reject) ||
        !v41_validate_attribute(candidate.attribs[3], 2, candidate.indices) ||
        (own->aux_count == 2 && !v41_validate_attribute(candidate.attribs[4], 2, candidate.indices))) {
        v49_fallback(candidate_reject == v41_reject_t::none ? v41_reject_t::vertex_mapping : candidate_reject,
                     mode, count, type, indices);
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
    stream_aux_instance_t aux{};
    if (!build_stream_instance(candidate, *program, layer, &instance) ||
        !v493_build_aux_instance(candidate, own->aux_count, &aux)) {
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
    const bool aux_mismatch = g_material.collector_active &&
        (!g_material.aux_collector_active || g_material.aux_collector.size() != g_material.collector.size());
    if (g_material.collector_active &&
        (g_material.collector_program != program->stream_program || g_material.collector_bank != bank ||
         !v49_key_equal(g_v49_collector_key, key) || alpha_enabled_mismatch || aux_mismatch))
        v491_soft_flush_restore();

    if (!g_material.collector_active) {
        v49_apply_stream_state(*meta);
        v491_apply_alpha_enabled(*meta, *extra);
        g_material.collector_active = true;
        g_material.collector_program = program->stream_program;
        g_material.collector_bank = bank;
        g_material.aux_collector_active = true;
        g_material.aux_collector.clear();
        g_v49_collector_key = key;
        g_v491_collector_alpha_valid = alpha_enabled_relevant;
        g_v491_collector_alpha_enabled = extra->alpha_enabled_value;
    }

    g_material.collector.push_back(instance);
    g_material.aux_collector.push_back(aux);
    ++g_material.stats.draws_captured;
    ++g_material.stats.texture_array_hits;
    ++g_material.stats.instances;
    ++g_v49_stats.depth_captured;

    if (g_material.collector.size() >= kCollectorLimit) v491_soft_flush_restore();
    v41_maybe_report();
    v44_maybe_report();
    v49_maybe_report();
    v493_maybe_report_world_only();
}

void v493_glLinkProgram(GLuint program) {
    g_v493_wrapped.link_program(program);
    g_v493_programs.erase(program);
}

void v493_glDeleteProgram(GLuint program) {
    g_v493_programs.erase(program);
    g_v493_wrapped.delete_program(program);
}

void v493_install_impl() {
    ::mg_pz_material_stream_v492_install();
    if (!g_v492_enabled || !g_v491_enabled || !g_v49_enabled || !g_v41_enabled || !g_material_enabled ||
        !g_enabled || !g_v44_enabled)
        return;

    if (!ensure_limits() || g_material.aux_ssbo_binding < 0 ||
        static_cast<GLuint>(g_material.aux_ssbo_binding) == g_material.ssbo_binding) {
        LOG_W_FORCE("ZOMDROID_PZ_MATERIAL_STREAM_V493 disabled reason=aux_ssbo_binding")
        return;
    }

    g_v493_wrapped.draw_elements = static_cast<glDrawElements_PTR>(GLES.glDrawElements);
    g_v493_wrapped.link_program = static_cast<glLinkProgram_PTR>(GLES.glLinkProgram);
    g_v493_wrapped.delete_program = static_cast<glDeleteProgram_PTR>(GLES.glDeleteProgram);
    if (!g_v493_wrapped.draw_elements || !g_v493_wrapped.link_program || !g_v493_wrapped.delete_program) {
        LOG_W_FORCE("ZOMDROID_PZ_MATERIAL_STREAM_V493 disabled reason=wrapper_capture_missing")
        return;
    }

    g_v493_enabled = true;
    GLES.glDrawElements = v493_glDrawElements;
    GLES.glLinkProgram = v493_glLinkProgram;
    GLES.glDeleteProgram = v493_glDeleteProgram;

    LOG_I("ZOMDROID_PZ_MATERIAL_STREAM_V493 enabled=1 revision=4.9.3-world-only mode=perf_ceiling base=v492 "
          "depth_aux=ssbo_sidecar inputs=loc3_vec2+optional_loc4_vec2 mask_contract=aux2 "
          "capture=tile_depth_only no_depth=passthrough no_depth_layout=unused depth_aux=96B "
          "ordering=preserved stable_untouched=1")
}

} // namespace

void install_v493_internal() { v493_install_impl(); }

} // namespace v41_base
} // namespace legacy_v41
} // namespace v43_base
} // namespace v44_base

void mg_pz_material_stream_v493_install(void) {
    v44_base::v43_base::legacy_v41::v41_base::install_v493_internal();
}

#else

void mg_pz_material_stream_v493_install(void) {}

#endif
