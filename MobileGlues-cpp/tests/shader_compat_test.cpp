// Host-side regression tests for source-only GLSL compatibility rewrites.

#include "../gl/glsl/shader_compat.h"

#include <array>
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

using mg_glsl_compat::pz_alpha_shader_kind;

std::string read_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    assert(input && "optional PZ shader fixture could not be opened");
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

void expect_alpha_rewrite(std::string source, pz_alpha_shader_kind expected_kind, bool producer) {
    const auto result = mg_glsl_compat::rewrite_pz_alpha_test_family(source);
    assert(result.candidate);
    assert(result.contract_matched);
    assert(result.rewritten);
    assert(result.kind == expected_kind);
    assert(source.find("uniform int zomdroidAlphaEnabled;") != std::string::npos);
    assert(source.find("uniform int zomdroidAlphaFunc;") != std::string::npos);
    assert(source.find("uniform float zomdroidAlphaRef;") != std::string::npos);

    const size_t discard = source.find("!zomdroidAlphaPass(");
    assert(discard != std::string::npos);
    if (producer) {
        const size_t depth = source.find("gl_FragDepth = calcDepthZ;", discard);
        assert(depth != std::string::npos);
        assert(discard < depth);
    } else {
        assert(source.find("zomdroidAlphaFinalColor = c * col") != std::string::npos);
    }

    const std::string once = source;
    const auto second = mg_glsl_compat::rewrite_pz_alpha_test_family(source);
    assert(second.candidate);
    assert(!second.rewritten);
    assert(source == once);
}

} // namespace

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
            "struct Material { vec4 texture; };\n"
            "uniform sampler2D texture;\n"
            "void main() { Material obj; vec4 a = obj . texture; "
            "vec4 b = texture2D(texture, vec2(0.0)); }\n"
            "// texture must stay unchanged in comments\n";
        const auto result = mg_glsl_compat::rewrite_legacy_texture2d_calls(source);
        assert(result.calls_rewritten && result.sampler_identifier_renamed);
        assert(source.find("vec4 texture;") != std::string::npos);
        assert(source.find("obj . texture") != std::string::npos);
        assert(source.find("texture(zomdroid_texture_sampler, vec2(0.0))") != std::string::npos);
        assert(source.find("// texture must stay unchanged") != std::string::npos);
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

    // Structurally faithful, reduced forms of the four B42.20.x fragment
    // shaders that bypass desktop GL_ALPHA_TEST. These fixtures test the
    // contract without copying the game's complete shader sources here.
    expect_alpha_rewrite(
        "uniform sampler2D DIFFUSE; uniform sampler2D DEPTH;\n"
        "uniform int useTexture = 1; uniform float chunkDepth = 0.0; varying vec4 col;\n"
        "void main() { vec4 c = vec4(1); float depthTexel = texture2D(DEPTH, vec2(0)).r;\n"
        "gl_FragDepth = chunkDepth + depthTexel; gl_FragColor = c * col; }\n",
        pz_alpha_shader_kind::chunk_composite, false);

    expect_alpha_rewrite(
        "uniform sampler2D DIFFUSE; uniform sampler2D DEPTH; varying vec4 col;\n"
        "uniform float zDepthBlendZ = 0; uniform float zDepthBlendToZ = 0;\n"
        "void main() { vec4 c = texture2D(DIFFUSE, vec2(0)); float d = texture2D(DEPTH, vec2(0)).r;\n"
        "c *= col; c.rgb *= col.a; if (d > 0) { float calcDepthZ = zDepthBlendZ + d * zDepthBlendToZ;\n"
        "gl_FragDepth = calcDepthZ; gl_FragColor = c; } else { discard; } }\n",
        pz_alpha_shader_kind::tile_with_depth, true);

    expect_alpha_rewrite(
        "uniform sampler2D DIFFUSE; uniform sampler2D DEPTH; varying vec4 col;\n"
        "uniform float zDepthBlendZ = 0; uniform float zDepthBlendToZ = 0;\n"
        "void main() { vec4 c0 = texture2D(DIFFUSE, vec2(0)); float d = texture2D(DEPTH, vec2(0)).r;\n"
        "vec4 c = c0 * col; c.rgb *= col.a; if (c0.a > 0.8 && d > 0.0) {\n"
        "float calcDepthZ = zDepthBlendZ; gl_FragDepth = calcDepthZ; gl_FragColor = c; } else { discard; } }\n",
        pz_alpha_shader_kind::opaque_with_depth, true);

    expect_alpha_rewrite(
        "uniform sampler2D DIFFUSE; uniform sampler2D DEPTH; uniform sampler2D MASK; varying vec4 col;\n"
        "uniform float zDepthBlendZ = 0; uniform float zDepthBlendToZ = 0;\n"
        "void main() { vec4 c = texture2D(DIFFUSE, vec2(0)); float d = texture2D(DEPTH, vec2(0)).r;\n"
        "vec4 m = texture2D(MASK, vec2(0)); c *= col; c.rgb *= col.a; if (d * m.a > 0) {\n"
        "float calcDepthZ = zDepthBlendZ; gl_FragDepth = calcDepthZ; gl_FragColor = c; } else { discard; } }\n",
        pz_alpha_shader_kind::seam_fix_2, true);

    {
        std::string near_miss =
            "uniform sampler2D DIFFUSE; uniform sampler2D DEPTH; varying vec4 col;\n"
            "uniform float zDepthBlendZ = 0; uniform float zDepthBlendToZ = 0;\n"
            "void main() { vec4 c = texture2D(DIFFUSE, vec2(0)); float d = texture2D(DEPTH, vec2(0)).r;\n"
            "c *= col; c.rgb *= col.a; if (d >= 0) { float calcDepthZ = zDepthBlendZ;\n"
            "gl_FragDepth = calcDepthZ; gl_FragColor = c; } }\n";
        const std::string original = near_miss;
        const auto result = mg_glsl_compat::rewrite_pz_alpha_test_family(near_miss);
        assert(result.candidate);
        assert(!result.contract_matched);
        assert(!result.rewritten);
        assert(near_miss == original);
    }

    {
        std::string source =
            "#version 330\n"
            "layout (location = 0) in vec2 vPos;\n"
            "layout (location = 1) in vec2 vUV;\n"
            "layout (location = 2) in vec4 vCol;\n"
            "uniform mat4 ModelViewProjection;\n"
            "uniform float chunkDepth = 0.0;\n"
            "uniform float zDepth = 0.0;\n"
            "void main() { vec4 o = ModelViewProjection * vec4(vPos, 0, 1); "
            "o.z = chunkDepth + zDepth; gl_Position = o; }\n";
        const auto result = mg_glsl_compat::rewrite_pz_default_tile_batch(source);
        assert(result.candidate && result.contract_matched && result.rewritten);
        assert(source.find("uniform int zomdroidBatchRunCount;") != std::string::npos);
        assert(source.find("uniform int zomdroidBatchRunStart[8];") != std::string::npos);
        assert(source.find("uniform vec2 zomdroidBatchDepth[8];") != std::string::npos);
        assert(source.find("gl_VertexID >= zomdroidBatchRunStart[zomdroidRun]") != std::string::npos);
        assert(source.find("o.z = chunkDepth + zDepth;") == std::string::npos);
        const std::string once = source;
        const auto second = mg_glsl_compat::rewrite_pz_default_tile_batch(source);
        assert(!second.rewritten && source == once);
    }

    {
        std::string near_miss =
            "layout (location = 0) in vec2 vPos; uniform float chunkDepth; uniform float zDepth; "
            "void main() { gl_Position = vec4(vPos, chunkDepth + zDepth, 1); }";
        const std::string original = near_miss;
        const auto result = mg_glsl_compat::rewrite_pz_default_tile_batch(near_miss);
        assert(result.candidate && !result.contract_matched && !result.rewritten);
        assert(near_miss == original);
    }

    // Local validation can point at the legally installed game shaders. CI
    // intentionally has no such dependency and skips this block.
    if (const char* shader_dir = std::getenv("PZ_SHADER_DIR")) {
        const std::array<std::pair<const char*, pz_alpha_shader_kind>, 4> shaders{{
            {"chunkShader.frag", pz_alpha_shader_kind::chunk_composite},
            {"tileWithDepth.frag", pz_alpha_shader_kind::tile_with_depth},
            {"opaqueWithDepth.frag", pz_alpha_shader_kind::opaque_with_depth},
            {"seamFix2.frag", pz_alpha_shader_kind::seam_fix_2},
        }};
        for (const auto& shader : shaders) {
            expect_alpha_rewrite(read_file(std::string(shader_dir) + "/" + shader.first), shader.second,
                                 shader.second != pz_alpha_shader_kind::chunk_composite);
        }
    }

    std::cout << "shader compatibility tests passed\n";
    return 0;
}
