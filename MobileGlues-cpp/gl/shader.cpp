// MobileGlues - gl/shader.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#include <algorithm>
#include <atomic>
#include <cctype>
#include <vector>
#include "shader.h"

#include <GL/gl.h>
#include "log.h"
#include "program.h"
#include "../gles/loader.h"
#include "../includes.h"
#include "glsl/glsl_for_es.h"
#include "../config/settings.h"
#include "FSR1/FSR1.h"

#define DEBUG 0

struct shader_t shaderInfo;

UnorderedMap<GLuint, bool> shader_map_is_sampler_buffer_emulated;
UnorderedMap<GLuint, std::vector<mg_glsl_compat::uniform_default_value>> shader_map_uniform_defaults;
#if defined(ZOMDROID_EXPERIMENTAL)
UnorderedMap<GLuint, mg_glsl_compat::pz_alpha_shader_kind> shader_map_pz_alpha_kind;
UnorderedMap<GLuint, bool> shader_map_uses_vertex_id;
#endif
#if defined(ZOMDROID_GL_BREADCRUMBS)
UnorderedMap<GLuint, bool> zomdroid_tile_depth_shader;
#endif

const std::vector<mg_glsl_compat::uniform_default_value>* mg_shader_uniform_defaults(GLuint shader) {
    const auto it = shader_map_uniform_defaults.find(shader);
    return it == shader_map_uniform_defaults.end() ? nullptr : &it->second;
}

#if defined(ZOMDROID_EXPERIMENTAL)
mg_glsl_compat::pz_alpha_shader_kind mg_shader_pz_alpha_kind(GLuint shader) {
    const auto it = shader_map_pz_alpha_kind.find(shader);
    return it == shader_map_pz_alpha_kind.end() ? mg_glsl_compat::pz_alpha_shader_kind::none : it->second;
}

bool mg_shader_uses_vertex_id(GLuint shader) {
    const auto it = shader_map_uses_vertex_id.find(shader);
    // An untracked source cannot prove that changing gl_VertexID is safe.
    return it == shader_map_uses_vertex_id.end() || it->second;
}
#endif

void mg_shader_deleted(GLuint shader) {
    shader_map_uniform_defaults.erase(shader);
    shader_map_is_sampler_buffer_emulated.erase(shader);
#if defined(ZOMDROID_EXPERIMENTAL)
    shader_map_pz_alpha_kind.erase(shader);
    shader_map_uses_vertex_id.erase(shader);
#endif
#if defined(ZOMDROID_GL_BREADCRUMBS)
    zomdroid_tile_depth_shader.erase(shader);
#endif
    if (shaderInfo.id == shader) {
        shaderInfo.id = 0;
        shaderInfo.converted.clear();
        shaderInfo.frag_data_changed_converted.clear();
        shaderInfo.frag_data_changed = 0;
    }
}

namespace {
#if defined(ZOMDROID_GL_BREADCRUMBS)
std::atomic<unsigned int> g_zomdroid_shader_source_seq{0};
std::atomic<unsigned int> g_zomdroid_shader_status_seq{0};
std::atomic<unsigned int> g_zomdroid_alpha_nearmiss_seq{0};
constexpr unsigned int k_zomdroid_shader_trace_limit = 16;

std::string shader_preview(const std::string& source) {
    constexpr size_t k_preview_limit = 192;
    std::string preview = source.substr(0, k_preview_limit);
    for (char& c : preview) {
        const unsigned char value = static_cast<unsigned char>(c);
        if (c == '\n' || c == '\r' || c == '\t')
            c = ' ';
        else if (value < 0x20 || value == 0x7f)
            c = '?';
    }
    return preview;
}

void trace_zomdroid_shader_source(GLuint shader, GLint shader_type, GLsizei fragment_count, size_t input_length,
                                  const char* route, int conversion_result, bool version_normalized,
                                  const std::string& input, const std::string& output) {
    const unsigned int seq = g_zomdroid_shader_source_seq.fetch_add(1, std::memory_order_relaxed) + 1;
    if (seq > k_zomdroid_shader_trace_limit) return;
    const std::string input_head = shader_preview(input);
    const std::string output_head = shader_preview(output);
    write_log("ZOMDROID_SHADER_SOURCE %u shader=%u type=0x%x fragments=%d input_len=%zu output_len=%zu route=%s "
              "convert=%d version_normalized=%d input_head=[%s] output_head=[%s]",
              seq, shader, shader_type, fragment_count, input_length, output.size(), route, conversion_result,
              version_normalized ? 1 : 0, input_head.c_str(), output_head.c_str());
}

void trace_zomdroid_shader_status(GLuint shader, GLint status) {
    const unsigned int seq = g_zomdroid_shader_status_seq.fetch_add(1, std::memory_order_relaxed) + 1;
    if (seq > k_zomdroid_shader_trace_limit) return;

    if (status == GL_TRUE) {
        write_log("ZOMDROID_SHADER_STATUS %u shader=%u compile=PASS", seq, shader);
        return;
    }

    GLint log_length = 0;
    GLES.glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);
    const GLsizei capacity = static_cast<GLsizei>(std::max(1, std::min(log_length, 1024)));
    std::vector<GLchar> info(static_cast<size_t>(capacity), '\0');
    GLsizei written = 0;
    GLES.glGetShaderInfoLog(shader, capacity, &written, info.data());
    const size_t safe_length = static_cast<size_t>(std::max(0, std::min(written, capacity - 1)));
    const std::string info_head = shader_preview(std::string(info.data(), safe_length));
    write_log("ZOMDROID_SHADER_STATUS %u shader=%u compile=FAIL driver=[%s]", seq, shader, info_head.c_str());
}
#else
void trace_zomdroid_shader_source(GLuint, GLint, GLsizei, size_t, const char*, int, bool, const std::string&,
                                  const std::string&) {}
void trace_zomdroid_shader_status(GLuint, GLint) {}
#endif

// Qualcomm accepts the ESSL versions used here, but requires the version
// directive to be the first directive in canonical form. Sources assembled by
// desktop GL callers can contain a BOM, blank lines or indentation before it.
// Only known ESSL versions are rewritten; desktop GLSL is left to the real
// translator rather than being mislabeled as ESSL.
bool normalize_essl_version_directive(std::string& source) {
    bool changed = false;
    if (source.size() >= 3 && static_cast<unsigned char>(source[0]) == 0xef &&
        static_cast<unsigned char>(source[1]) == 0xbb && static_cast<unsigned char>(source[2]) == 0xbf) {
        source.erase(0, 3);
        changed = true;
    }

    const size_t version_pos = source.find("#version");
    if (version_pos == std::string::npos) return false;
    for (size_t i = 0; i < version_pos; ++i) {
        if (!std::isspace(static_cast<unsigned char>(source[i]))) return false;
    }

    size_t version_end = source.find_first_of("\r\n", version_pos);
    if (version_end == std::string::npos) version_end = source.size();
    const std::string directive = source.substr(version_pos, version_end - version_pos);

    size_t cursor = strlen("#version");
    while (cursor < directive.size() && std::isspace(static_cast<unsigned char>(directive[cursor]))) ++cursor;
    size_t digits_end = cursor;
    while (digits_end < directive.size() && std::isdigit(static_cast<unsigned char>(directive[digits_end])))
        ++digits_end;
    if (digits_end == cursor) return false;

    int version = 0;
    for (size_t i = cursor; i < digits_end; ++i) {
        version = version * 10 + (directive[i] - '0');
        if (version > 1000) return changed;
    }
    if (version != 100 && version != 300 && version != 310 && version != 320) return false;

    size_t next_line = version_end;
    if (next_line < source.size() && source[next_line] == '\r') ++next_line;
    if (next_line < source.size() && source[next_line] == '\n') ++next_line;

    const std::string prefix = source.substr(0, version_pos);
    const std::string rest = source.substr(next_line);
    const std::string canonical =
        "#version " + std::to_string(version) + (version >= 300 ? " es\n" : "\n");
    const std::string normalized = canonical + prefix + rest;
    if (normalized == source) return changed;
    source = normalized;
    return true;
}
} // namespace

bool can_run_essl3(unsigned int esversion, const char* glsl) {
    if (strncmp(glsl, "#version 100", 12) == 0) {
        return true;
    }

    unsigned int glsl_version = 0;
    if (strncmp(glsl, "#version 300 es", 15) == 0) {
        glsl_version = 300;
    } else if (strncmp(glsl, "#version 310 es", 15) == 0) {
        glsl_version = 310;
    } else if (strncmp(glsl, "#version 320 es", 15) == 0) {
        glsl_version = 320;
    } else {
        return false;
    }
    return esversion >= glsl_version;
}

bool is_direct_shader(const char* glsl) {
    bool es3_ability = can_run_essl3(hardware->es_version, glsl);
    return es3_ability;
}

bool check_if_sampler_buffer_used(std::string str) {
    return str.find("samplerBuffer") != std::string::npos;
}

void glShaderSource(GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length) {
    LOG()
    shaderInfo.id = 0;
    shaderInfo.converted = "";
    shaderInfo.frag_data_changed_converted.clear();
    shaderInfo.frag_data_changed = 0;
    size_t l = 0;
    for (int i = 0; i < count; i++)
        l += (length && length[i] >= 0) ? length[i] : strlen(string[i]);
    std::string glsl_src, essl_src;
    glsl_src.reserve(l + 1);
    if (length) {
        for (int i = 0; i < count; i++) {
            if (length[i] >= 0)
                glsl_src += std::string_view(string[i], length[i]);
            else
                glsl_src += string[i];
        }
    } else {
        for (int i = 0; i < count; i++) {
            glsl_src += string[i];
        }
    }

#if defined(ZOMDROID_GL_BREADCRUMBS)
    const bool is_tile_depth_source =
        glsl_src.find("zDepthBlendToZ") != std::string::npos &&
        (glsl_src.find("DEPTH") != std::string::npos || glsl_src.find("depth") != std::string::npos);
    zomdroid_tile_depth_shader[shader] = is_tile_depth_source;
#endif

    bool is_sampler_buffer_emulated = hardware->emulate_texture_buffer && check_if_sampler_buffer_used(glsl_src);
    GLint shader_type = 0;
    GLES.glGetShaderiv(shader, GL_SHADER_TYPE, &shader_type);
#if defined(ZOMDROID_EXPERIMENTAL)
    shader_map_pz_alpha_kind.erase(shader);
    // A textual hit in a comment only disables the optimization, which is the
    // safe failure mode. Missing a real use would change gl_VertexID when a
    // pointer offset is represented as baseVertex.
    shader_map_uses_vertex_id[shader] = glsl_src.find("gl_VertexID") != std::string::npos ||
                                        glsl_src.find("gl_BaseVertex") != std::string::npos;
    mg_glsl_compat::pz_alpha_rewrite_result alpha_rewrite;
    if (shader_type == GL_FRAGMENT_SHADER) {
        alpha_rewrite = mg_glsl_compat::rewrite_pz_alpha_test_family(glsl_src);
        if (alpha_rewrite.rewritten) {
            shader_map_pz_alpha_kind[shader] = alpha_rewrite.kind;
#if defined(ZOMDROID_GL_BREADCRUMBS)
            write_log("ZOMDROID_ALPHA_SHADER_REWRITE shader=%u family=%s semantic_applied=1", shader,
                      mg_glsl_compat::pz_alpha_shader_kind_name(alpha_rewrite.kind));
#endif
        } else if (alpha_rewrite.candidate) {
#if defined(ZOMDROID_GL_BREADCRUMBS)
            const unsigned int hit = g_zomdroid_alpha_nearmiss_seq.fetch_add(1, std::memory_order_relaxed) + 1;
            if (hit <= 12) {
                write_log("ZOMDROID_ALPHA_SHADER_NEARMISS shader=%u contract_matched=%d semantic_applied=0 hit=%u",
                          shader, alpha_rewrite.contract_matched ? 1 : 0, hit);
            }
#endif
        }
    }
#endif
    int conversion_result = 0;
    const char* shader_route = "direct";

    const bool direct_shader = is_direct_shader(glsl_src.c_str());
    if (direct_shader) {
        LOG_D("[INFO] [Shader] Direct shader source: ")
        LOG_D("%s", glsl_src.c_str())
        essl_src = glsl_src;
    } else {
        shader_route = "converted";
        shader_map_uniform_defaults[shader] = mg_glsl_compat::collect_uniform_defaults(glsl_src);
        int glsl_version = getGLSLVersion(glsl_src.c_str());
        LOG_D("[INFO] [Shader] Shader source: ")
        LOG_D("%s", glsl_src.c_str())
        conversion_result = 0;
        essl_src = GLSLtoGLSLES(glsl_src.c_str(), shader_type, hardware->es_version, glsl_version, conversion_result);

        if (essl_src.empty()) {
            trace_zomdroid_shader_source(shader, shader_type, count, l, "conversion-empty", conversion_result, false,
                                         glsl_src, essl_src);
            LOG_E("Failed to convert shader %d.", shader)
            return;
        }
        if (conversion_result < 0) shader_route = "fallback-original";
        LOG_D("\n[INFO] [Shader] Converted Shader source: \n%s", essl_src.c_str())
    }
    if (direct_shader) shader_map_uniform_defaults.erase(shader);
    if (!essl_src.empty()) {
        const bool version_normalized = normalize_essl_version_directive(essl_src);
        trace_zomdroid_shader_source(shader, shader_type, count, l, shader_route, conversion_result,
                                     version_normalized, glsl_src, essl_src);
#if defined(ZOMDROID_GL_BREADCRUMBS)
        if (is_tile_depth_source) {
            LOG_I("ZOMDROID_TILEDEPTH_SOURCE shader=%u type=0x%x route=%s convert=%d", shader, shader_type,
                  shader_route, conversion_result)
        }
#endif
        shaderInfo.id = shader;
        shaderInfo.converted = essl_src;
        // The input fragments above have already been joined and converted into
        // one owned string.  Passing the caller's original `count` here made the
        // GLES driver read past this one-element pointer array whenever desktop
        // GL supplied a shader in multiple fragments.  On Adreno this first
        // surfaced as "Invalid #version" and could corrupt the native process
        // before the Java side reached the menu.
        const char* s = essl_src.c_str();
        GLES.glShaderSource(shader, 1, &s, nullptr);
        if (hardware->emulate_texture_buffer)
            shader_map_is_sampler_buffer_emulated[shader] = is_sampler_buffer_emulated;
    } else
        LOG_E("Failed to convert glsl.")
    CHECK_GL_ERROR
}

void glGetShaderiv(GLuint shader, GLenum pname, GLint* params) {
    LOG()
    GLES.glGetShaderiv(shader, pname, params);
    if (pname == GL_COMPILE_STATUS && params) trace_zomdroid_shader_status(shader, *params);
#if defined(ZOMDROID_GL_BREADCRUMBS)
    if (pname == GL_COMPILE_STATUS && params && *params != GL_TRUE) {
        GLchar info_log[1024] = {};
        GLES.glGetShaderInfoLog(shader, sizeof(info_log), nullptr, info_log);
        const auto tile_it = zomdroid_tile_depth_shader.find(shader);
        const bool is_tile_depth = tile_it != zomdroid_tile_depth_shader.end() && tile_it->second;
        LOG_W_FORCE("ZOMDROID_SHADER_FAILURE shader=%u tiledepth=%d driver=[%s]", shader,
                    is_tile_depth ? 1 : 0, info_log)
    }
#endif
    if (global_settings.ignore_error >= IgnoreErrorLevel::Partial && pname == GL_COMPILE_STATUS && !*params) {
        GLchar infoLog[512];
        GLES.glGetShaderInfoLog(shader, 512, nullptr, infoLog);
        LOG_W_FORCE("Shader %d compilation failed: \n%s", shader, infoLog)
        LOG_W_FORCE("Now try to cheat.")
        *params = GL_TRUE;
    }
    CHECK_GL_ERROR
}

GLuint glCreateShader(GLenum shaderType) {
    if (global_settings.fsr1_setting != FSR1_Quality_Preset::Disabled && !fsrInitialized) {
        InitFSRResources();
    }

    LOG()
    LOG_D("glCreateShader(%s)", glEnumToString(shaderType))
    GLuint shader = GLES.glCreateShader(shaderType);
    if (shader != 0 && hardware->emulate_texture_buffer) shader_map_is_sampler_buffer_emulated[shader] = false;
    CHECK_GL_ERROR
    return shader;
}
