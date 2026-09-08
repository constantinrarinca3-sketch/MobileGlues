// MobileGlues - gl/glsl/shader_compat.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#ifndef MOBILEGLUES_SHADER_COMPAT_H
#define MOBILEGLUES_SHADER_COMPAT_H

#include <regex>
#include <string>

namespace mg_glsl_compat {

struct texture_call_rewrite_result {
    bool calls_rewritten = false;
    bool sampler_identifier_renamed = false;
};

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
        glsl = std::regex_replace(glsl, texture_identifier, "zomdroid_texture_sampler");
        result.sampler_identifier_renamed = true;
    }

    glsl = std::regex_replace(glsl, texture_2d_call, "texture(");
    result.calls_rewritten = true;
    return result;
}

} // namespace mg_glsl_compat

#endif // MOBILEGLUES_SHADER_COMPAT_H
