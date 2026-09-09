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
inline texture_call_rewrite_result rewrite_legacy_texture2d_calls(std::string& glsl) {
    static const std::regex texture_2d_call(R"(\btexture2D\s*\()", std::regex::ECMAScript);
    if (!std::regex_search(glsl, texture_2d_call)) return {};

    texture_call_rewrite_result result;
    static const std::regex sampler_named_texture(R"(\b[ui]?sampler[A-Za-z0-9_]*\s+texture\b)",
                                                   std::regex::ECMAScript);
    if (std::regex_search(glsl, sampler_named_texture)) {
        static const std::regex texture_identifier(R"(\btexture\b(?!\s*\())", std::regex::ECMAScript);
        glsl = std::regex_replace(glsl, texture_identifier, k_texture_sampler_alias);
        result.sampler_identifier_renamed = true;
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
