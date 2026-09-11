// MobileGlues - gl/glsl/shader_compat.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#ifndef MOBILEGLUES_SHADER_COMPAT_H
#define MOBILEGLUES_SHADER_COMPAT_H

#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <regex>
#include <string>
#include <vector>

namespace mg_glsl_compat {

struct texture_call_rewrite_result {
    bool calls_rewritten = false;
    bool sampler_identifier_renamed = false;
};

enum class pz_alpha_shader_kind {
    none,
    chunk_composite,
    tile_with_depth,
    opaque_with_depth,
    seam_fix_2,
};

struct pz_alpha_rewrite_result {
    bool candidate = false;
    bool contract_matched = false;
    bool rewritten = false;
    pz_alpha_shader_kind kind = pz_alpha_shader_kind::none;
};

inline const char* pz_alpha_shader_kind_name(pz_alpha_shader_kind kind) {
    switch (kind) {
    case pz_alpha_shader_kind::chunk_composite:
        return "chunk_composite";
    case pz_alpha_shader_kind::tile_with_depth:
        return "tile_with_depth";
    case pz_alpha_shader_kind::opaque_with_depth:
        return "opaque_with_depth";
    case pz_alpha_shader_kind::seam_fix_2:
        return "seam_fix_2";
    case pz_alpha_shader_kind::none:
        return "none";
    }
    return "none";
}

constexpr const char* k_texture_sampler_alias = "zomdroid_texture_sampler";

struct uniform_default_value {
    std::string name;
    bool integer = false;
    unsigned int components = 0;
    std::array<double, 4> values{};
};

inline std::string trim_copy(const std::string& text) {
    size_t first = 0;
    while (first < text.size() && std::isspace(static_cast<unsigned char>(text[first]))) ++first;
    size_t last = text.size();
    while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1]))) --last;
    return text.substr(first, last - first);
}

// B42 relies on desktop-GL uniform initializers. glslang/SPIR-V Cross removes
// them while producing ESSL, whose linked uniforms consequently start at zero.
// Keep only constant numeric forms that can be reproduced exactly after link;
// symbolic expressions are deliberately ignored instead of guessed.
inline bool parse_numeric_constant(const std::string& expression, double& value) {
    const std::string text = trim_copy(expression);
    if (text == "true") {
        value = 1.0;
        return true;
    }
    if (text == "false") {
        value = 0.0;
        return true;
    }

    const char* begin = text.c_str();
    char* end = nullptr;
    const double numerator = std::strtod(begin, &end);
    if (end == begin) return false;
    while (*end && std::isspace(static_cast<unsigned char>(*end))) ++end;
    if (*end == '\0') {
        value = numerator;
        return true;
    }
    if (*end != '/') return false;

    const char* denominator_begin = end + 1;
    while (*denominator_begin && std::isspace(static_cast<unsigned char>(*denominator_begin))) ++denominator_begin;
    char* denominator_end = nullptr;
    const double denominator = std::strtod(denominator_begin, &denominator_end);
    if (denominator_end == denominator_begin || denominator == 0.0) return false;
    while (*denominator_end && std::isspace(static_cast<unsigned char>(*denominator_end))) ++denominator_end;
    if (*denominator_end != '\0') return false;
    value = numerator / denominator;
    return true;
}

inline bool parse_uniform_components(const std::string& type, const std::string& expression,
                                     uniform_default_value& result) {
    result.integer = type == "int" || type == "bool" || type.rfind("ivec", 0) == 0 ||
                     type.rfind("bvec", 0) == 0;
    result.components = 1;
    if (type.size() == 4 && (type.rfind("vec", 0) == 0)) result.components = static_cast<unsigned int>(type[3] - '0');
    if (type.size() == 5 && (type.rfind("ivec", 0) == 0 || type.rfind("bvec", 0) == 0))
        result.components = static_cast<unsigned int>(type[4] - '0');
    if (result.components < 1 || result.components > 4) return false;

    std::string values = trim_copy(expression);
    if (result.components > 1) {
        const size_t open = values.find('(');
        const size_t close = values.rfind(')');
        if (open == std::string::npos || close == std::string::npos || close <= open ||
            !trim_copy(values.substr(close + 1)).empty())
            return false;
        values = values.substr(open + 1, close - open - 1);
    }

    std::vector<double> parsed;
    size_t start = 0;
    while (start <= values.size()) {
        const size_t comma = values.find(',', start);
        double component = 0.0;
        if (!parse_numeric_constant(values.substr(start, comma == std::string::npos ? std::string::npos
                                                                                    : comma - start),
                                    component))
            return false;
        parsed.push_back(component);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }

    if (parsed.size() == 1 && result.components > 1) parsed.resize(result.components, parsed.front());
    if (parsed.size() != result.components) return false;
    for (unsigned int i = 0; i < result.components; ++i) result.values[i] = parsed[i];
    return true;
}

inline std::string strip_glsl_comments(const std::string& glsl) {
    std::string clean = glsl;
    bool line_comment = false;
    bool block_comment = false;
    for (size_t i = 0; i < clean.size(); ++i) {
        if (line_comment) {
            if (clean[i] == '\n' || clean[i] == '\r')
                line_comment = false;
            else
                clean[i] = ' ';
            continue;
        }
        if (block_comment) {
            if (clean[i] == '*' && i + 1 < clean.size() && clean[i + 1] == '/') {
                clean[i] = clean[i + 1] = ' ';
                ++i;
                block_comment = false;
            } else if (clean[i] != '\n' && clean[i] != '\r') {
                clean[i] = ' ';
            }
            continue;
        }
        if (clean[i] == '/' && i + 1 < clean.size() && clean[i + 1] == '/') {
            clean[i] = clean[i + 1] = ' ';
            ++i;
            line_comment = true;
        } else if (clean[i] == '/' && i + 1 < clean.size() && clean[i + 1] == '*') {
            clean[i] = clean[i + 1] = ' ';
            ++i;
            block_comment = true;
        }
    }
    return clean;
}

struct unique_regex_match {
    bool found = false;
    size_t position = 0;
    size_t length = 0;
};

inline unique_regex_match find_unique_regex(const std::string& source, const std::regex& pattern) {
    unique_regex_match result;
    size_t count = 0;
    for (std::sregex_iterator it(source.begin(), source.end(), pattern), end; it != end; ++it) {
        ++count;
        if (count > 1) return {};
        result.found = true;
        result.position = static_cast<size_t>(it->position());
        result.length = static_cast<size_t>(it->length());
    }
    return result;
}

inline bool regex_present(const std::string& source, const std::regex& pattern) {
    return std::regex_search(source, pattern);
}

// Restore the one piece of legacy fixed-function state the four PZ chunk
// shaders bypass. The matcher intentionally describes complete shader
// contracts, not filenames (GL never receives those) and not a global alpha
// heuristic. A changed game shader therefore stays untouched instead of being
// approximately rewritten.
inline pz_alpha_rewrite_result rewrite_pz_alpha_test_family(std::string& glsl) {
    pz_alpha_rewrite_result result;
    const std::string clean = strip_glsl_comments(glsl); // length-preserving; offsets remain valid

    static const std::regex diffuse_decl(R"(\buniform\s+sampler2D\s+DIFFUSE\s*;)");
    static const std::regex depth_decl(R"(\buniform\s+sampler2D\s+DEPTH\s*;)");
    static const std::regex frag_color(R"(\bgl_FragColor\b)");
    if (!regex_present(clean, diffuse_decl) || !regex_present(clean, depth_decl) ||
        !find_unique_regex(clean, frag_color).found)
        return result;
    result.candidate = true;

    static const std::regex injected(R"(\bzomdroidAlpha(?:Enabled|Func|Ref|Pass|FinalColor)\b)");
    static const std::regex main_decl(R"(\bvoid\s+main\s*\(\s*\)\s*\{)");
    const unique_regex_match main = find_unique_regex(clean, main_decl);
    if (regex_present(clean, injected) || !main.found) return result;

    static const std::regex chunk_depth(
        R"(\bgl_FragDepth\s*=\s*chunkDepth\s*\+\s*depthTexel\s*;)");
    static const std::regex chunk_color(R"(\bgl_FragColor\s*=\s*c\s*\*\s*col\s*;)");
    static const std::regex chunk_depth_uniform(R"(\buniform\s+float\s+chunkDepth(?:\s*=\s*[^;]+)?\s*;)");
    static const std::regex use_texture_uniform(R"(\buniform\s+int\s+useTexture(?:\s*=\s*[^;]+)?\s*;)");
    static const std::regex depth_texel(R"(\bfloat\s+depthTexel\s*=)");

    const unique_regex_match chunk_depth_write = find_unique_regex(clean, chunk_depth);
    const unique_regex_match chunk_color_write = find_unique_regex(clean, chunk_color);
    unique_regex_match replace;
    bool replace_final_color = false;
    if (chunk_depth_write.found && chunk_color_write.found && regex_present(clean, chunk_depth_uniform) &&
        regex_present(clean, use_texture_uniform) && regex_present(clean, depth_texel)) {
        result.kind = pz_alpha_shader_kind::chunk_composite;
        replace = chunk_color_write;
        replace_final_color = true;
    } else {
        static const std::regex producer_color(R"(\bgl_FragColor\s*=\s*c\s*;)");
        static const std::regex producer_depth(R"(\bgl_FragDepth\s*=\s*calcDepthZ\s*;)");
        static const std::regex depth_z_uniform(R"(\buniform\s+float\s+zDepthBlendZ(?:\s*=\s*[^;]+)?\s*;)");
        static const std::regex depth_to_z_uniform(
            R"(\buniform\s+float\s+zDepthBlendToZ(?:\s*=\s*[^;]+)?\s*;)");
        static const std::regex multiply_color_a(R"(\bc\s*\.\s*rgb\s*\*=\s*col\s*\.\s*a\s*;)");
        static const std::regex multiply_color(R"(\bc\s*\*=\s*col\s*;)");
        static const std::regex multiply_opaque(R"(\bvec4\s+c\s*=\s*c0\s*\*\s*col\s*;)");

        const unique_regex_match producer_color_write = find_unique_regex(clean, producer_color);
        const unique_regex_match producer_depth_write = find_unique_regex(clean, producer_depth);
        const bool common = producer_color_write.found && producer_depth_write.found &&
                            regex_present(clean, depth_z_uniform) && regex_present(clean, depth_to_z_uniform) &&
                            regex_present(clean, multiply_color_a) &&
                            (regex_present(clean, multiply_color) || regex_present(clean, multiply_opaque));
        if (common) {
            static const std::regex mask_decl(R"(\buniform\s+sampler2D\s+MASK\s*;)");
            static const std::regex mask_sample(R"(\bvec4\s+m\s*=\s*texture2D\s*\(\s*MASK\b)");
            static const std::regex seam_condition(R"(\bif\s*\(\s*d\s*\*\s*m\s*\.\s*a\s*>\s*0(?:\.0+)?\s*\))");
            static const std::regex opaque_sample(R"(\bvec4\s+c0\s*=\s*texture2D\s*\(\s*DIFFUSE\b)");
            static const std::regex opaque_condition(
                R"(\bif\s*\(\s*c0\s*\.\s*a\s*>\s*0\.8\s*&&\s*d\s*>\s*0\.0\s*\))");
            static const std::regex tile_sample(R"(\bvec4\s+c\s*=\s*texture2D\s*\(\s*DIFFUSE\b)");
            static const std::regex tile_condition(R"(\bif\s*\(\s*d\s*>\s*0(?:\.0+)?\s*\))");
            static const std::regex c0_token(R"(\bc0\b)");

            const bool seam = regex_present(clean, mask_decl) && regex_present(clean, mask_sample) &&
                              regex_present(clean, seam_condition);
            const bool opaque = !regex_present(clean, mask_decl) && regex_present(clean, opaque_sample) &&
                                regex_present(clean, multiply_opaque) && regex_present(clean, opaque_condition);
            const bool tile = !regex_present(clean, mask_decl) && !regex_present(clean, c0_token) &&
                              regex_present(clean, tile_sample) && regex_present(clean, multiply_color) &&
                              regex_present(clean, tile_condition);
            const unsigned matches = static_cast<unsigned>(seam) + static_cast<unsigned>(opaque) +
                                     static_cast<unsigned>(tile);
            if (matches == 1) {
                result.kind = seam       ? pz_alpha_shader_kind::seam_fix_2
                              : opaque   ? pz_alpha_shader_kind::opaque_with_depth
                                         : pz_alpha_shader_kind::tile_with_depth;
                replace = producer_depth_write;
            }
        }
    }

    result.contract_matched = result.kind != pz_alpha_shader_kind::none && replace.found;
    if (!result.contract_matched) return result;

    const std::string replacement =
        replace_final_color
            ? "vec4 zomdroidAlphaFinalColor = c * col;\n"
              "    if (zomdroidAlphaEnabled != 0 && !zomdroidAlphaPass(zomdroidAlphaFinalColor.a)) discard;\n"
              "    gl_FragColor = zomdroidAlphaFinalColor;"
            : "if (zomdroidAlphaEnabled != 0 && !zomdroidAlphaPass(c.a)) discard;\n"
              "        gl_FragDepth = calcDepthZ;";
    glsl.replace(replace.position, replace.length, replacement);

    static constexpr const char* declarations =
        "uniform int zomdroidAlphaEnabled;\n"
        "uniform int zomdroidAlphaFunc;\n"
        "uniform float zomdroidAlphaRef;\n"
        "bool zomdroidAlphaPass(float value) {\n"
        "    if (zomdroidAlphaFunc == 512) return false;\n"
        "    if (zomdroidAlphaFunc == 513) return value < zomdroidAlphaRef;\n"
        "    if (zomdroidAlphaFunc == 514) return value == zomdroidAlphaRef;\n"
        "    if (zomdroidAlphaFunc == 515) return value <= zomdroidAlphaRef;\n"
        "    if (zomdroidAlphaFunc == 516) return value > zomdroidAlphaRef;\n"
        "    if (zomdroidAlphaFunc == 517) return value != zomdroidAlphaRef;\n"
        "    if (zomdroidAlphaFunc == 518) return value >= zomdroidAlphaRef;\n"
        "    return true;\n"
        "}\n\n";
    glsl.insert(main.position, declarations);
    result.rewritten = true;
    return result;
}

inline std::vector<uniform_default_value> collect_uniform_defaults(const std::string& glsl) {
    static const std::regex declaration(
        R"(\buniform\s+(?:(?:lowp|mediump|highp)\s+)?(float|int|bool|vec[234]|ivec[234]|bvec[234])\s+([A-Za-z_]\w*)\s*=\s*([^;]+);)",
        std::regex::ECMAScript);

    std::vector<uniform_default_value> defaults;
    const std::string clean = strip_glsl_comments(glsl);
    for (std::sregex_iterator it(clean.begin(), clean.end(), declaration), end; it != end; ++it) {
        uniform_default_value value;
        value.name = (*it)[2].str();
        if (parse_uniform_components((*it)[1].str(), (*it)[3].str(), value)) defaults.push_back(std::move(value));
    }
    return defaults;
}

// GLSL 1.10/1.20 allowed a sampler to be named `texture`, because sampling
// used texture2D(). Once the shader is promoted to core GLSL, texture2D() must
// become texture(), and that old sampler name shadows the built-in function.
// Rename only the non-call identifier tokens before rewriting the calls.
inline size_t next_glsl_token_char(const std::string& glsl, size_t position) {
    while (position < glsl.size()) {
        if (std::isspace(static_cast<unsigned char>(glsl[position]))) {
            ++position;
            continue;
        }
        if (position + 1 < glsl.size() && glsl[position] == '/' && glsl[position + 1] == '/') {
            const size_t end = glsl.find('\n', position + 2);
            if (end == std::string::npos) return glsl.size();
            position = end + 1;
            continue;
        }
        if (position + 1 < glsl.size() && glsl[position] == '/' && glsl[position + 1] == '*') {
            const size_t end = glsl.find("*/", position + 2);
            if (end == std::string::npos) return glsl.size();
            position = end + 2;
            continue;
        }
        break;
    }
    return position;
}

inline bool rename_texture_sampler_tokens(std::string& glsl) {
    std::string rewritten;
    rewritten.reserve(glsl.size() + 32);
    std::vector<bool> brace_is_struct;
    size_t struct_depth = 0;
    bool awaiting_struct_body = false;
    bool previous_token_was_dot = false;
    bool renamed = false;

    for (size_t i = 0; i < glsl.size();) {
        if (i + 1 < glsl.size() && glsl[i] == '/' && glsl[i + 1] == '/') {
            const size_t end = glsl.find('\n', i + 2);
            const size_t length = end == std::string::npos ? glsl.size() - i : end + 1 - i;
            rewritten.append(glsl, i, length);
            i += length;
            continue;
        }
        if (i + 1 < glsl.size() && glsl[i] == '/' && glsl[i + 1] == '*') {
            const size_t end = glsl.find("*/", i + 2);
            const size_t length = end == std::string::npos ? glsl.size() - i : end + 2 - i;
            rewritten.append(glsl, i, length);
            i += length;
            continue;
        }

        const unsigned char current = static_cast<unsigned char>(glsl[i]);
        if (std::isalpha(current) || glsl[i] == '_') {
            size_t end = i + 1;
            while (end < glsl.size()) {
                const unsigned char ch = static_cast<unsigned char>(glsl[end]);
                if (!std::isalnum(ch) && glsl[end] != '_') break;
                ++end;
            }
            const bool is_texture = glsl.compare(i, end - i, "texture") == 0;
            const size_t next = next_glsl_token_char(glsl, end);
            const bool is_call = next < glsl.size() && glsl[next] == '(';
            if (is_texture && struct_depth == 0 && !previous_token_was_dot && !is_call) {
                rewritten += k_texture_sampler_alias;
                renamed = true;
            } else {
                rewritten.append(glsl, i, end - i);
            }
            if (glsl.compare(i, end - i, "struct") == 0) awaiting_struct_body = true;
            previous_token_was_dot = false;
            i = end;
            continue;
        }

        const char ch = glsl[i++];
        rewritten.push_back(ch);
        if (ch == '{') {
            brace_is_struct.push_back(awaiting_struct_body);
            if (awaiting_struct_body) ++struct_depth;
            awaiting_struct_body = false;
            previous_token_was_dot = false;
        } else if (ch == '}') {
            if (!brace_is_struct.empty()) {
                if (brace_is_struct.back()) --struct_depth;
                brace_is_struct.pop_back();
            }
            previous_token_was_dot = false;
        } else if (ch == ';') {
            awaiting_struct_body = false;
            previous_token_was_dot = false;
        } else if (ch == '.') {
            previous_token_was_dot = true;
        } else if (!std::isspace(static_cast<unsigned char>(ch))) {
            previous_token_was_dot = false;
        }
    }

    if (renamed) glsl.swap(rewritten);
    return renamed;
}

inline texture_call_rewrite_result rewrite_legacy_texture2d_calls(std::string& glsl) {
    static const std::regex texture_2d_call(R"(\btexture2D\s*\()", std::regex::ECMAScript);
    if (!std::regex_search(glsl, texture_2d_call)) return {};

    texture_call_rewrite_result result;
    static const std::regex sampler_named_texture(R"(\b[ui]?sampler[A-Za-z0-9_]*\s+texture\b)",
                                                   std::regex::ECMAScript);
    if (std::regex_search(glsl, sampler_named_texture)) {
        result.sampler_identifier_renamed = rename_texture_sampler_tokens(glsl);
    }

    glsl = std::regex_replace(glsl, texture_2d_call, "texture(");
    result.calls_rewritten = true;
    return result;
}

// Source rewrites must stay invisible to the desktop caller.  In particular,
// game code still asks for glGetUniformLocation(program, "texture").  Return
// the renamed driver-facing spelling for that exact uniform (and array element
// lookups), while every unrelated uniform name keeps its original pointer.
inline const char* remap_texture_sampler_uniform_name(const char* requested_name, std::string& storage) {
    if (!requested_name) return requested_name;

    constexpr const char* original = "texture";
    constexpr size_t original_length = 7;
    if (std::strncmp(requested_name, original, original_length) != 0) return requested_name;
    const char suffix = requested_name[original_length];
    if (suffix != '\0' && suffix != '[') return requested_name;

    storage = k_texture_sampler_alias;
    storage += requested_name + original_length;
    return storage.c_str();
}

} // namespace mg_glsl_compat

#endif // MOBILEGLUES_SHADER_COMPAT_H
