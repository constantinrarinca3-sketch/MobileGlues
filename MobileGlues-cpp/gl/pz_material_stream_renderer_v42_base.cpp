// MobileGlues - gl/pz_material_stream_renderer.cpp
// Project Zomboid experiment V4.2: semantic no-depth material-stream capture.
// V4.1 is retained verbatim in a private namespace. V4.2 replaces only the
// program classifier and draw entry points, keeping the proven state tracking,
// collector and stream emitter from V4/V4.1.

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
#include "pz_material_stream_renderer_v41_base.cpp"
#else

namespace legacy_v41 {
namespace mg_ts {
using backend_command_class = ::mg_ts::backend_command_class;
}
#include "pz_material_stream_renderer_v41_base.cpp"
} // namespace legacy_v41

namespace legacy_v41 {
namespace v41_base {
namespace {

enum class v42_family_reject_t : uint8_t {
    none = 0,
    uniforms,
    forbidden_depth,
    inputs,
    mvp,
    scalar_uniform,
    sampler,
    use_texture,
    texture_call,
    main,
};

const char* v42_family_reject_name(v42_family_reject_t reason) {
    switch (reason) {
    case v42_family_reject_t::none: return "none";
    case v42_family_reject_t::uniforms: return "uniforms";
    case v42_family_reject_t::forbidden_depth: return "forbidden_depth";
    case v42_family_reject_t::inputs: return "inputs";
    case v42_family_reject_t::mvp: return "mvp";
    case v42_family_reject_t::scalar_uniform: return "scalar_uniform";
    case v42_family_reject_t::sampler: return "sampler";
    case v42_family_reject_t::use_texture: return "use_texture";
    case v42_family_reject_t::texture_call: return "texture_call";
    case v42_family_reject_t::main: return "main";
    }
    return "unknown";
}

bool v42_identifier_char(char c) {
    const unsigned char value = static_cast<unsigned char>(c);
    return std::isalnum(value) != 0 || c == '_';
}

std::string v42_trim(const std::string& text) {
    size_t first = 0;
    while (first < text.size() && std::isspace(static_cast<unsigned char>(text[first]))) ++first;
    size_t last = text.size();
    while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1]))) --last;
    return text.substr(first, last - first);
}

size_t v42_word_count(const std::string& source, const std::string& word) {
    if (word.empty()) return 0;
    size_t count = 0;
    size_t cursor = 0;
    while ((cursor = source.find(word, cursor)) != std::string::npos) {
        const bool left = cursor == 0 || !v42_identifier_char(source[cursor - 1]);
        const size_t end = cursor + word.size();
        const bool right = end >= source.size() || !v42_identifier_char(source[end]);
        if (left && right) ++count;
        cursor = end;
    }
    return count;
}

bool v42_split_call_args(const std::string& source, size_t open, size_t close,
                         std::vector<std::string>* args) {
    if (!args || open >= close || close > source.size() || source[open] != '(') return false;
    args->clear();
    size_t start = open + 1;
    int paren = 0;
    int bracket = 0;
    int brace = 0;
    for (size_t i = open + 1; i < close; ++i) {
        const char c = source[i];
        if (c == '(') ++paren;
        else if (c == ')') {
            if (paren <= 0) return false;
            --paren;
        } else if (c == '[') ++bracket;
        else if (c == ']') {
            if (bracket <= 0) return false;
            --bracket;
        } else if (c == '{') ++brace;
        else if (c == '}') {
            if (brace <= 0) return false;
            --brace;
        } else if (c == ',' && paren == 0 && bracket == 0 && brace == 0) {
            args->push_back(v42_trim(source.substr(start, i - start)));
            start = i + 1;
        }
    }
    if (paren != 0 || bracket != 0 || brace != 0) return false;
    args->push_back(v42_trim(source.substr(start, close - start)));
    return true;
}

bool v42_find_call_close(const std::string& source, size_t open, size_t* close) {
    if (!close || open >= source.size() || source[open] != '(') return false;
    int depth = 0;
    for (size_t i = open; i < source.size(); ++i) {
        if (source[i] == '(') {
            ++depth;
        } else if (source[i] == ')') {
            --depth;
            if (depth == 0) {
                *close = i;
                return true;
            }
            if (depth < 0) return false;
        }
    }
    return false;
}

bool v42_rewrite_diffuse_texture_call(std::string* source) {
    if (!source) return false;
    const std::string original = *source;
    size_t match_start = std::string::npos;
    size_t match_end = std::string::npos;
    std::string replacement;
    size_t matches = 0;

    size_t cursor = 0;
    while ((cursor = original.find("texture", cursor)) != std::string::npos) {
        const size_t word_end = cursor + 7;
        const bool left = cursor == 0 || !v42_identifier_char(original[cursor - 1]);
        const bool right = word_end >= original.size() || !v42_identifier_char(original[word_end]);
        if (!left || !right) {
            cursor = word_end;
            continue;
        }
        size_t open = word_end;
        while (open < original.size() && std::isspace(static_cast<unsigned char>(original[open]))) ++open;
        if (open >= original.size() || original[open] != '(') {
            cursor = word_end;
            continue;
        }
        size_t close = 0;
        if (!v42_find_call_close(original, open, &close)) return false;
        std::vector<std::string> args;
        if (!v42_split_call_args(original, open, close, &args)) return false;
        if ((args.size() == 2 || args.size() == 3) && args[0] == "DIFFUSE" && !args[1].empty()) {
            ++matches;
            if (matches > 1) return false;
            match_start = cursor;
            match_end = close + 1;
            replacement = "texture(zomdroidStreamDiffuse, vec3((" + args[1] +
                          "), float(zomdroidStreamLayer))";
            if (args.size() == 3) {
                if (args[2].empty()) return false;
                replacement += ", " + args[2];
            }
            replacement += ")";
        }
        cursor = close + 1;
    }

    if (matches != 1 || match_start == std::string::npos || match_end <= match_start) return false;
    source->replace(match_start, match_end - match_start, replacement);
    return true;
}

bool v42_transform_vertex_source(const std::string& original, GLint ssbo_binding, std::string* transformed,
                                 bool* has_z_depth, bool* has_chunk_depth,
                                 v42_family_reject_t* reject) {
    if (reject) *reject = v42_family_reject_t::none;
    if (!transformed || ssbo_binding < 0) return false;
    if (!only_uniforms(original, {"ModelViewProjection", "chunkDepth", "zDepth"})) {
        if (reject) *reject = v42_family_reject_t::uniforms;
        return false;
    }

    static const std::regex pos_decl(
        R"(layout\s*\(\s*location\s*=\s*0\s*\)\s*in\s+(?:(?:lowp|mediump|highp)\s+)?vec2\s+vPos\s*;)");
    static const std::regex uv_decl(
        R"(layout\s*\(\s*location\s*=\s*1\s*\)\s*in\s+(?:(?:lowp|mediump|highp)\s+)?vec2\s+vUV\s*;)");
    static const std::regex color_decl(
        R"(layout\s*\(\s*location\s*=\s*2\s*\)\s*in\s+(?:(?:lowp|mediump|highp)\s+)?vec4\s+vCol\s*;)");
    static const std::regex any_input(R"(layout\s*\(\s*location\s*=\s*[0-9]+\s*\)\s*in\s+)");
    static const std::regex mvp_decl(
        R"(uniform\s+(?:(?:lowp|mediump|highp)\s+)?mat4\s+ModelViewProjection\s*;)");
    static const std::regex chunk_decl(
        R"(uniform\s+(?:(?:lowp|mediump|highp)\s+)?float\s+chunkDepth\s*;)");
    static const std::regex z_decl(
        R"(uniform\s+(?:(?:lowp|mediump|highp)\s+)?float\s+zDepth\s*;)");
    static const std::regex main_decl(R"(void\s+main\s*\(\s*\)\s*\{)");

    if (regex_count(original, any_input) != 3 || regex_count(original, pos_decl) != 1 ||
        regex_count(original, uv_decl) != 1 || regex_count(original, color_decl) != 1) {
        if (reject) *reject = v42_family_reject_t::inputs;
        return false;
    }
    if (regex_count(original, mvp_decl) != 1) {
        if (reject) *reject = v42_family_reject_t::mvp;
        return false;
    }
    const size_t chunk_count = regex_count(original, chunk_decl);
    const size_t z_count = regex_count(original, z_decl);
    if (chunk_count > 1 || z_count > 1) {
        if (reject) *reject = v42_family_reject_t::scalar_uniform;
        return false;
    }
    if (regex_count(original, main_decl) != 1) {
        if (reject) *reject = v42_family_reject_t::main;
        return false;
    }

    std::string source = original;
    if (!replace_unique(&source, pos_decl, "vec2 vPos;") ||
        !replace_unique(&source, uv_decl, "vec2 vUV;") ||
        !replace_unique(&source, color_decl, "vec4 vCol;") ||
        !replace_unique(&source, mvp_decl, "mat4 ModelViewProjection;")) {
        if (reject) *reject = v42_family_reject_t::inputs;
        return false;
    }
    if (chunk_count == 1 && !replace_unique(&source, chunk_decl, "float chunkDepth;")) return false;
    if (z_count == 1 && !replace_unique(&source, z_decl, "float zDepth;")) return false;

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
        "flat out highp int zomdroidStreamUseTexture;";
    if (!insert_after_precision(&source, declarations)) return false;

    std::smatch main_match;
    if (!std::regex_search(source, main_match, main_decl)) return false;
    const size_t body = static_cast<size_t>(main_match.position() + main_match.length());
    std::string setup =
        "\n    ZomdroidStreamInstance zomdroidStream = zomdroidInstances[gl_InstanceID];\n"
        "    int zomdroidStreamVertex = gl_VertexID % 6;\n"
        "    vPos = zomdroidStream.pos_uv[zomdroidStreamVertex].xy;\n"
        "    vUV = zomdroidStream.pos_uv[zomdroidStreamVertex].zw;\n"
        "    vCol = zomdroidStream.color[zomdroidStreamVertex];\n"
        "    ModelViewProjection = zomdroidStream.mvp;\n";
    if (z_count == 1) setup += "    zDepth = zomdroidStream.material.x;\n";
    if (chunk_count == 1) setup += "    chunkDepth = zomdroidStream.material.y;\n";
    setup +=
        "    zomdroidStreamLayer = int(zomdroidStream.material.z);\n"
        "    zomdroidStreamUseTexture = int(zomdroidStream.material.w);\n";
    source.insert(body, setup);

    if (has_z_depth) *has_z_depth = z_count == 1;
    if (has_chunk_depth) *has_chunk_depth = chunk_count == 1;
    *transformed = std::move(source);
    return true;
}

bool v42_transform_fragment_source(const std::string& original, std::string* transformed,
                                   bool* has_use_texture, v42_family_reject_t* reject) {
    if (reject) *reject = v42_family_reject_t::none;
    if (!transformed) return false;
    if (!only_uniforms(original, {"DIFFUSE", "useTexture"})) {
        if (reject) *reject = v42_family_reject_t::uniforms;
        return false;
    }
    if (v42_word_count(original, "DEPTH") != 0 || v42_word_count(original, "MASK") != 0 ||
        original.find("gl_FragDepth") != std::string::npos) {
        if (reject) *reject = v42_family_reject_t::forbidden_depth;
        return false;
    }

    static const std::regex sampler_decl(
        R"(uniform\s+(?:(?:lowp|mediump|highp)\s+)?sampler2D\s+DIFFUSE\s*;)");
    static const std::regex use_decl(
        R"(uniform\s+(?:(?:lowp|mediump|highp)\s+)?int\s+useTexture\s*;)");
    static const std::regex texcoord_decl(
        R"(in\s+(?:(?:lowp|mediump|highp)\s+)?vec2\s+texCoord\s*;)");
    static const std::regex color_decl(
        R"(in\s+(?:(?:lowp|mediump|highp)\s+)?vec4\s+col\s*;)");
    static const std::regex any_sampler(R"(\bsampler[A-Za-z0-9_]*\b)");
    static const std::regex main_decl(R"(void\s+main\s*\(\s*\)\s*\{)");

    if (regex_count(original, sampler_decl) != 1 || regex_count(original, any_sampler) != 1) {
        if (reject) *reject = v42_family_reject_t::sampler;
        return false;
    }
    if (regex_count(original, texcoord_decl) != 1 || regex_count(original, color_decl) != 1) {
        if (reject) *reject = v42_family_reject_t::inputs;
        return false;
    }
    const size_t use_count = regex_count(original, use_decl);
    if (use_count > 1) {
        if (reject) *reject = v42_family_reject_t::use_texture;
        return false;
    }
    if (regex_count(original, main_decl) != 1) {
        if (reject) *reject = v42_family_reject_t::main;
        return false;
    }
    if (v42_word_count(original, "DIFFUSE") != 2) {
        if (reject) *reject = v42_family_reject_t::texture_call;
        return false;
    }

    std::string source = original;
    if (!replace_unique(&source, sampler_decl, "uniform highp sampler2DArray zomdroidStreamDiffuse;")) {
        if (reject) *reject = v42_family_reject_t::sampler;
        return false;
    }
    if (use_count == 1 && !replace_unique(&source, use_decl, "int useTexture;")) {
        if (reject) *reject = v42_family_reject_t::use_texture;
        return false;
    }
    if (!v42_rewrite_diffuse_texture_call(&source)) {
        if (reject) *reject = v42_family_reject_t::texture_call;
        return false;
    }

    std::string declarations = "flat in highp int zomdroidStreamLayer;";
    if (use_count == 1) declarations += "\nflat in highp int zomdroidStreamUseTexture;";
    if (!insert_after_precision(&source, declarations)) return false;

    if (use_count == 1) {
        std::smatch main_match;
        if (!std::regex_search(source, main_match, main_decl)) return false;
        const size_t body = static_cast<size_t>(main_match.position() + main_match.length());
        source.insert(body, "\n    useTexture = zomdroidStreamUseTexture;\n");
    }

    if (has_use_texture) *has_use_texture = use_count == 1;
    *transformed = std::move(source);
    return true;
}

void v42_log_program(GLuint program, const char* result, const char* stage, const char* reason,
                     bool has_z, bool has_chunk, bool has_use) {
    static thread_local unsigned logged = 0;
    if (logged >= 32) return;
    ++logged;
    LOG_I("ZOMDROID_PZ_MATERIAL_STREAM_V42_PROGRAM program=%u result=%s stage=%s reason=%s z=%d chunk=%d use=%d",
          program, result ? result : "unknown", stage ? stage : "none", reason ? reason : "none",
          has_z ? 1 : 0, has_chunk ? 1 : 0, has_use ? 1 : 0)
}

material_program_t* v42_ensure_program(GLuint program) {
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

    bool has_z = false;
    bool has_chunk = false;
    bool has_use = false;
    v42_family_reject_t family_reject = v42_family_reject_t::none;
    std::string stream_vertex;
    if (!v42_transform_vertex_source(source.vertex, static_cast<GLint>(g_material.ssbo_binding), &stream_vertex,
                                     &has_z, &has_chunk, &family_reject)) {
        v42_log_program(program, "reject", "vs", v42_family_reject_name(family_reject), false, false, false);
        v41_set_local_failure(program, source.generation, v41_reject_t::vertex_family);
        return v41_find_local_program(program);
    }

    std::string stream_fragment;
    family_reject = v42_family_reject_t::none;
    if (!v42_transform_fragment_source(source.fragment, &stream_fragment, &has_use, &family_reject)) {
        v42_log_program(program, "reject", "fs", v42_family_reject_name(family_reject), has_z, has_chunk, false);
        v41_set_local_failure(program, source.generation, v41_reject_t::fragment_family);
        return v41_find_local_program(program);
    }

    material_program_t info{};
    info.classified = true;
    info.stream_program = stream_variant_for(stream_vertex, stream_fragment);
    if (!info.stream_program) {
        v42_log_program(program, "reject", "compiler", "stream_variant", has_z, has_chunk, has_use);
        v41_set_local_failure(program, source.generation, v41_reject_t::compiler);
        return v41_find_local_program(program);
    }

    info.mvp = g_material_backend.get_uniform_location(program, "ModelViewProjection");
    info.z_depth = has_z ? g_material_backend.get_uniform_location(program, "zDepth") : -1;
    info.chunk_depth = has_chunk ? g_material_backend.get_uniform_location(program, "chunkDepth") : -1;
    info.diffuse = g_material_backend.get_uniform_location(program, "DIFFUSE");
    info.use_texture = has_use ? g_material_backend.get_uniform_location(program, "useTexture") : -1;
    if (info.mvp < 0 || info.diffuse < 0 || (has_z && info.z_depth < 0) ||
        (has_chunk && info.chunk_depth < 0) || (has_use && info.use_texture < 0)) {
        v42_log_program(program, "reject", "locations", "required_uniform_missing", has_z, has_chunk, has_use);
        v41_set_local_failure(program, source.generation, v41_reject_t::uniform_locations);
        return v41_find_local_program(program);
    }
    if (!g_v41_wrapped.get_uniformfv || !g_v41_wrapped.get_uniformiv) {
        v41_set_local_failure(program, source.generation, v41_reject_t::uniform_state);
        return v41_find_local_program(program);
    }

    g_v41_wrapped.get_uniformfv(program, info.mvp, info.mvp_value.data());
    if (has_z) g_v41_wrapped.get_uniformfv(program, info.z_depth, &info.z_depth_value);
    if (has_chunk) g_v41_wrapped.get_uniformfv(program, info.chunk_depth, &info.chunk_depth_value);
    g_v41_wrapped.get_uniformiv(program, info.diffuse, &info.diffuse_unit);
    if (has_use)
        g_v41_wrapped.get_uniformiv(program, info.use_texture, &info.use_texture_value);
    else
        info.use_texture_value = 1;
    info.compatible = true;

    g_material.programs[program] = info;
    g_v41_local_programs[program] = v41_local_program_t{source.generation, v41_reject_t::none};
    ++g_v41_stats.classified_ok;
    v42_log_program(program, "compatible", "none", "semantic_no_depth", has_z, has_chunk, has_use);
    return &g_material.programs[program];
}

bool v42_canonical_texture_format(GLint reported, GLint red, GLint green, GLint blue, GLint alpha,
                                  GLint* canonical) {
    if (!canonical) return false;
    if (supported_internal_format(reported)) {
        *canonical = reported;
        return true;
    }
    if (reported == GL_RGBA && red == 8 && green == 8 && blue == 8 && alpha == 8) {
        *canonical = GL_RGBA8;
        return true;
    }
    if (reported == GL_RGB && red == 8 && green == 8 && blue == 8 && alpha == 0) {
        *canonical = GL_RGB8;
        return true;
    }
#ifdef GL_RED
    if (reported == GL_RED && red == 8 && green == 0 && blue == 0 && alpha == 0) {
        *canonical = GL_R8;
        return true;
    }
#endif
#ifdef GL_RG
    if (reported == GL_RG && red == 8 && green == 8 && blue == 0 && alpha == 0) {
        *canonical = GL_RG8;
        return true;
    }
#endif
    return false;
}

bool v42_query_texture_key(GLint unit, GLuint texture, texture_key_t* key) {
    if (!key || texture == 0 || !ensure_limits() || unit < 0 || unit >= g_material.max_texture_units) return false;
    if (!v41_sampler_is_zero(unit)) return false;

    const GLenum restore_active = g_material.active_texture;
    g_material_backend.active_texture(GL_TEXTURE0 + static_cast<GLenum>(unit));
    GLint bound = 0;
    g_material_backend.get_integerv(GL_TEXTURE_BINDING_2D, &bound);
    if (bound != static_cast<GLint>(texture)) {
        g_material_backend.active_texture(restore_active);
        return false;
    }

    texture_key_t out{};
    GLint red = 0, green = 0, blue = 0, alpha = 0;
    GLint reported = 0;
    GLint base_level = 0, compare_mode = GL_NONE;
    GLint swizzle_r = GL_RED, swizzle_g = GL_GREEN, swizzle_b = GL_BLUE, swizzle_a = GL_ALPHA;
    g_material_backend.get_tex_level_parameter_iv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &out.width);
    g_material_backend.get_tex_level_parameter_iv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &out.height);
    g_material_backend.get_tex_level_parameter_iv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &reported);
    g_material_backend.get_tex_level_parameter_iv(GL_TEXTURE_2D, 0, GL_TEXTURE_RED_SIZE, &red);
    g_material_backend.get_tex_level_parameter_iv(GL_TEXTURE_2D, 0, GL_TEXTURE_GREEN_SIZE, &green);
    g_material_backend.get_tex_level_parameter_iv(GL_TEXTURE_2D, 0, GL_TEXTURE_BLUE_SIZE, &blue);
    g_material_backend.get_tex_level_parameter_iv(GL_TEXTURE_2D, 0, GL_TEXTURE_ALPHA_SIZE, &alpha);
    g_material_backend.get_tex_parameter_iv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &out.min_filter);
    g_material_backend.get_tex_parameter_iv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &out.mag_filter);
    g_material_backend.get_tex_parameter_iv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &out.wrap_s);
    g_material_backend.get_tex_parameter_iv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, &out.wrap_t);
    g_material_backend.get_tex_parameter_iv(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, &base_level);
    g_material_backend.get_tex_parameter_iv(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, &compare_mode);
    g_material_backend.get_tex_parameter_iv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, &swizzle_r);
    g_material_backend.get_tex_parameter_iv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, &swizzle_g);
    g_material_backend.get_tex_parameter_iv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, &swizzle_b);
    g_material_backend.get_tex_parameter_iv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, &swizzle_a);
    g_material_backend.active_texture(restore_active);

    if (out.width <= 0 || out.height <= 0 || base_level != 0 || compare_mode != GL_NONE ||
        swizzle_r != GL_RED || swizzle_g != GL_GREEN || swizzle_b != GL_BLUE || swizzle_a != GL_ALPHA ||
        !v42_canonical_texture_format(reported, red, green, blue, alpha, &out.internal_format))
        return false;
    if ((out.min_filter != GL_NEAREST && out.min_filter != GL_LINEAR) ||
        (out.mag_filter != GL_NEAREST && out.mag_filter != GL_LINEAR))
        return false;
    *key = out;
    return true;
}

GLuint v42_bound_texture(GLint unit) {
    if (!ensure_limits() || unit < 0 || unit >= g_material.max_texture_units) return 0;
    GLuint texture = g_material.bound_2d[static_cast<size_t>(unit)];
    if (texture != 0) return texture;
    const GLenum restore_active = g_material.active_texture;
    g_material_backend.active_texture(GL_TEXTURE0 + static_cast<GLenum>(unit));
    GLint bound = 0;
    g_material_backend.get_integerv(GL_TEXTURE_BINDING_2D, &bound);
    g_material_backend.active_texture(restore_active);
    if (bound > 0) {
        texture = static_cast<GLuint>(bound);
        g_material.bound_2d[static_cast<size_t>(unit)] = texture;
    }
    return texture;
}

bool v42_ensure_texture_layer(GLint unit, GLuint texture, size_t* bank_index, GLint* layer,
                              v41_reject_t* reject) {
    if (reject) *reject = v41_reject_t::texture_state;
    if (!bank_index || !layer || texture == 0 || !v41_sampler_is_zero(unit)) return false;

    texture_key_t key{};
    if (!v42_query_texture_key(unit, texture, &key)) return false;
    const unsigned long long generation = texture_generation(texture);
    auto existing = g_material.texture_layers.find(texture);
    if (existing != g_material.texture_layers.end() && existing->second.generation == generation &&
        existing->second.bank < g_material.banks.size() && g_material.banks[existing->second.bank].key == key) {
        *bank_index = existing->second.bank;
        *layer = existing->second.layer;
        if (reject) *reject = v41_reject_t::none;
        return true;
    }

    size_t bank = std::numeric_limits<size_t>::max();
    GLint destination_layer = -1;
    if (existing != g_material.texture_layers.end() && existing->second.bank < g_material.banks.size() &&
        g_material.banks[existing->second.bank].key == key) {
        bank = existing->second.bank;
        destination_layer = existing->second.layer;
    } else {
        for (size_t i = 0; i < g_material.banks.size(); ++i) {
            texture_bank_t& candidate = g_material.banks[i];
            if (candidate.key == key && candidate.used < candidate.capacity) {
                bank = i;
                destination_layer = candidate.used++;
                break;
            }
        }
        if (bank == std::numeric_limits<size_t>::max()) {
            if (!create_texture_bank(key, &bank)) {
                if (reject) *reject = v41_reject_t::texture_array;
                return false;
            }
            destination_layer = g_material.banks[bank].used++;
        }
    }

    texture_bank_t& destination = g_material.banks[bank];
    g_material_backend.copy_image_sub_data(texture, GL_TEXTURE_2D, 0, 0, 0, 0, destination.texture,
                                           GL_TEXTURE_2D_ARRAY, 0, 0, 0, destination_layer,
                                           key.width, key.height, 1);
    g_material.texture_layers[texture] = texture_layer_t{generation, bank, destination_layer};
    *bank_index = bank;
    *layer = destination_layer;
    if (reject) *reject = v41_reject_t::none;
    return true;
}

void v42_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    ++g_material.stats.draws_seen;

    material_program_t* program = v42_ensure_program(g_state.program);
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

    const GLuint texture = v42_bound_texture(program->diffuse_unit);
    size_t bank = 0;
    GLint layer = 0;
    v41_reject_t texture_reject = v41_reject_t::none;
    if (!v42_ensure_texture_layer(program->diffuse_unit, texture, &bank, &layer, &texture_reject)) {
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

void v42_glUseProgram(GLuint program) {
    if (g_material.collector_active && !g_material.collector.empty()) {
        material_program_t* next = v41_find_local_program(program);
        if (!next || !next->compatible || next->stream_program != g_material.collector_program)
            soft_segment_flush();
    }
    g_material_wrapped.use_program(program);
    (void)v42_ensure_program(program);
}

void v42_install_impl() {
    install_v41_internal();
    if (!g_v41_enabled || !g_material_enabled || !g_enabled) return;
    GLES.glDrawElements = v42_glDrawElements;
    GLES.glUseProgram = v42_glUseProgram;
    LOG_I("ZOMDROID_PZ_MATERIAL_STREAM_V42 enabled=1 revision=4.2 family=semantic_no_depth "
          "texture_unsized=canonical_8bit goal=capture_not_probe stable_untouched=1")
}

} // namespace

void install_v42_internal() { v42_install_impl(); }

} // namespace v41_base
} // namespace legacy_v41

namespace mg_ts {
void pz_repack_before_backend_command(backend_command_class classification) {
    legacy_v41::v41_base::before_v41_internal(classification);
}
} // namespace mg_ts

void mg_pz_repack_renderer_install(void) {
    legacy_v41::v41_base::install_v42_internal();
}

#endif // ZOMDROID_EXPERIMENTAL
