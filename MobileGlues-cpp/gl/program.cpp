// MobileGlues - gl/program.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#include <regex.h>
#include "GL/glext.h"
#include "GLES3/gl32.h"
#include "log.h"
#include "shader.h"
#include "program.h"
#include "enable.h"
#include <regex>
#include <atomic>
#include <cstring>
#include <iostream>
#include "../config/settings.h"
#include "drawing.h"
#include "pz_tile_batch.h"
#include "../egl/context.h"

#define DEBUG 0

extern UnorderedMap<GLuint, bool> shader_map_is_sampler_buffer_emulated;
#if defined(ZOMDROID_GL_BREADCRUMBS)
extern UnorderedMap<GLuint, bool> zomdroid_tile_depth_shader;
#endif
UnorderedMap<GLuint, bool> program_map_is_sampler_buffer_emulated;

enum class ShouldGenerateFSState : int {
    Never = 0,
    Maybe = 1,
    Unknown = 2
};

UnorderedMap<GLuint, ShouldGenerateFSState> program_map_should_generate_fs;

using uniform_default_value = mg_glsl_compat::uniform_default_value;
using shader_uniform_defaults = UnorderedMap<GLuint, std::vector<uniform_default_value>>;
UnorderedMap<GLuint, shader_uniform_defaults> program_map_uniform_defaults;

#if defined(ZOMDROID_EXPERIMENTAL)
using pz_alpha_shader_bindings = UnorderedMap<GLuint, mg_glsl_compat::pz_alpha_shader_kind>;
UnorderedMap<GLuint, pz_alpha_shader_bindings> program_map_pz_alpha_shaders;

struct pz_alpha_program_state {
    mg_glsl_compat::pz_alpha_shader_kind kind = mg_glsl_compat::pz_alpha_shader_kind::none;
    GLint enabled_location = -1;
    GLint function_location = -1;
    GLint reference_location = -1;
    unsigned long long last_context = 0;
    GLboolean last_enabled = GL_FALSE;
    GLenum last_function = GL_ALWAYS;
    GLfloat last_reference = 0.0f;
    bool last_values_valid = false;
};
UnorderedMap<GLuint, pz_alpha_program_state> program_map_pz_alpha_state;
#endif

namespace {

bool has_program_uniform_entry_points() {
    return GLES.glProgramUniform1fv && GLES.glProgramUniform2fv && GLES.glProgramUniform3fv &&
           GLES.glProgramUniform4fv && GLES.glProgramUniform1iv && GLES.glProgramUniform2iv &&
           GLES.glProgramUniform3iv && GLES.glProgramUniform4iv;
}

void apply_float_default(GLuint program, GLint location, const uniform_default_value& value,
                         const GLfloat* components, bool direct) {
    switch (value.components) {
    case 1:
        direct ? GLES.glProgramUniform1fv(program, location, 1, components)
               : GLES.glUniform1fv(location, 1, components);
        break;
    case 2:
        direct ? GLES.glProgramUniform2fv(program, location, 1, components)
               : GLES.glUniform2fv(location, 1, components);
        break;
    case 3:
        direct ? GLES.glProgramUniform3fv(program, location, 1, components)
               : GLES.glUniform3fv(location, 1, components);
        break;
    case 4:
        direct ? GLES.glProgramUniform4fv(program, location, 1, components)
               : GLES.glUniform4fv(location, 1, components);
        break;
    }
}

void apply_integer_default(GLuint program, GLint location, const uniform_default_value& value,
                           const GLint* components, bool direct) {
    switch (value.components) {
    case 1:
        direct ? GLES.glProgramUniform1iv(program, location, 1, components)
               : GLES.glUniform1iv(location, 1, components);
        break;
    case 2:
        direct ? GLES.glProgramUniform2iv(program, location, 1, components)
               : GLES.glUniform2iv(location, 1, components);
        break;
    case 3:
        direct ? GLES.glProgramUniform3iv(program, location, 1, components)
               : GLES.glUniform3iv(location, 1, components);
        break;
    case 4:
        direct ? GLES.glProgramUniform4iv(program, location, 1, components)
               : GLES.glUniform4iv(location, 1, components);
        break;
    }
}

void apply_uniform_defaults(GLuint program) {
    const auto program_it = program_map_uniform_defaults.find(program);
    if (program_it == program_map_uniform_defaults.end() || program_it->second.empty()) return;

    GLint linked = GL_FALSE;
    GLES.glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) return;

    const bool direct = has_program_uniform_entry_points();
    GLint previous_program = 0;
    if (!direct) {
        GLES.glGetIntegerv(GL_CURRENT_PROGRAM, &previous_program);
        if (static_cast<GLuint>(previous_program) != program) GLES.glUseProgram(program);
    }

    unsigned int applied = 0;
    for (const auto& shader_entry : program_it->second) {
        for (const auto& value : shader_entry.second) {
            const char* requested_name = value.name.c_str();
            GLint location = GLES.glGetUniformLocation(program, requested_name);
            if (location < 0) {
                std::string remapped_name;
                const char* driver_name =
                    mg_glsl_compat::remap_texture_sampler_uniform_name(requested_name, remapped_name);
                if (driver_name != requested_name) location = GLES.glGetUniformLocation(program, driver_name);
            }
            if (location < 0) continue;

            if (value.integer) {
                std::array<GLint, 4> components{};
                for (unsigned int i = 0; i < value.components; ++i)
                    components[i] = static_cast<GLint>(value.values[i]);
                apply_integer_default(program, location, value, components.data(), direct);
            } else {
                std::array<GLfloat, 4> components{};
                for (unsigned int i = 0; i < value.components; ++i)
                    components[i] = static_cast<GLfloat>(value.values[i]);
                apply_float_default(program, location, value, components.data(), direct);
            }
            ++applied;
#if defined(ZOMDROID_GL_BREADCRUMBS)
            if (value.name == "useTexture") {
                ZOMDROID_DIAGNOSTIC_LOG("ZOMDROID_UNIFORM_DEFAULT program=%u name=useTexture value=%d location=%d", program,
                          static_cast<int>(value.values[0]), location);
            }
#endif
        }
    }

    if (!direct && static_cast<GLuint>(previous_program) != program)
        GLES.glUseProgram(static_cast<GLuint>(previous_program));
#if defined(ZOMDROID_GL_BREADCRUMBS)
    if (applied != 0) ZOMDROID_DIAGNOSTIC_LOG("ZOMDROID_UNIFORM_DEFAULTS program=%u applied=%u", program, applied);
#endif
}

#if defined(ZOMDROID_EXPERIMENTAL)
void configure_pz_alpha_program(GLuint program) {
    program_map_pz_alpha_state.erase(program);
    const auto attached = program_map_pz_alpha_shaders.find(program);
    if (attached == program_map_pz_alpha_shaders.end()) return;

    mg_glsl_compat::pz_alpha_shader_kind kind = mg_glsl_compat::pz_alpha_shader_kind::none;
    unsigned target_count = 0;
    for (const auto& shader : attached->second) {
        if (shader.second == mg_glsl_compat::pz_alpha_shader_kind::none) continue;
        kind = shader.second;
        ++target_count;
    }
    if (target_count != 1) return; // ambiguous/malformed programs fail closed

    GLint linked = GL_FALSE;
    GLES.glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) return;

    pz_alpha_program_state state;
    state.kind = kind;
    state.enabled_location = GLES.glGetUniformLocation(program, "zomdroidAlphaEnabled");
    state.function_location = GLES.glGetUniformLocation(program, "zomdroidAlphaFunc");
    state.reference_location = GLES.glGetUniformLocation(program, "zomdroidAlphaRef");
    if (state.enabled_location < 0 || state.function_location < 0 || state.reference_location < 0) return;
    program_map_pz_alpha_state[program] = state;

#if defined(ZOMDROID_GL_BREADCRUMBS)
    if (mg_pz_census_active) {
        static std::atomic<unsigned int> links{0};
        const unsigned int hit = links.fetch_add(1, std::memory_order_relaxed) + 1;
        if (hit <= 8) {
            ZOMDROID_DIAGNOSTIC_LOG(
                "ZOMDROID_ALPHA_PROGRAM program=%u family=%s locations=%d,%d,%d semantic_ready=1 hit=%u",
                program, mg_glsl_compat::pz_alpha_shader_kind_name(kind), state.enabled_location,
                state.function_location, state.reference_location, hit);
        }
    }
#endif
}
#endif

} // namespace

#if defined(ZOMDROID_EXPERIMENTAL)
void mg_prepare_pz_alpha_test(GLuint program) {
    const auto it = program_map_pz_alpha_state.find(program);
    if (it == program_map_pz_alpha_state.end()) return;
    pz_alpha_program_state& target = it->second;

    GLboolean enabled = GL_FALSE;
    GLenum function = GL_ALWAYS;
    GLfloat reference = 0.0f;
    mg_alpha_test_get(&enabled, &function, &reference);
    const unsigned long long context = g_current_ctx ? g_current_ctx->id : 0;
    const bool upload = !target.last_values_valid || target.last_context != context ||
                        target.last_enabled != enabled || target.last_function != function ||
                        target.last_reference != reference;
    if (upload) {
        const GLint enabled_value = enabled ? 1 : 0;
        const GLint function_value = static_cast<GLint>(function);
        GLES.glUniform1i(target.enabled_location, enabled_value);
        MG_PZ_UNIFORM_STATE(mg_pz_uniform_driver_write(program, target.enabled_location, 0x101U, 1, &enabled_value,
                                                        sizeof(enabled_value)));
        GLES.glUniform1i(target.function_location, function_value);
        MG_PZ_UNIFORM_STATE(mg_pz_uniform_driver_write(program, target.function_location, 0x101U, 1, &function_value,
                                                        sizeof(function_value)));
        GLES.glUniform1f(target.reference_location, reference);
        MG_PZ_UNIFORM_STATE(mg_pz_uniform_driver_write(program, target.reference_location, 0x301U, 1, &reference,
                                                        sizeof(reference)));
        target.last_context = context;
        target.last_enabled = enabled;
        target.last_function = function;
        target.last_reference = reference;
        target.last_values_valid = true;
    }

#if defined(ZOMDROID_GL_BREADCRUMBS)
    if (mg_pz_census_active) {
        static std::atomic<unsigned long long> family_hits[5]{};
        const size_t family = static_cast<size_t>(target.kind);
        const unsigned long long hit = family_hits[family].fetch_add(1, std::memory_order_relaxed) + 1;
        if (hit == 1 || hit == 1024 || hit == 65536) {
            ZOMDROID_DIAGNOSTIC_LOG(
                "ZOMDROID_ALPHA_DRAW program=%u family=%s enabled=%d func=0x%x ref=%.9g uniforms_uploaded=%d "
                "semantic_applied=1 hit=%llu",
                program, mg_glsl_compat::pz_alpha_shader_kind_name(target.kind), enabled ? 1 : 0, function,
                static_cast<double>(reference), upload ? 1 : 0, hit);
        }
    }
#endif
}
#endif

std::string updateLayoutLocation(const std::string& esslSource, GLuint color, const char* name) {
    const std::string& shaderCode = esslSource;

    std::string pattern = std::string(R"((layout\s*$[^)]*location\s*=\s*\d+[^)]*$\s*)?)") +
                          R"(out\s+((?:highp|mediump|lowp|\w+\s+)*\w+)\s+)" + name + R"(\s*;)";

    std::string replacement = "layout (location = " + std::to_string(color) + ") out $2 " + name + ";";

    std::regex reg(pattern);
    std::string modifiedCode = std::regex_replace(shaderCode, reg, replacement);

    return modifiedCode;
}

void glBindFragDataLocation(GLuint program, GLuint color, const GLchar* name) {
    LOG()
    LOG_D("glBindFragDataLocation(%d, %d, %s)", program, color, name)

    if (strlen(name) > 8 && strncmp(name, "outColor", 8) == 0) {
        const char* numberStr = name + 8;
        bool isNumber = true;
        for (int i = 0; numberStr[i] != '\0'; ++i) {
            if (!isdigit(numberStr[i])) {
                isNumber = false;
                break;
            }
        }

        if (isNumber) {
            unsigned int extractedColor = static_cast<unsigned int>(std::stoul(numberStr));
            if (extractedColor == color) {
                // outColor was bound in glsl process. exit now
                LOG_D("Find outColor* with color *, skipping")
                return;
            }
        }
    }

    // Copied before the call, not aliased into it: the result is assigned back
    // over the same member that supplies the input.
    const std::string origin_glsl =
        shaderInfo.frag_data_changed ? shaderInfo.frag_data_changed_converted : shaderInfo.converted;

    shaderInfo.frag_data_changed_converted = updateLayoutLocation(origin_glsl, color, name);
    shaderInfo.frag_data_changed = 1;
}

static std::string DefaultFSSource;
static unsigned CurrentDefaultFSSourceVersion = 0; // the version (hardware->es_version) may change during runtime

void GenerateDefaultFSSource() {
    if (CurrentDefaultFSSourceVersion != hardware->es_version) {
        CurrentDefaultFSSourceVersion = hardware->es_version;
        std::ostringstream ss;
        ss << "#version " << CurrentDefaultFSSourceVersion << " es\n";
        ss << "precision mediump float;\n\n";
        ss << "out vec4 fragColor;\n\n";
        ss << "void main() {\n";
        ss << "    fragColor = vec4(1.0, 1.0, 1.0, 1.0);\n";
        ss << "}\n";

        DefaultFSSource = ss.str();
    }
}

static UnorderedMap<unsigned, GLuint> DefaultFSMap; // essl version <-> shader id
void glLinkProgram(GLuint program) {
    LOG()
    MG_PZ_UNIFORM_STATE(mg_pz_census_forget_program(program));

    LOG_D("glLinkProgram(%d)", program)
    if (!shaderInfo.converted.empty() && shaderInfo.frag_data_changed) {
        const GLchar* patched = shaderInfo.frag_data_changed_converted.c_str();
        GLES.glShaderSource(shaderInfo.id, 1, &patched, nullptr);
        GLES.glCompileShader(shaderInfo.id);
        GLint status = 0;
        GLES.glGetShaderiv(shaderInfo.id, GL_COMPILE_STATUS, &status);
        if (status != GL_TRUE) {
            char tmp[500];
            GLES.glGetShaderInfoLog(shaderInfo.id, 500, nullptr, tmp);
            LOG_E("Failed to compile patched shader, log:\n%s", tmp)
        }
        GLES.glDetachShader(program, shaderInfo.id);
        GLES.glAttachShader(program, shaderInfo.id);
        CHECK_GL_ERROR
    }
    shaderInfo.id = 0;
    shaderInfo.converted = "";
    shaderInfo.frag_data_changed_converted.clear();
    shaderInfo.frag_data_changed = 0;

    // Generate defaut fragment shader if needed
    if (program_map_should_generate_fs[program] == ShouldGenerateFSState::Maybe) {
        GenerateDefaultFSSource();
        GLuint& default_fs = DefaultFSMap[CurrentDefaultFSSourceVersion];
        if (!default_fs) {
            default_fs = GLES.glCreateShader(GL_FRAGMENT_SHADER);
            const char* src = DefaultFSSource.c_str();
            GLES.glShaderSource(default_fs, 1, &src, nullptr);

            GLES.glCompileShader(default_fs);

            GLint success = 0;
            GLES.glGetShaderiv(default_fs, GL_COMPILE_STATUS, &success);
            if (!success) {
                GLint logLength = 0;
                GLES.glGetShaderiv(default_fs, GL_INFO_LOG_LENGTH, &logLength);
                std::vector<char> log(logLength);
                GLES.glGetShaderInfoLog(default_fs, logLength, nullptr, log.data());
                LOG_E("Default fragment shader compile error for program %u :\n%s\n", program, log.data());
                GLES.glDeleteShader(default_fs);
                default_fs = 0;
            }
        }

        if (default_fs) {
            LOG_D("Try to attach missing default FS for program %u...", program);
            GLES.glAttachShader(program, default_fs);
        }
    }

    GLES.glLinkProgram(program);
    apply_uniform_defaults(program);
#if defined(ZOMDROID_EXPERIMENTAL)
    configure_pz_alpha_program(program);
    mg_pz_tile_batch_program_linked(program);
#endif

    CHECK_GL_ERROR
}

void glGetProgramiv(GLuint program, GLenum pname, GLint* params) {
    LOG()
    GLES.glGetProgramiv(program, pname, params);
#if defined(ZOMDROID_GL_BREADCRUMBS)
    if (params && (pname == GL_LINK_STATUS || pname == GL_VALIDATE_STATUS) && *params != GL_TRUE) {
        GLchar info_log[1024] = {};
        GLES.glGetProgramInfoLog(program, sizeof(info_log), nullptr, info_log);
        LOG_W_FORCE("ZOMDROID_PROGRAM_FAILURE program=%u query=0x%x driver=[%s]", program, pname, info_log)
    }
#endif
    if (global_settings.ignore_error >= IgnoreErrorLevel::Partial &&
        (pname == GL_LINK_STATUS || pname == GL_VALIDATE_STATUS) && !*params) {
        GLchar infoLog[512];
        GLES.glGetProgramInfoLog(program, 512, nullptr, infoLog);

        LOG_W_FORCE("Program %d linking failed: \n%s", program, infoLog);
        LOG_W_FORCE("Now try to cheat.");
        *params = GL_TRUE;
    }
    CHECK_GL_ERROR
}

void glUseProgram(GLuint program) {
    LOG()
    LOG_D("glUseProgram(%d)", program)
    MG_PZ_CENSUS(mg_pz_census_use_program(program == gl_state->current_program));
    if (program != gl_state->current_program) {
        gl_state->current_program = program;
        GLES.glUseProgram(program);
        CHECK_GL_ERROR
    }
}

void glAttachShader(GLuint program, GLuint shader) {
    LOG()
    LOG_D("glAttachShader(%u, %u)", program, shader)
    if (hardware->emulate_texture_buffer && shader_map_is_sampler_buffer_emulated[shader])
        program_map_is_sampler_buffer_emulated[program] = true;

    auto& defaults_by_shader = program_map_uniform_defaults[program];
    const auto* defaults = mg_shader_uniform_defaults(shader);
    if (defaults && !defaults->empty())
        defaults_by_shader[shader] = *defaults;
    else
        defaults_by_shader.erase(shader);

#if defined(ZOMDROID_EXPERIMENTAL)
    mg_pz_tile_batch_attach_shader(program, shader);
    const mg_glsl_compat::pz_alpha_shader_kind alpha_kind = mg_shader_pz_alpha_kind(shader);
    auto& alpha_shaders = program_map_pz_alpha_shaders[program];
    if (alpha_kind == mg_glsl_compat::pz_alpha_shader_kind::none)
        alpha_shaders.erase(shader);
    else
        alpha_shaders[shader] = alpha_kind;
    program_map_pz_alpha_state.erase(program);
#endif

    GLint type = 0;
    GLES.glGetShaderiv(shader, GL_SHADER_TYPE, &type);
    auto& should_gen_fs_map = program_map_should_generate_fs;
    if (type == GL_FRAGMENT_SHADER) {
        should_gen_fs_map[program] = ShouldGenerateFSState::Never;
    } else if (type == GL_VERTEX_SHADER) {
        auto it = should_gen_fs_map.find(program);
        if (it == should_gen_fs_map.end() || should_gen_fs_map[program] != ShouldGenerateFSState::Never) {
            should_gen_fs_map[program] = ShouldGenerateFSState::Maybe;
        }
    }

    GLES.glAttachShader(program, shader);
#if defined(ZOMDROID_GL_BREADCRUMBS)
    if (mg_pz_census_active) {
        const auto tile_it = zomdroid_tile_depth_shader.find(shader);
        if (tile_it != zomdroid_tile_depth_shader.end() && tile_it->second) {
            LOG_I("ZOMDROID_TILEDEPTH_ATTACH program=%u shader=%u type=0x%x", program, shader, type)
        }
    }
#endif
    CHECK_GL_ERROR
}

void mg_shader_detached(GLuint program, GLuint shader) {
    const auto program_it = program_map_uniform_defaults.find(program);
    if (program_it != program_map_uniform_defaults.end()) program_it->second.erase(shader);
#if defined(ZOMDROID_EXPERIMENTAL)
    mg_pz_tile_batch_detach_shader(program, shader);
    const auto alpha_it = program_map_pz_alpha_shaders.find(program);
    if (alpha_it != program_map_pz_alpha_shaders.end()) {
        alpha_it->second.erase(shader);
        if (alpha_it->second.empty()) program_map_pz_alpha_shaders.erase(alpha_it);
    }
    program_map_pz_alpha_state.erase(program);
#endif
}

extern UnorderedMap<GLuint, SamplerInfo> g_samplerCacheForSamplerBuffer;

void mg_program_deleted(GLuint program) {
    program_map_uniform_defaults.erase(program);
    program_map_is_sampler_buffer_emulated.erase(program);
    program_map_should_generate_fs.erase(program);
    g_samplerCacheForSamplerBuffer.erase(program);
#if defined(ZOMDROID_EXPERIMENTAL)
    mg_pz_tile_batch_program_deleted(program);
    program_map_pz_alpha_shaders.erase(program);
    program_map_pz_alpha_state.erase(program);
#endif
}

GLuint glCreateProgram() {
    LOG()
    LOG_D("glCreateProgram")
    GLuint program = GLES.glCreateProgram();
    program_map_uniform_defaults.erase(program);
#if defined(ZOMDROID_EXPERIMENTAL)
    mg_pz_tile_batch_program_deleted(program);
    program_map_pz_alpha_shaders.erase(program);
    program_map_pz_alpha_state.erase(program);
#endif
    if (hardware->emulate_texture_buffer) {
        program_map_is_sampler_buffer_emulated[program] = false;
        if (g_samplerCacheForSamplerBuffer.find(program) != g_samplerCacheForSamplerBuffer.end()) {
            g_samplerCacheForSamplerBuffer.erase(program);
        }
    }
    program_map_should_generate_fs[program] = ShouldGenerateFSState::Unknown;

    CHECK_GL_ERROR
    return program;
}

// GL 3.1's name-only half of the active-uniform query, on top of the ES call that
// already returns the same string.
//
// It was a stub -- a no-op that wrote neither the name nor the length and, being a
// stub rather than an error, left glGetError clean. Callers got whatever was
// already in the buffer they passed.
//
// That is not a cosmetic gap. The standard way to build a name -> location map is
// to walk the active uniforms by index and ask for each name, and a caller doing
// that ended up with a map keyed on garbage: every later lookup missed, so the
// uniforms never got set and kept whatever the driver had zero-initialised them
// to. NeoForge's early loading window does exactly this, and a screenSize of
// (0, 0) turned its every vertex into a division by zero -- gl_Position came out
// non-finite, every primitive was discarded, and the window rendered black with
// nothing anywhere reporting a problem.
void glGetActiveUniformName(GLuint program, GLuint uniformIndex, GLsizei bufSize, GLsizei* length,
                            GLchar* uniformName) {
    LOG()
    LOG_D("glGetActiveUniformName(program: %u, index: %u, bufSize: %d)", program, uniformIndex, bufSize)

    if (length) *length = 0;
    if (bufSize <= 0 || uniformName == nullptr) {
        // Nothing to write. Still forwarded when bufSize is negative so the driver
        // raises the GL_INVALID_VALUE the caller is owed.
        if (bufSize < 0) GLES.glGetActiveUniform(program, uniformIndex, bufSize, nullptr, nullptr, nullptr, nullptr);
        CHECK_GL_ERROR
        return;
    }

    // Same buffer contract in both calls: at most bufSize-1 characters plus the
    // terminator, and a length that excludes it. The size and type this also
    // returns are what glGetActiveUniformsiv is for; they are discarded here.
    GLint size = 0;
    GLenum type = 0;
    GLsizei written = 0;
    uniformName[0] = '\0';
    GLES.glGetActiveUniform(program, uniformIndex, bufSize, &written, &size, &type, uniformName);
    if (length) *length = written;

    LOG_D("  -> \"%s\"", uniformName)
    CHECK_GL_ERROR
}
