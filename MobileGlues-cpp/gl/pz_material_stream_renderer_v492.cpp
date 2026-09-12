// MobileGlues - gl/pz_material_stream_renderer_v492.cpp
// Project Zomboid V4.9.2 diagnostic overlay. V4.9.1 remains the renderer;
// this layer adds exact vertex-contract telemetry for the tile-depth family.
// It does not relax ordering, barriers, or shader eligibility.

#include "pz_material_stream_renderer_v491.cpp"

#if defined(ZOMDROID_EXPERIMENTAL)

namespace v44_base {
namespace v43_base {
namespace legacy_v41 {
namespace v41_base {
namespace {

struct v492_wrapped_t {
    glDrawElements_PTR draw_elements = nullptr;
    glLinkProgram_PTR link_program = nullptr;
    glDeleteProgram_PTR delete_program = nullptr;
};

thread_local std::unordered_map<GLuint, bool> g_v492_vs_reported;
v492_wrapped_t g_v492_wrapped;
bool g_v492_enabled = false;

void v492_probe_depth_vertex(GLuint program_id) {
    if (!g_v492_enabled || program_id == 0 || g_v492_vs_reported.find(program_id) != g_v492_vs_reported.end()) return;
    g_v492_vs_reported.emplace(program_id, true);

    v41_program_record_t source{};
    if (!v41_registry_program(program_id, &source) || !source.complete) return;
    const bool depth_candidate = source.vertex.find("zDepthBlendToZ") != std::string::npos &&
                                 source.fragment.find("DEPTH") != std::string::npos;
    if (!depth_candidate) return;

    static const std::regex pos_decl(
        R"(layout\s*\(\s*location\s*=\s*0\s*\)\s*in\s+(?:(?:lowp|mediump|highp)\s+)?vec2\s+vPos\s*;)");
    static const std::regex uv_decl(
        R"(layout\s*\(\s*location\s*=\s*1\s*\)\s*in\s+(?:(?:lowp|mediump|highp)\s+)?vec2\s+vUV\s*;)");
    static const std::regex color_decl(
        R"(layout\s*\(\s*location\s*=\s*2\s*\)\s*in\s+(?:(?:lowp|mediump|highp)\s+)?vec4\s+vCol\s*;)");
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

    const bool uniforms = only_uniforms(
        source.vertex, {"ModelViewProjection", "zDepth", "zDepthBlendZ", "zDepthBlendToZ"});
    const size_t inputs = regex_count(source.vertex, any_input);
    const size_t pos = regex_count(source.vertex, pos_decl);
    const size_t uv = regex_count(source.vertex, uv_decl);
    const size_t color = regex_count(source.vertex, color_decl);
    const size_t mvp = regex_count(source.vertex, mvp_decl);
    const size_t z = regex_count(source.vertex, z_decl);
    const size_t blend_z = regex_count(source.vertex, blend_z_decl);
    const size_t blend_to = regex_count(source.vertex, blend_to_decl);
    const size_t main_count = regex_count(source.vertex, main_decl);

    const char* reason = "rewrite_or_unknown";
    if (!uniforms) reason = "uniforms";
    else if (inputs != 3) reason = "inputs";
    else if (pos != 1) reason = "pos";
    else if (uv != 1) reason = "uv";
    else if (color != 1) reason = "color";
    else if (mvp != 1) reason = "mvp";
    else if (z != 1) reason = "z";
    else if (blend_z != 1) reason = "blend_z";
    else if (blend_to != 1) reason = "blend_to";
    else if (main_count != 1) reason = "main";

    LOG_I("ZOMDROID_PZ_MATERIAL_STREAM_V492_VS program=%u reason=%s uniforms=%d inputs=%zu pos=%zu uv=%zu "
          "color=%zu mvp=%zu z=%zu blend_z=%zu blend_to=%zu main=%zu",
          program_id, reason, uniforms ? 1 : 0, inputs, pos, uv, color, mvp, z, blend_z, blend_to, main_count)
}

void v492_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    v492_probe_depth_vertex(g_state.program);
    g_v492_wrapped.draw_elements(mode, count, type, indices);
}

void v492_glLinkProgram(GLuint program) {
    g_v492_wrapped.link_program(program);
    g_v492_vs_reported.erase(program);
}

void v492_glDeleteProgram(GLuint program) {
    g_v492_vs_reported.erase(program);
    g_v492_wrapped.delete_program(program);
}

void v492_install_impl() {
    ::mg_pz_material_stream_v491_install();
    if (!g_v491_enabled || !g_v49_enabled || !g_v41_enabled || !g_material_enabled || !g_enabled || !g_v44_enabled)
        return;

    g_v492_wrapped.draw_elements = static_cast<glDrawElements_PTR>(GLES.glDrawElements);
    g_v492_wrapped.link_program = static_cast<glLinkProgram_PTR>(GLES.glLinkProgram);
    g_v492_wrapped.delete_program = static_cast<glDeleteProgram_PTR>(GLES.glDeleteProgram);
    if (!g_v492_wrapped.draw_elements || !g_v492_wrapped.link_program || !g_v492_wrapped.delete_program) {
        LOG_W_FORCE("ZOMDROID_PZ_MATERIAL_STREAM_V492 disabled reason=wrapper_capture_missing")
        return;
    }

    g_v492_enabled = true;
    GLES.glDrawElements = v492_glDrawElements;
    GLES.glLinkProgram = v492_glLinkProgram;
    GLES.glDeleteProgram = v492_glDeleteProgram;

    LOG_I("ZOMDROID_PZ_MATERIAL_STREAM_V492 enabled=1 revision=4.9.2 mode=perf_ceiling base=v491 "
          "texture_guard=uint32_overflow_band probe=depth_vs_contract depth_behavior=unchanged "
          "ordering=unchanged stable_untouched=1")
}

} // namespace

void install_v492_internal() { v492_install_impl(); }

} // namespace v41_base
} // namespace legacy_v41
} // namespace v43_base
} // namespace v44_base

void mg_pz_material_stream_v492_install(void) {
    v44_base::v43_base::legacy_v41::v41_base::install_v492_internal();
}

#else

void mg_pz_material_stream_v492_install(void) {}

#endif
