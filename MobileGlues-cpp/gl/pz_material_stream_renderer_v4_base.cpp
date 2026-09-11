// MobileGlues - gl/pz_material_stream_renderer.cpp
// Project Zomboid experiment V4: material-stream renderer for one exact
// tile/sprite shader family. It preserves draw order, snapshots per-draw
// material/vertex state, remaps compatible 2D textures into texture arrays and
// emits one instanced backend draw per hard-state segment.

#define mg_pz_repack_renderer_install mg_pz_repack_renderer_install_v2_impl
#define pz_repack_before_backend_command pz_repack_before_backend_command_v2_impl
#include "pz_repack_renderer.cpp"
#undef pz_repack_before_backend_command
#undef mg_pz_repack_renderer_install

#if defined(ZOMDROID_EXPERIMENTAL)

#include <array>
#include <chrono>
#include <cmath>
#include <regex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr size_t kCollectorLimit = 512;
constexpr size_t kTextureBankBudget = 32U * 1024U * 1024U;
constexpr size_t kTextureTotalBudget = 96U * 1024U * 1024U;
constexpr GLint kMaxBankLayers = 64;
constexpr size_t kMaterialShaderTextLimit = 1024U * 1024U;

struct stream_instance_t {
    float pos_uv[6][4];
    float color[6][4];
    float mvp[16];
    float material[4]; // zDepth, chunkDepth, texture layer, useTexture
};
static_assert(sizeof(stream_instance_t) == 272, "std430 stream instance layout changed");

struct material_wrapped_t {
    glUseProgram_PTR use_program = nullptr;
    glShaderSource_PTR shader_source = nullptr;
    glLinkProgram_PTR link_program = nullptr;
    glDeleteProgram_PTR delete_program = nullptr;
    glActiveTexture_PTR active_texture = nullptr;
    glBindTexture_PTR bind_texture = nullptr;
    glUniform1f_PTR uniform1f = nullptr;
    glUniform1i_PTR uniform1i = nullptr;
    glUniformMatrix4fv_PTR uniform_matrix4fv = nullptr;

    glTexImage2D_PTR tex_image_2d = nullptr;
    glTexSubImage2D_PTR tex_sub_image_2d = nullptr;
    glCompressedTexImage2D_PTR compressed_tex_image_2d = nullptr;
    glCompressedTexSubImage2D_PTR compressed_tex_sub_image_2d = nullptr;
    glCopyTexImage2D_PTR copy_tex_image_2d = nullptr;
    glCopyTexSubImage2D_PTR copy_tex_sub_image_2d = nullptr;
    glTexStorage2D_PTR tex_storage_2d = nullptr;
    glGenerateMipmap_PTR generate_mipmap = nullptr;
    glTexParameteri_PTR tex_parameter_i = nullptr;
    glTexParameterf_PTR tex_parameter_f = nullptr;
    glTexParameteriv_PTR tex_parameter_iv = nullptr;
    glTexParameterfv_PTR tex_parameter_fv = nullptr;
    glDeleteTextures_PTR delete_textures = nullptr;
    glCopyImageSubData_PTR copy_image_sub_data = nullptr;
};

struct material_backend_t {
    glCreateShader_PTR create_shader = nullptr;
    glCompileShader_PTR compile_shader = nullptr;
    glGetShaderiv_PTR get_shader_iv = nullptr;
    glGetShaderInfoLog_PTR get_shader_info_log = nullptr;
    glDeleteShader_PTR delete_shader = nullptr;
    glCreateProgram_PTR create_program = nullptr;
    glGetProgramiv_PTR get_program_iv = nullptr;
    glGetProgramInfoLog_PTR get_program_info_log = nullptr;
    glGetUniformLocation_PTR get_uniform_location = nullptr;
    glUniform1i_PTR uniform1i = nullptr;
    glDrawArraysInstanced_PTR draw_arrays_instanced = nullptr;
    glBindBufferBase_PTR bind_buffer_base = nullptr;
    glBindBufferRange_PTR bind_buffer_range = nullptr;
    glGetIntegeri_v_PTR get_integer_i_v = nullptr;
    glGetInteger64i_v_PTR get_integer64_i_v = nullptr;
    glGetIntegerv_PTR get_integerv = nullptr;
    glGetTexLevelParameteriv_PTR get_tex_level_parameter_iv = nullptr;
    glGetTexParameteriv_PTR get_tex_parameter_iv = nullptr;
    glGenTextures_PTR gen_textures = nullptr;
    glDeleteTextures_PTR delete_textures = nullptr;
    glActiveTexture_PTR active_texture = nullptr;
    glBindTexture_PTR bind_texture = nullptr;
    glTexStorage3D_PTR tex_storage_3d = nullptr;
    glTexParameteri_PTR tex_parameter_i = nullptr;
    glCopyImageSubData_PTR copy_image_sub_data = nullptr;
};

struct shader_source_t {
    std::string source;
};

struct material_program_t {
    bool classified = false;
    bool compatible = false;
    GLuint stream_program = 0;
    GLint mvp = -1;
    GLint z_depth = -1;
    GLint chunk_depth = -1;
    GLint diffuse = -1;
    GLint use_texture = -1;
    std::array<float, 16> mvp_value{};
    float z_depth_value = 0.0f;
    float chunk_depth_value = 0.0f;
    GLint diffuse_unit = 0;
    GLint use_texture_value = 0;
};

struct texture_key_t {
    GLint width = 0;
    GLint height = 0;
    GLint internal_format = 0;
    GLint min_filter = 0;
    GLint mag_filter = 0;
    GLint wrap_s = 0;
    GLint wrap_t = 0;

    bool operator==(const texture_key_t& other) const {
        return width == other.width && height == other.height && internal_format == other.internal_format &&
               min_filter == other.min_filter && mag_filter == other.mag_filter && wrap_s == other.wrap_s &&
               wrap_t == other.wrap_t;
    }
};

struct texture_bank_t {
    texture_key_t key{};
    GLuint texture = 0;
    GLint capacity = 0;
    GLint used = 0;
    size_t bytes = 0;
};

struct texture_layer_t {
    unsigned long long generation = 0;
    size_t bank = 0;
    GLint layer = 0;
};

struct material_stats_t {
    unsigned long long draws_seen = 0;
    unsigned long long draws_captured = 0;
    unsigned long long hard_segments = 0;
    unsigned long long texture_array_hits = 0;
    unsigned long long instances = 0;
    unsigned long long backend_draws = 0;
    unsigned long long draws_eliminated = 0;
    unsigned long long packed_bytes = 0;
    double compiler_ms = 0.0;
    unsigned long long fallback_draws = 0;
};

struct material_state_t {
    std::unordered_map<GLuint, shader_source_t> shaders;
    std::unordered_map<GLuint, material_program_t> programs;
    std::unordered_map<std::string, GLuint> variants;

    GLenum active_texture = GL_TEXTURE0;
    std::vector<GLuint> bound_2d;
    std::unordered_map<GLuint, unsigned long long> texture_generation;
    std::unordered_map<GLuint, texture_layer_t> texture_layers;
    std::vector<texture_bank_t> banks;
    size_t texture_bytes = 0;

    bool limits_known = false;
    GLint max_texture_units = 0;
    GLint max_array_layers = 0;
    GLint max_ssbo_bindings = 0;
    GLint internal_texture_unit = -1;
    GLuint ssbo_binding = 0;

    GLuint instance_buffer = 0;
    GLuint stream_vao = 0;

    bool collector_active = false;
    GLuint collector_program = 0;
    size_t collector_bank = 0;
    std::vector<stream_instance_t> collector;

    material_stats_t stats{};
};

material_wrapped_t g_material_wrapped;
material_backend_t g_material_backend;
bool g_material_enabled = false;
thread_local material_state_t g_material;

bool collect_shader_source(GLsizei count, const GLchar* const* strings, const GLint* lengths, std::string* out) {
    if (out == nullptr || count <= 0 || strings == nullptr) return false;
    std::string source;
    try {
        for (GLsizei i = 0; i < count; ++i) {
            if (strings[i] == nullptr) return false;
            size_t bytes = lengths != nullptr && lengths[i] >= 0 ? static_cast<size_t>(lengths[i])
                                                                 : std::strlen(strings[i]);
            if (bytes > kMaterialShaderTextLimit || source.size() > kMaterialShaderTextLimit - bytes) return false;
            source.append(strings[i], bytes);
        }
    } catch (...) {
        return false;
    }
    *out = std::move(source);
    return true;
}

size_t regex_count(const std::string& source, const std::regex& pattern) {
    size_t count = 0;
    for (std::sregex_iterator it(source.begin(), source.end(), pattern), end; it != end; ++it) ++count;
    return count;
}

bool replace_unique(std::string* source, const std::regex& pattern, const std::string& replacement) {
    if (source == nullptr) return false;
    std::sregex_iterator it(source->begin(), source->end(), pattern), end;
    if (it == end) return false;
    const std::smatch first = *it;
    ++it;
    if (it != end) return false;
    source->replace(static_cast<size_t>(first.position()), static_cast<size_t>(first.length()), replacement);
    return true;
}

bool insert_after_precision(std::string* source, const std::string& declarations) {
    if (source == nullptr) return false;
    size_t position = source->find("precision highp int;");
    if (position != std::string::npos) {
        position += std::strlen("precision highp int;");
        source->insert(position, "\n" + declarations + "\n");
        return true;
    }
    position = source->find('\n');
    if (position == std::string::npos) return false;
    source->insert(position + 1, declarations + "\n");
    return true;
}

bool only_uniforms(const std::string& source, const std::vector<std::string>& allowed) {
    static const std::regex uniform(
        R"(\buniform\s+(?:(?:lowp|mediump|highp)\s+)?[A-Za-z_]\w*\s+([A-Za-z_]\w*))",
        std::regex::ECMAScript);
    for (std::sregex_iterator it(source.begin(), source.end(), uniform), end; it != end; ++it) {
        const std::string name = (*it)[1].str();
        if (std::find(allowed.begin(), allowed.end(), name) == allowed.end()) return false;
    }
    return true;
}

bool transform_vertex_source(const std::string& original, GLint ssbo_binding, std::string* transformed) {
    if (transformed == nullptr || ssbo_binding < 0) return false;
    if (!only_uniforms(original, {"ModelViewProjection", "chunkDepth", "zDepth"})) return false;

    static const std::regex pos_decl(
        R"(layout\s*\(\s*location\s*=\s*0\s*\)\s*in\s+(?:(?:lowp|mediump|highp)\s+)?vec2\s+vPos\s*;)");
    static const std::regex uv_decl(
        R"(layout\s*\(\s*location\s*=\s*1\s*\)\s*in\s+(?:(?:lowp|mediump|highp)\s+)?vec2\s+vUV\s*;)");
    static const std::regex color_decl(
        R"(layout\s*\(\s*location\s*=\s*2\s*\)\s*in\s+(?:(?:lowp|mediump|highp)\s+)?vec4\s+vCol\s*;)");
    static const std::regex mvp_decl(
        R"(uniform\s+(?:(?:lowp|mediump|highp)\s+)?mat4\s+ModelViewProjection\s*;)");
    static const std::regex chunk_decl(
        R"(uniform\s+(?:(?:lowp|mediump|highp)\s+)?float\s+chunkDepth\s*;)");
    static const std::regex z_decl(
        R"(uniform\s+(?:(?:lowp|mediump|highp)\s+)?float\s+zDepth\s*;)");
    static const std::regex any_input(R"(layout\s*\(\s*location\s*=\s*[0-9]+\s*\)\s*in\s+)");
    static const std::regex main_decl(R"(void\s+main\s*\(\s*\)\s*\{)");

    if (regex_count(original, any_input) != 3 || regex_count(original, pos_decl) != 1 ||
        regex_count(original, uv_decl) != 1 || regex_count(original, color_decl) != 1 ||
        regex_count(original, mvp_decl) != 1 || regex_count(original, chunk_decl) != 1 ||
        regex_count(original, z_decl) != 1 || regex_count(original, main_decl) != 1)
        return false;

    std::string source = original;
    if (!replace_unique(&source, pos_decl, "vec2 vPos;") || !replace_unique(&source, uv_decl, "vec2 vUV;") ||
        !replace_unique(&source, color_decl, "vec4 vCol;") ||
        !replace_unique(&source, mvp_decl, "mat4 ModelViewProjection;") ||
        !replace_unique(&source, chunk_decl, "float chunkDepth;") ||
        !replace_unique(&source, z_decl, "float zDepth;"))
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
        "flat out highp int zomdroidStreamUseTexture;";
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
        "    chunkDepth = zomdroidStream.material.y;\n"
        "    zomdroidStreamLayer = int(zomdroidStream.material.z);\n"
        "    zomdroidStreamUseTexture = int(zomdroidStream.material.w);\n";
    source.insert(body, setup);
    *transformed = std::move(source);
    return true;
}

bool transform_fragment_source(const std::string& original, std::string* transformed) {
    if (transformed == nullptr || !only_uniforms(original, {"DIFFUSE", "useTexture"})) return false;
    if (original.find("DEPTH") != std::string::npos || original.find("MASK") != std::string::npos ||
        original.find("gl_FragDepth") != std::string::npos)
        return false;

    static const std::regex sampler_decl(
        R"(uniform\s+(?:(?:lowp|mediump|highp)\s+)?sampler2D\s+DIFFUSE\s*;)");
    static const std::regex use_decl(
        R"(uniform\s+(?:(?:lowp|mediump|highp)\s+)?int\s+useTexture\s*;)");
    static const std::regex texcoord_decl(
        R"(in\s+(?:(?:lowp|mediump|highp)\s+)?vec2\s+texCoord\s*;)");
    static const std::regex color_decl(
        R"(in\s+(?:(?:lowp|mediump|highp)\s+)?vec4\s+col\s*;)");
    static const std::regex sample(
        R"(texture\s*\(\s*DIFFUSE\s*,\s*texCoord\s*\))");
    static const std::regex main_decl(R"(void\s+main\s*\(\s*\)\s*\{)");
    static const std::regex any_sampler(R"(\bsampler[A-Za-z0-9_]*\b)");

    if (regex_count(original, sampler_decl) != 1 || regex_count(original, use_decl) != 1 ||
        regex_count(original, texcoord_decl) != 1 || regex_count(original, color_decl) != 1 ||
        regex_count(original, sample) != 1 || regex_count(original, main_decl) != 1 ||
        regex_count(original, any_sampler) != 1)
        return false;

    std::string source = original;
    if (!replace_unique(&source, sampler_decl, "uniform highp sampler2DArray zomdroidStreamDiffuse;") ||
        !replace_unique(&source, use_decl, "int useTexture;") ||
        !replace_unique(&source, sample,
                        "texture(zomdroidStreamDiffuse, vec3(texCoord, float(zomdroidStreamLayer)))"))
        return false;

    const std::string declarations =
        "flat in highp int zomdroidStreamLayer;\n"
        "flat in highp int zomdroidStreamUseTexture;";
    if (!insert_after_precision(&source, declarations)) return false;

    std::smatch main_match;
    if (!std::regex_search(source, main_match, main_decl)) return false;
    const size_t body = static_cast<size_t>(main_match.position() + main_match.length());
    source.insert(body, "\n    useTexture = zomdroidStreamUseTexture;\n");
    *transformed = std::move(source);
    return true;
}

bool ensure_limits() {
    if (g_material.limits_known) return g_material.max_texture_units > 0 && g_material.max_array_layers > 0 &&
                                        g_material.max_ssbo_bindings > 0;
    g_material.limits_known = true;
    if (!g_material_backend.get_integerv) return false;
    g_material_backend.get_integerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &g_material.max_texture_units);
    g_material_backend.get_integerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &g_material.max_array_layers);
    g_material_backend.get_integerv(GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, &g_material.max_ssbo_bindings);
    if (g_material.max_texture_units <= 0 || g_material.max_array_layers <= 0 || g_material.max_ssbo_bindings <= 0)
        return false;
    g_material.internal_texture_unit = g_material.max_texture_units - 1;
    g_material.ssbo_binding = static_cast<GLuint>(g_material.max_ssbo_bindings - 1);
    g_material.bound_2d.assign(static_cast<size_t>(g_material.max_texture_units), 0);
    return true;
}

GLuint compile_shader(GLenum type, const std::string& source) {
    GLuint shader = g_material_backend.create_shader(type);
    if (shader == 0) return 0;
    const GLchar* pointer = source.c_str();
    const GLint length = static_cast<GLint>(source.size());
    g_orig.shader_source(shader, 1, &pointer, &length);
    g_material_backend.compile_shader(shader);
    GLint status = GL_FALSE;
    g_material_backend.get_shader_iv(shader, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        GLchar log[1024]{};
        GLsizei written = 0;
        if (g_material_backend.get_shader_info_log)
            g_material_backend.get_shader_info_log(shader, sizeof(log), &written, log);
        LOG_W_FORCE("ZOMDROID_PZ_MATERIAL_STREAM compiler_fail stage=shader type=0x%x log=%s", type, log)
        g_material_backend.delete_shader(shader);
        return 0;
    }
    return shader;
}

GLuint compile_stream_program(const std::string& vertex, const std::string& fragment) {
    const auto started = std::chrono::steady_clock::now();
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vertex);
    GLuint fs = vs ? compile_shader(GL_FRAGMENT_SHADER, fragment) : 0;
    GLuint program = 0;
    if (vs && fs) {
        program = g_material_backend.create_program();
        if (program) {
            g_orig.attach_shader(program, vs);
            g_orig.attach_shader(program, fs);
            g_orig.link_program(program);
            GLint status = GL_FALSE;
            g_material_backend.get_program_iv(program, GL_LINK_STATUS, &status);
            if (status != GL_TRUE) {
                GLchar log[1024]{};
                GLsizei written = 0;
                if (g_material_backend.get_program_info_log)
                    g_material_backend.get_program_info_log(program, sizeof(log), &written, log);
                LOG_W_FORCE("ZOMDROID_PZ_MATERIAL_STREAM compiler_fail stage=link log=%s", log)
                g_orig.delete_program(program);
                program = 0;
            }
        }
    }
    if (vs) g_material_backend.delete_shader(vs);
    if (fs) g_material_backend.delete_shader(fs);
    const auto ended = std::chrono::steady_clock::now();
    g_material.stats.compiler_ms += std::chrono::duration<double, std::milli>(ended - started).count();
    return program;
}

GLuint stream_variant_for(const std::string& vertex, const std::string& fragment) {
    const std::string key = vertex + "\n//__ZOMDROID_STREAM_SPLIT__\n" + fragment;
    const auto found = g_material.variants.find(key);
    if (found != g_material.variants.end()) return found->second;
    GLuint program = compile_stream_program(vertex, fragment);
    if (program) {
        const GLint sampler = g_material_backend.get_uniform_location(program, "zomdroidStreamDiffuse");
        if (sampler < 0) {
            g_orig.delete_program(program);
            program = 0;
        } else {
            const GLuint restore = g_state.program;
            g_orig.use_program(program);
            g_material_backend.uniform1i(sampler, g_material.internal_texture_unit);
            g_orig.use_program(restore);
        }
    }
    g_material.variants.emplace(key, program);
    return program;
}

void classify_program(GLuint program) {
    material_program_t info{};
    info.classified = true;
    if (!ensure_limits() || !program_safe(program)) {
        g_material.programs[program] = info;
        return;
    }

    const auto tracked = g_state.programs.find(program);
    if (tracked == g_state.programs.end() || tracked->second.attached.size() != 2) {
        g_material.programs[program] = info;
        return;
    }

    std::string vertex;
    std::string fragment;
    for (GLuint shader : tracked->second.attached) {
        const auto source = g_material.shaders.find(shader);
        if (source == g_material.shaders.end()) {
            g_material.programs[program] = info;
            return;
        }
        GLint type = 0;
        g_material_backend.get_shader_iv(shader, GL_SHADER_TYPE, &type);
        if (type == GL_VERTEX_SHADER && vertex.empty())
            vertex = source->second.source;
        else if (type == GL_FRAGMENT_SHADER && fragment.empty())
            fragment = source->second.source;
        else {
            g_material.programs[program] = info;
            return;
        }
    }
    if (vertex.empty() || fragment.empty()) {
        g_material.programs[program] = info;
        return;
    }

    std::string stream_vertex;
    std::string stream_fragment;
    if (!transform_vertex_source(vertex, static_cast<GLint>(g_material.ssbo_binding), &stream_vertex) ||
        !transform_fragment_source(fragment, &stream_fragment)) {
        g_material.programs[program] = info;
        return;
    }

    info.stream_program = stream_variant_for(stream_vertex, stream_fragment);
    if (!info.stream_program) {
        g_material.programs[program] = info;
        return;
    }

    info.mvp = g_material_backend.get_uniform_location(program, "ModelViewProjection");
    info.z_depth = g_material_backend.get_uniform_location(program, "zDepth");
    info.chunk_depth = g_material_backend.get_uniform_location(program, "chunkDepth");
    info.diffuse = g_material_backend.get_uniform_location(program, "DIFFUSE");
    info.use_texture = g_material_backend.get_uniform_location(program, "useTexture");
    info.compatible = info.mvp >= 0 && info.z_depth >= 0 && info.chunk_depth >= 0 && info.diffuse >= 0 &&
                      info.use_texture >= 0;
    g_material.programs[program] = info;
}

material_program_t* current_material_program() {
    const auto found = g_material.programs.find(g_state.program);
    return found == g_material.programs.end() ? nullptr : &found->second;
}

float half_to_float(uint16_t value) {
    const uint32_t sign = static_cast<uint32_t>(value & 0x8000U) << 16U;
    uint32_t exponent = (value >> 10U) & 0x1fU;
    uint32_t mantissa = value & 0x3ffU;
    uint32_t bits = 0;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            exponent = 127U - 15U + 1U;
            while ((mantissa & 0x400U) == 0) {
                mantissa <<= 1U;
                --exponent;
            }
            mantissa &= 0x3ffU;
            bits = sign | (exponent << 23U) | (mantissa << 13U);
        }
    } else if (exponent == 31U) {
        bits = sign | 0x7f800000U | (mantissa << 13U);
    } else {
        bits = sign | ((exponent + (127U - 15U)) << 23U) | (mantissa << 13U);
    }
    float result = 0.0f;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

bool decode_component(const unsigned char* source, GLenum type, GLboolean normalized, float* out) {
    if (!source || !out) return false;
    switch (type) {
    case GL_FLOAT:
        std::memcpy(out, source, sizeof(float));
        return true;
    case GL_HALF_FLOAT: {
        uint16_t value = 0;
        std::memcpy(&value, source, sizeof(value));
        *out = half_to_float(value);
        return true;
    }
    case GL_UNSIGNED_BYTE: {
        const uint8_t value = *source;
        *out = normalized ? static_cast<float>(value) / 255.0f : static_cast<float>(value);
        return true;
    }
    case GL_BYTE: {
        int8_t value = 0;
        std::memcpy(&value, source, sizeof(value));
        *out = normalized ? std::max(-1.0f, static_cast<float>(value) / 127.0f) : static_cast<float>(value);
        return true;
    }
    case GL_UNSIGNED_SHORT: {
        uint16_t value = 0;
        std::memcpy(&value, source, sizeof(value));
        *out = normalized ? static_cast<float>(value) / 65535.0f : static_cast<float>(value);
        return true;
    }
    case GL_SHORT: {
        int16_t value = 0;
        std::memcpy(&value, source, sizeof(value));
        *out = normalized ? std::max(-1.0f, static_cast<float>(value) / 32767.0f) : static_cast<float>(value);
        return true;
    }
    default:
        return false;
    }
}

bool decode_attribute(const candidate_t& candidate, GLuint attribute, unsigned components, size_t vertex,
                      float* output) {
    if (attribute >= kTrackedAttribs || vertex >= candidate.indices.size() || !output || components == 0 ||
        components > 4)
        return false;
    const attrib_t& a = candidate.attribs[attribute];
    if (!a.enabled || !a.described || a.integer_format || a.size != static_cast<GLint>(components) || a.buffer == 0)
        return false;
    const unsigned scalar = scalar_size(a.type);
    if (scalar == 0 || a.type == GL_FIXED || a.type == GL_INT_2_10_10_10_REV ||
        a.type == GL_UNSIGNED_INT_2_10_10_10_REV)
        return false;
    const uint64_t stride = a.stride ? static_cast<uint64_t>(a.stride) : scalar * components;
    const uint64_t offset = static_cast<uint64_t>(a.pointer) + static_cast<uint64_t>(candidate.indices[vertex]) * stride;
    const unsigned char* bytes = nullptr;
    if (!mapped_range(a.buffer, offset, static_cast<uint64_t>(scalar) * components, &bytes)) return false;
    for (unsigned component = 0; component < components; ++component)
        if (!decode_component(bytes + component * scalar, a.type, a.normalized, &output[component])) return false;
    return true;
}

bool build_stream_instance(const candidate_t& candidate, const material_program_t& program, GLint layer,
                           stream_instance_t* instance) {
    if (!instance) return false;
    stream_instance_t out{};
    for (size_t vertex = 0; vertex < 6; ++vertex) {
        float pos[2]{};
        float uv[2]{};
        float color[4]{};
        if (!decode_attribute(candidate, 0, 2, vertex, pos) || !decode_attribute(candidate, 1, 2, vertex, uv) ||
            !decode_attribute(candidate, 2, 4, vertex, color))
            return false;
        out.pos_uv[vertex][0] = pos[0];
        out.pos_uv[vertex][1] = pos[1];
        out.pos_uv[vertex][2] = uv[0];
        out.pos_uv[vertex][3] = uv[1];
        for (unsigned c = 0; c < 4; ++c) out.color[vertex][c] = color[c];
    }
    std::copy(program.mvp_value.begin(), program.mvp_value.end(), out.mvp);
    out.material[0] = program.z_depth_value;
    out.material[1] = program.chunk_depth_value;
    out.material[2] = static_cast<float>(layer);
    out.material[3] = static_cast<float>(program.use_texture_value);
    *instance = out;
    return true;
}

unsigned long long texture_generation(GLuint texture) {
    auto [it, inserted] = g_material.texture_generation.emplace(texture, 1ULL);
    return it->second;
}

void invalidate_texture(GLuint texture) {
    if (!texture) return;
    auto [it, inserted] = g_material.texture_generation.emplace(texture, 1ULL);
    if (!inserted) ++it->second;
}

bool supported_internal_format(GLint format) {
    switch (format) {
    case GL_RGBA8:
    case GL_RGB8:
    case GL_R8:
    case GL_RG8:
#ifdef GL_SRGB8
    case GL_SRGB8:
#endif
#ifdef GL_SRGB8_ALPHA8
    case GL_SRGB8_ALPHA8:
#endif
        return true;
    default:
        return false;
    }
}

bool query_texture_key(GLint unit, GLuint texture, texture_key_t* key) {
    if (!key || texture == 0 || !ensure_limits() || unit < 0 || unit >= g_material.max_texture_units) return false;
    GLint sampler = 0;
    g_material_backend.get_integer_i_v(GL_SAMPLER_BINDING, static_cast<GLuint>(unit), &sampler);
    if (sampler != 0) return false;

    const GLenum restore_active = g_material.active_texture;
    g_material_backend.active_texture(GL_TEXTURE0 + static_cast<GLenum>(unit));
    GLint bound = 0;
    g_material_backend.get_integerv(GL_TEXTURE_BINDING_2D, &bound);
    if (bound != static_cast<GLint>(texture)) {
        g_material_backend.active_texture(restore_active);
        return false;
    }

    texture_key_t out{};
    g_material_backend.get_tex_level_parameter_iv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &out.width);
    g_material_backend.get_tex_level_parameter_iv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &out.height);
    g_material_backend.get_tex_level_parameter_iv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &out.internal_format);
    g_material_backend.get_tex_parameter_iv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &out.min_filter);
    g_material_backend.get_tex_parameter_iv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &out.mag_filter);
    g_material_backend.get_tex_parameter_iv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &out.wrap_s);
    g_material_backend.get_tex_parameter_iv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, &out.wrap_t);
    g_material_backend.active_texture(restore_active);

    if (out.width <= 0 || out.height <= 0 || !supported_internal_format(out.internal_format)) return false;
    if ((out.min_filter != GL_NEAREST && out.min_filter != GL_LINEAR) ||
        (out.mag_filter != GL_NEAREST && out.mag_filter != GL_LINEAR))
        return false;
    *key = out;
    return true;
}

bool create_texture_bank(const texture_key_t& key, size_t* bank_index) {
    if (!bank_index || !ensure_limits()) return false;
    const uint64_t pixels = static_cast<uint64_t>(key.width) * static_cast<uint64_t>(key.height);
    if (pixels == 0 || pixels > std::numeric_limits<size_t>::max() / 4U) return false;
    const size_t layer_bytes = static_cast<size_t>(pixels) * 4U;
    if (layer_bytes == 0 || layer_bytes > kTextureBankBudget || g_material.texture_bytes >= kTextureTotalBudget)
        return false;
    const size_t remaining = kTextureTotalBudget - g_material.texture_bytes;
    GLint capacity = static_cast<GLint>(std::min<size_t>(
        static_cast<size_t>(std::min(g_material.max_array_layers, kMaxBankLayers)),
        std::min(kTextureBankBudget / layer_bytes, remaining / layer_bytes)));
    if (capacity <= 0) return false;

    texture_bank_t bank{};
    bank.key = key;
    bank.capacity = capacity;
    bank.bytes = layer_bytes * static_cast<size_t>(capacity);
    g_material_backend.gen_textures(1, &bank.texture);
    if (!bank.texture) return false;

    const GLenum restore_active = g_material.active_texture;
    g_material_backend.active_texture(GL_TEXTURE0 + static_cast<GLenum>(g_material.internal_texture_unit));
    GLint restore_array = 0;
    g_material_backend.get_integerv(GL_TEXTURE_BINDING_2D_ARRAY, &restore_array);
    g_material_backend.bind_texture(GL_TEXTURE_2D_ARRAY, bank.texture);
    g_material_backend.tex_storage_3d(GL_TEXTURE_2D_ARRAY, 1, static_cast<GLenum>(key.internal_format), key.width,
                                      key.height, capacity);
    g_material_backend.tex_parameter_i(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, key.min_filter);
    g_material_backend.tex_parameter_i(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, key.mag_filter);
    g_material_backend.tex_parameter_i(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, key.wrap_s);
    g_material_backend.tex_parameter_i(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, key.wrap_t);
    g_material_backend.tex_parameter_i(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_BASE_LEVEL, 0);
    g_material_backend.tex_parameter_i(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, 0);
    g_material_backend.bind_texture(GL_TEXTURE_2D_ARRAY, static_cast<GLuint>(restore_array));
    g_material_backend.active_texture(restore_active);

    *bank_index = g_material.banks.size();
    g_material.texture_bytes += bank.bytes;
    g_material.banks.push_back(bank);
    return true;
}

bool ensure_texture_layer(GLint unit, GLuint texture, size_t* bank_index, GLint* layer) {
    if (!bank_index || !layer) return false;
    texture_key_t key{};
    if (!query_texture_key(unit, texture, &key)) return false;
    const unsigned long long generation = texture_generation(texture);
    auto existing = g_material.texture_layers.find(texture);
    if (existing != g_material.texture_layers.end() && existing->second.generation == generation &&
        existing->second.bank < g_material.banks.size() && g_material.banks[existing->second.bank].key == key) {
        *bank_index = existing->second.bank;
        *layer = existing->second.layer;
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
            if (!create_texture_bank(key, &bank)) return false;
            destination_layer = g_material.banks[bank].used++;
        }
    }

    texture_bank_t& destination = g_material.banks[bank];
    g_material_backend.copy_image_sub_data(texture, GL_TEXTURE_2D, 0, 0, 0, 0, destination.texture,
                                           GL_TEXTURE_2D_ARRAY, 0, 0, 0, destination_layer, key.width, key.height, 1);
    g_material.texture_layers[texture] = texture_layer_t{generation, bank, destination_layer};
    *bank_index = bank;
    *layer = destination_layer;
    return true;
}

bool ensure_stream_objects() {
    if (g_material.instance_buffer == 0) g_orig.gen_buffers(1, &g_material.instance_buffer);
    if (g_material.stream_vao == 0) g_orig.gen_vertex_arrays(1, &g_material.stream_vao);
    return g_material.instance_buffer != 0 && g_material.stream_vao != 0;
}

void report_material_stream() {
    const material_stats_t& s = g_material.stats;
    LOG_I("ZOMDROID_PZ_MATERIAL_STREAM draws_seen=%llu draws_captured=%llu hard_segments=%llu "
          "texture_array_hits=%llu instances=%llu backend_draws=%llu draws_eliminated=%llu packed_bytes=%llu "
          "compiler_ms=%.3f fallback_draws=%llu pending=%llu banks=%llu",
          s.draws_seen, s.draws_captured, s.hard_segments, s.texture_array_hits, s.instances, s.backend_draws,
          s.draws_eliminated, s.packed_bytes, s.compiler_ms, s.fallback_draws,
          static_cast<unsigned long long>(g_material.collector.size()),
          static_cast<unsigned long long>(g_material.banks.size()))
}

void maybe_report_material_stream() {
    const unsigned long long n = g_material.stats.draws_seen;
    if (n == 1 || n == 1024 || n == 65536 || (n != 0 && n % 250000ULL == 0)) report_material_stream();
}

void emit_collector() {
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

    const GLuint restore_program = g_state.program;
    const GLuint restore_vao = g_state.vao;
    const GLenum restore_active = g_material.active_texture;

    GLint restore_ssbo_generic = 0;
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
    g_orig.use_program(g_material.collector_program);
    g_orig.bind_vertex_array(g_material.stream_vao);
    g_material_backend.draw_arrays_instanced(GL_TRIANGLES, 0, 6, static_cast<GLsizei>(g_material.collector.size()));

    g_orig.bind_vertex_array(restore_vao);
    g_orig.use_program(restore_program);
    if (restore_ssbo_indexed != 0 && restore_ssbo_size > 0)
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
    g_material.collector.clear();
}

void hard_flush() {
    if (!g_material.collector_active || g_material.collector.empty()) return;
    ++g_material.stats.hard_segments;
    emit_collector();
}

void soft_segment_flush() {
    if (!g_material.collector_active || g_material.collector.empty()) return;
    emit_collector();
}

void fallback_draw(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    hard_flush();
    ++g_material.stats.fallback_draws;
    g_orig.draw_elements(mode, count, type, indices);
}

void material_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    ++g_material.stats.draws_seen;
    material_program_t* program = current_material_program();
    if (!program || !program->compatible || program->use_texture_value != 1 || program->diffuse_unit < 0 ||
        !ensure_limits() || program->diffuse_unit >= g_material.max_texture_units) {
        fallback_draw(mode, count, type, indices);
        maybe_report_material_stream();
        return;
    }

    candidate_t candidate{};
    if (!build_candidate(mode, count, type, indices, &candidate)) {
        fallback_draw(mode, count, type, indices);
        maybe_report_material_stream();
        return;
    }

    const GLuint texture = g_material.bound_2d[static_cast<size_t>(program->diffuse_unit)];
    size_t bank = 0;
    GLint layer = 0;
    if (!texture || !ensure_texture_layer(program->diffuse_unit, texture, &bank, &layer) || !ensure_stream_objects()) {
        fallback_draw(mode, count, type, indices);
        maybe_report_material_stream();
        return;
    }

    stream_instance_t instance{};
    if (!build_stream_instance(candidate, *program, layer, &instance)) {
        fallback_draw(mode, count, type, indices);
        maybe_report_material_stream();
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
    maybe_report_material_stream();
}

void material_glUseProgram(GLuint program) {
    if (g_material.collector_active && !g_material.collector.empty()) {
        const auto next = g_material.programs.find(program);
        if (next == g_material.programs.end() || !next->second.compatible ||
            next->second.stream_program != g_material.collector_program)
            soft_segment_flush();
    }
    g_material_wrapped.use_program(program);
}

void material_glShaderSource(GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length) {
    g_material_wrapped.shader_source(shader, count, string, length);
    std::string source;
    if (collect_shader_source(count, string, length, &source))
        g_material.shaders[shader] = shader_source_t{std::move(source)};
    else
        g_material.shaders.erase(shader);
}

void material_glLinkProgram(GLuint program) {
    g_material_wrapped.link_program(program);
    g_material.programs.erase(program);
    classify_program(program);
}

void material_glDeleteProgram(GLuint program) {
    if (g_material.collector_active && g_state.program == program) soft_segment_flush();
    g_material.programs.erase(program);
    g_material_wrapped.delete_program(program);
}

void material_glActiveTexture(GLenum texture) {
    g_material_wrapped.active_texture(texture);
    if (texture >= GL_TEXTURE0 && ensure_limits() &&
        static_cast<GLint>(texture - GL_TEXTURE0) < g_material.max_texture_units)
        g_material.active_texture = texture;
}

void material_glBindTexture(GLenum target, GLuint texture) {
    g_material_wrapped.bind_texture(target, texture);
    if (target == GL_TEXTURE_2D && ensure_limits()) {
        const GLint unit = static_cast<GLint>(g_material.active_texture - GL_TEXTURE0);
        if (unit >= 0 && unit < g_material.max_texture_units)
            g_material.bound_2d[static_cast<size_t>(unit)] = texture;
    }
}

void material_glUniform1f(GLint location, GLfloat value) {
    material_program_t* program = current_material_program();
    if (program && program->compatible) {
        if (location == program->z_depth)
            program->z_depth_value = value;
        else if (location == program->chunk_depth)
            program->chunk_depth_value = value;
        else if (location >= 0)
            hard_flush();
    }
    g_material_wrapped.uniform1f(location, value);
}

void material_glUniform1i(GLint location, GLint value) {
    material_program_t* program = current_material_program();
    if (program && program->compatible) {
        if (location == program->diffuse)
            program->diffuse_unit = value;
        else if (location == program->use_texture)
            program->use_texture_value = value;
        else if (location >= 0)
            hard_flush();
    }
    g_material_wrapped.uniform1i(location, value);
}

void material_glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value) {
    material_program_t* program = current_material_program();
    if (program && program->compatible) {
        if (location == program->mvp && count == 1 && transpose == GL_FALSE && value != nullptr)
            std::copy(value, value + 16, program->mvp_value.begin());
        else if (location >= 0)
            hard_flush();
    }
    g_material_wrapped.uniform_matrix4fv(location, count, transpose, value);
}

GLuint current_bound_2d() {
    if (!ensure_limits()) return 0;
    const GLint unit = static_cast<GLint>(g_material.active_texture - GL_TEXTURE0);
    if (unit < 0 || unit >= g_material.max_texture_units) return 0;
    return g_material.bound_2d[static_cast<size_t>(unit)];
}

void material_glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                           GLint border, GLenum format, GLenum type, const void* pixels) {
    g_material_wrapped.tex_image_2d(target, level, internalformat, width, height, border, format, type, pixels);
    if (target == GL_TEXTURE_2D) invalidate_texture(current_bound_2d());
}

void material_glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height,
                              GLenum format, GLenum type, const void* pixels) {
    g_material_wrapped.tex_sub_image_2d(target, level, xoffset, yoffset, width, height, format, type, pixels);
    if (target == GL_TEXTURE_2D) invalidate_texture(current_bound_2d());
}

void material_glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height,
                                     GLint border, GLsizei imageSize, const void* data) {
    g_material_wrapped.compressed_tex_image_2d(target, level, internalformat, width, height, border, imageSize, data);
    if (target == GL_TEXTURE_2D) invalidate_texture(current_bound_2d());
}

void material_glCompressedTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
                                        GLsizei height, GLenum format, GLsizei imageSize, const void* data) {
    g_material_wrapped.compressed_tex_sub_image_2d(target, level, xoffset, yoffset, width, height, format, imageSize,
                                                    data);
    if (target == GL_TEXTURE_2D) invalidate_texture(current_bound_2d());
}

void material_glCopyTexImage2D(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width,
                               GLsizei height, GLint border) {
    g_material_wrapped.copy_tex_image_2d(target, level, internalformat, x, y, width, height, border);
    if (target == GL_TEXTURE_2D) invalidate_texture(current_bound_2d());
}

void material_glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y,
                                  GLsizei width, GLsizei height) {
    g_material_wrapped.copy_tex_sub_image_2d(target, level, xoffset, yoffset, x, y, width, height);
    if (target == GL_TEXTURE_2D) invalidate_texture(current_bound_2d());
}

void material_glTexStorage2D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height) {
    g_material_wrapped.tex_storage_2d(target, levels, internalformat, width, height);
    if (target == GL_TEXTURE_2D) invalidate_texture(current_bound_2d());
}

void material_glGenerateMipmap(GLenum target) {
    g_material_wrapped.generate_mipmap(target);
    if (target == GL_TEXTURE_2D) invalidate_texture(current_bound_2d());
}

void material_glTexParameteri(GLenum target, GLenum pname, GLint param) {
    g_material_wrapped.tex_parameter_i(target, pname, param);
    if (target == GL_TEXTURE_2D) invalidate_texture(current_bound_2d());
}

void material_glTexParameterf(GLenum target, GLenum pname, GLfloat param) {
    g_material_wrapped.tex_parameter_f(target, pname, param);
    if (target == GL_TEXTURE_2D) invalidate_texture(current_bound_2d());
}

void material_glTexParameteriv(GLenum target, GLenum pname, const GLint* params) {
    g_material_wrapped.tex_parameter_iv(target, pname, params);
    if (target == GL_TEXTURE_2D) invalidate_texture(current_bound_2d());
}

void material_glTexParameterfv(GLenum target, GLenum pname, const GLfloat* params) {
    g_material_wrapped.tex_parameter_fv(target, pname, params);
    if (target == GL_TEXTURE_2D) invalidate_texture(current_bound_2d());
}

void material_glDeleteTextures(GLsizei n, const GLuint* textures) {
    g_material_wrapped.delete_textures(n, textures);
    if (!textures || n <= 0) return;
    for (GLsizei i = 0; i < n; ++i) invalidate_texture(textures[i]);
}

void material_glCopyImageSubData(GLuint srcName, GLenum srcTarget, GLint srcLevel, GLint srcX, GLint srcY, GLint srcZ,
                                 GLuint dstName, GLenum dstTarget, GLint dstLevel, GLint dstX, GLint dstY, GLint dstZ,
                                 GLsizei srcWidth, GLsizei srcHeight, GLsizei srcDepth) {
    g_material_wrapped.copy_image_sub_data(srcName, srcTarget, srcLevel, srcX, srcY, srcZ, dstName, dstTarget,
                                           dstLevel, dstX, dstY, dstZ, srcWidth, srcHeight, srcDepth);
    if (dstTarget == GL_TEXTURE_2D) invalidate_texture(dstName);
}

void cleanup_material_context() {
    if (g_material.collector_active) hard_flush();
    for (const auto& variant : g_material.variants)
        if (variant.second) g_orig.delete_program(variant.second);
    for (const texture_bank_t& bank : g_material.banks)
        if (bank.texture) g_material_backend.delete_textures(1, &bank.texture);
    if (g_material.instance_buffer) g_orig.delete_buffers(1, &g_material.instance_buffer);
    if (g_material.stream_vao) g_orig.delete_vertex_arrays(1, &g_material.stream_vao);
    const material_stats_t saved = g_material.stats;
    g_material = {};
    g_material.stats = saved;
}

bool capture_material_backend() {
#define MG_CAPTURE(field, type, slot) g_material_backend.field = static_cast<type>(GLES.slot)
    MG_CAPTURE(create_shader, glCreateShader_PTR, glCreateShader);
    MG_CAPTURE(compile_shader, glCompileShader_PTR, glCompileShader);
    MG_CAPTURE(get_shader_iv, glGetShaderiv_PTR, glGetShaderiv);
    MG_CAPTURE(get_shader_info_log, glGetShaderInfoLog_PTR, glGetShaderInfoLog);
    MG_CAPTURE(delete_shader, glDeleteShader_PTR, glDeleteShader);
    MG_CAPTURE(create_program, glCreateProgram_PTR, glCreateProgram);
    MG_CAPTURE(get_program_iv, glGetProgramiv_PTR, glGetProgramiv);
    MG_CAPTURE(get_program_info_log, glGetProgramInfoLog_PTR, glGetProgramInfoLog);
    MG_CAPTURE(get_uniform_location, glGetUniformLocation_PTR, glGetUniformLocation);
    MG_CAPTURE(uniform1i, glUniform1i_PTR, glUniform1i);
    MG_CAPTURE(draw_arrays_instanced, glDrawArraysInstanced_PTR, glDrawArraysInstanced);
    MG_CAPTURE(bind_buffer_base, glBindBufferBase_PTR, glBindBufferBase);
    MG_CAPTURE(bind_buffer_range, glBindBufferRange_PTR, glBindBufferRange);
    MG_CAPTURE(get_integer_i_v, glGetIntegeri_v_PTR, glGetIntegeri_v);
    MG_CAPTURE(get_integer64_i_v, glGetInteger64i_v_PTR, glGetInteger64i_v);
    MG_CAPTURE(get_integerv, glGetIntegerv_PTR, glGetIntegerv);
    MG_CAPTURE(get_tex_level_parameter_iv, glGetTexLevelParameteriv_PTR, glGetTexLevelParameteriv);
    MG_CAPTURE(get_tex_parameter_iv, glGetTexParameteriv_PTR, glGetTexParameteriv);
    MG_CAPTURE(gen_textures, glGenTextures_PTR, glGenTextures);
    MG_CAPTURE(delete_textures, glDeleteTextures_PTR, glDeleteTextures);
    MG_CAPTURE(active_texture, glActiveTexture_PTR, glActiveTexture);
    MG_CAPTURE(bind_texture, glBindTexture_PTR, glBindTexture);
    MG_CAPTURE(tex_storage_3d, glTexStorage3D_PTR, glTexStorage3D);
    MG_CAPTURE(tex_parameter_i, glTexParameteri_PTR, glTexParameteri);
    MG_CAPTURE(copy_image_sub_data, glCopyImageSubData_PTR, glCopyImageSubData);
#undef MG_CAPTURE
    return g_material_backend.create_shader && g_material_backend.compile_shader && g_material_backend.get_shader_iv &&
           g_material_backend.delete_shader && g_material_backend.create_program && g_material_backend.get_program_iv &&
           g_material_backend.get_uniform_location && g_material_backend.uniform1i &&
           g_material_backend.draw_arrays_instanced && g_material_backend.bind_buffer_base &&
           g_material_backend.bind_buffer_range && g_material_backend.get_integer_i_v &&
           g_material_backend.get_integer64_i_v && g_material_backend.get_integerv &&
           g_material_backend.get_tex_level_parameter_iv && g_material_backend.get_tex_parameter_iv &&
           g_material_backend.gen_textures && g_material_backend.delete_textures && g_material_backend.active_texture &&
           g_material_backend.bind_texture && g_material_backend.tex_storage_3d && g_material_backend.tex_parameter_i &&
           g_material_backend.copy_image_sub_data;
}

} // namespace

namespace mg_ts {
void pz_repack_before_backend_command(backend_command_class classification) {
    if (!g_material_enabled) return;
    if (classification == backend_command_class::context_adopt) {
        pz_repack_before_backend_command_v2_impl(classification);
        g_material.active_texture = GL_TEXTURE0;
        return;
    }
    if (classification == backend_command_class::context_release) {
        cleanup_material_context();
        pz_repack_before_backend_command_v2_impl(classification);
        return;
    }
    g_state.worker_context = true;
    if (classification == backend_command_class::barrier) hard_flush();
}
} // namespace mg_ts

void mg_pz_repack_renderer_install(void) {
    mg_pz_repack_renderer_install_v2_impl();
    if (!g_enabled) return;
    if (!capture_material_backend()) {
        LOG_W_FORCE("ZOMDROID_PZ_MATERIAL_STREAM disabled reason=backend_capability_missing")
        return;
    }

    g_material_wrapped.use_program = static_cast<glUseProgram_PTR>(GLES.glUseProgram);
    g_material_wrapped.shader_source = static_cast<glShaderSource_PTR>(GLES.glShaderSource);
    g_material_wrapped.link_program = static_cast<glLinkProgram_PTR>(GLES.glLinkProgram);
    g_material_wrapped.delete_program = static_cast<glDeleteProgram_PTR>(GLES.glDeleteProgram);
    g_material_wrapped.active_texture = static_cast<glActiveTexture_PTR>(GLES.glActiveTexture);
    g_material_wrapped.bind_texture = static_cast<glBindTexture_PTR>(GLES.glBindTexture);
    g_material_wrapped.uniform1f = static_cast<glUniform1f_PTR>(GLES.glUniform1f);
    g_material_wrapped.uniform1i = static_cast<glUniform1i_PTR>(GLES.glUniform1i);
    g_material_wrapped.uniform_matrix4fv = static_cast<glUniformMatrix4fv_PTR>(GLES.glUniformMatrix4fv);
    g_material_wrapped.tex_image_2d = static_cast<glTexImage2D_PTR>(GLES.glTexImage2D);
    g_material_wrapped.tex_sub_image_2d = static_cast<glTexSubImage2D_PTR>(GLES.glTexSubImage2D);
    g_material_wrapped.compressed_tex_image_2d = static_cast<glCompressedTexImage2D_PTR>(GLES.glCompressedTexImage2D);
    g_material_wrapped.compressed_tex_sub_image_2d =
        static_cast<glCompressedTexSubImage2D_PTR>(GLES.glCompressedTexSubImage2D);
    g_material_wrapped.copy_tex_image_2d = static_cast<glCopyTexImage2D_PTR>(GLES.glCopyTexImage2D);
    g_material_wrapped.copy_tex_sub_image_2d = static_cast<glCopyTexSubImage2D_PTR>(GLES.glCopyTexSubImage2D);
    g_material_wrapped.tex_storage_2d = static_cast<glTexStorage2D_PTR>(GLES.glTexStorage2D);
    g_material_wrapped.generate_mipmap = static_cast<glGenerateMipmap_PTR>(GLES.glGenerateMipmap);
    g_material_wrapped.tex_parameter_i = static_cast<glTexParameteri_PTR>(GLES.glTexParameteri);
    g_material_wrapped.tex_parameter_f = static_cast<glTexParameterf_PTR>(GLES.glTexParameterf);
    g_material_wrapped.tex_parameter_iv = static_cast<glTexParameteriv_PTR>(GLES.glTexParameteriv);
    g_material_wrapped.tex_parameter_fv = static_cast<glTexParameterfv_PTR>(GLES.glTexParameterfv);
    g_material_wrapped.delete_textures = static_cast<glDeleteTextures_PTR>(GLES.glDeleteTextures);
    g_material_wrapped.copy_image_sub_data = static_cast<glCopyImageSubData_PTR>(GLES.glCopyImageSubData);

    if (!g_material_wrapped.use_program || !g_material_wrapped.shader_source || !g_material_wrapped.link_program ||
        !g_material_wrapped.delete_program || !g_material_wrapped.active_texture || !g_material_wrapped.bind_texture ||
        !g_material_wrapped.uniform1f || !g_material_wrapped.uniform1i || !g_material_wrapped.uniform_matrix4fv) {
        LOG_W_FORCE("ZOMDROID_PZ_MATERIAL_STREAM disabled reason=wrapper_capture_missing")
        return;
    }

    GLES.glDrawElements = material_glDrawElements;
    GLES.glUseProgram = material_glUseProgram;
    GLES.glShaderSource = material_glShaderSource;
    GLES.glLinkProgram = material_glLinkProgram;
    GLES.glDeleteProgram = material_glDeleteProgram;
    GLES.glActiveTexture = material_glActiveTexture;
    GLES.glBindTexture = material_glBindTexture;
    GLES.glUniform1f = material_glUniform1f;
    GLES.glUniform1i = material_glUniform1i;
    GLES.glUniformMatrix4fv = material_glUniformMatrix4fv;

    if (g_material_wrapped.tex_image_2d) GLES.glTexImage2D = material_glTexImage2D;
    if (g_material_wrapped.tex_sub_image_2d) GLES.glTexSubImage2D = material_glTexSubImage2D;
    if (g_material_wrapped.compressed_tex_image_2d) GLES.glCompressedTexImage2D = material_glCompressedTexImage2D;
    if (g_material_wrapped.compressed_tex_sub_image_2d)
        GLES.glCompressedTexSubImage2D = material_glCompressedTexSubImage2D;
    if (g_material_wrapped.copy_tex_image_2d) GLES.glCopyTexImage2D = material_glCopyTexImage2D;
    if (g_material_wrapped.copy_tex_sub_image_2d) GLES.glCopyTexSubImage2D = material_glCopyTexSubImage2D;
    if (g_material_wrapped.tex_storage_2d) GLES.glTexStorage2D = material_glTexStorage2D;
    if (g_material_wrapped.generate_mipmap) GLES.glGenerateMipmap = material_glGenerateMipmap;
    if (g_material_wrapped.tex_parameter_i) GLES.glTexParameteri = material_glTexParameteri;
    if (g_material_wrapped.tex_parameter_f) GLES.glTexParameterf = material_glTexParameterf;
    if (g_material_wrapped.tex_parameter_iv) GLES.glTexParameteriv = material_glTexParameteriv;
    if (g_material_wrapped.tex_parameter_fv) GLES.glTexParameterfv = material_glTexParameterfv;
    if (g_material_wrapped.delete_textures) GLES.glDeleteTextures = material_glDeleteTextures;
    if (g_material_wrapped.copy_image_sub_data) GLES.glCopyImageSubData = material_glCopyImageSubData;

    g_material_enabled = true;
    LOG_I("ZOMDROID_PZ_MATERIAL_STREAM enabled=1 revision=4 collector=512 instance=ssbo texture=sampler2DArray "
          "family=tile_sprite_no_depth barriers=fbo+resource+fixed_state handoff_restore=removed")
}

#else

void mg_pz_repack_renderer_install(void) {
    mg_pz_repack_renderer_install_v2_impl();
}

#endif
