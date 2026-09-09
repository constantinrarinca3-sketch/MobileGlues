// Host-side regression tests for source-only GLSL compatibility rewrites.

#include "../gl/glsl/shader_compat.h"

#include <cassert>
#include <iostream>
#include <string>

int main() {
    {
        std::string source =
            "uniform sampler2D texture;\n"
            "void main() { vec4 c = texture2D(texture, vec2(0.0)); }\n";
        const auto result = mg_glsl_compat::rewrite_legacy_texture2d_calls(source);
        assert(result.calls_rewritten);
        assert(result.sampler_identifier_renamed);
        assert(source.find("uniform sampler2D zomdroid_texture_sampler;") != std::string::npos);
        assert(source.find("texture(zomdroid_texture_sampler, vec2(0.0))") != std::string::npos);
    }

    {
        std::string source =
            "vec4 outline(sampler2D texture, vec2 uv) { return texture2D(texture, uv); }\n";
        const auto result = mg_glsl_compat::rewrite_legacy_texture2d_calls(source);
        assert(result.calls_rewritten);
        assert(result.sampler_identifier_renamed);
        assert(source.find("sampler2D zomdroid_texture_sampler") != std::string::npos);
        assert(source.find("texture(zomdroid_texture_sampler, uv)") != std::string::npos);
    }

    {
        std::string source =
            "uniform sampler2D DIFFUSE;\n"
            "void main() { vec4 c = texture2D(DIFFUSE, vec2(0.0)); }\n";
        const auto result = mg_glsl_compat::rewrite_legacy_texture2d_calls(source);
        assert(result.calls_rewritten);
        assert(!result.sampler_identifier_renamed);
        assert(source.find("texture(DIFFUSE, vec2(0.0))") != std::string::npos);
    }

    {
        std::string source =
            "uniform sampler2D texture;\n"
            "uniform sampler2D DIFFUSE;\n"
            "void main() { vec4 a = texture(DIFFUSE, vec2(0.0)); vec4 b = texture2D(texture, vec2(0.0)); }\n";
        const auto result = mg_glsl_compat::rewrite_legacy_texture2d_calls(source);
        assert(result.calls_rewritten);
        assert(result.sampler_identifier_renamed);
        assert(source.find("texture(DIFFUSE, vec2(0.0))") != std::string::npos);
        assert(source.find("texture(zomdroid_texture_sampler, vec2(0.0))") != std::string::npos);
    }

    {
        std::string storage;
        assert(std::string(mg_glsl_compat::remap_texture_sampler_uniform_name("texture", storage)) ==
               "zomdroid_texture_sampler");
        assert(std::string(mg_glsl_compat::remap_texture_sampler_uniform_name("texture[0]", storage)) ==
               "zomdroid_texture_sampler[0]");
        const char* unchanged = mg_glsl_compat::remap_texture_sampler_uniform_name("textureScale", storage);
        assert(std::string(unchanged) == "textureScale");
    }

    {
        const std::string source =
            "uniform int useTexture = 1;\n"
            "uniform vec2 UVScale = vec2(1, 1);\n"
            "uniform float maskPaddingRadius = 1.0 / 64.0;\n"
            "uniform vec3 AmbientColor = vec3(0.4);\n"
            "uniform float symbolic = SOME_VALUE;\n"
            "// uniform float commentedLine = 1.0;\n"
            "/* uniform int commentedBlock = 1; */\n";
        const auto defaults = mg_glsl_compat::collect_uniform_defaults(source);
        assert(defaults.size() == 4);
        assert(defaults[0].name == "useTexture" && defaults[0].integer && defaults[0].components == 1 &&
               defaults[0].values[0] == 1.0);
        assert(defaults[1].name == "UVScale" && !defaults[1].integer && defaults[1].components == 2 &&
               defaults[1].values[0] == 1.0 && defaults[1].values[1] == 1.0);
        assert(defaults[2].name == "maskPaddingRadius" && defaults[2].components == 1 &&
               defaults[2].values[0] == 1.0 / 64.0);
        assert(defaults[3].name == "AmbientColor" && defaults[3].components == 3 &&
               defaults[3].values[0] == 0.4 && defaults[3].values[2] == 0.4);
    }

    std::cout << "shader compatibility tests passed\n";
    return 0;
}
