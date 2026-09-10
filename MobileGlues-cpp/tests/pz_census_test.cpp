// Host-side contract check for the opt-in Project Zomboid renderer census.
#include "gl/pz_census.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static std::string last_file_log;
static int failures = 0;

extern "C" void write_log(const char* format, ...) {
    char line[4096] = {};
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    last_file_log = line;
}

extern "C" void write_log_n(const char*, ...) {}

int __android_log_print(int, const char*, const char*, ...) {
    return 0;
}

static void expect(bool condition, const char* message) {
    if (condition) return;
    std::printf("FAIL %s\n", message);
    ++failures;
}

int main() {
    setenv("MOBILEGLUES_PZ_VAO_FASTPATH", "0", 1);
    setenv("MOBILEGLUES_PZ_ATTRIB_FASTPATH", "0", 1);
    setenv("MOBILEGLUES_PZ_UNIFORM_FASTPATH", "0", 1);
    setenv("MOBILEGLUES_PZ_BUFFER_STREAMING", "0", 1);
    setenv("MOBILEGLUES_PZ_BUFFER_DISCARD_COALESCE", "0", 1);
    setenv("MOBILEGLUES_PZ_STATE_SHADOW", "0", 1);
    setenv("MOBILEGLUES_PZ_RUNTIME_MIPMAP_SKIP", "0", 1);
    setenv("MOBILEGLUES_PZ_QUAD_INDEX_CACHE", "0", 1);
    setenv("MOBILEGLUES_PZ_CENSUS", "0", 1);
    mg_pz_census_init();
    expect(!mg_pz_census_active, "0 must disable the census");
    expect(!mg_pz_vao_fastpath_active, "0 must disable the VAO fast path");
    expect(!mg_pz_attrib_fastpath_active, "0 must disable the attribute fast path");
    expect(!mg_pz_uniform_fastpath_active, "0 must disable the uniform fast path");
    expect(!mg_pz_buffer_streaming_active, "0 must disable buffer streaming");
    expect(!mg_pz_buffer_discard_coalesce_active, "0 must disable buffer discard coalescing");
    expect(!mg_pz_state_shadow_active, "0 must disable the fixed-state shadow");
    expect(!mg_pz_runtime_mipmap_skip_active, "0 must disable runtime mipmap skipping");
    expect(!mg_pz_quad_index_cache_active, "0 must disable the quad index cache");

    setenv("MOBILEGLUES_PZ_CENSUS", "true", 1);
    mg_pz_census_init();
    expect(!mg_pz_census_active, "only the exact value 1 may enable the census");

    setenv("MOBILEGLUES_PZ_VAO_FASTPATH", "true", 1);
    setenv("MOBILEGLUES_PZ_ATTRIB_FASTPATH", "true", 1);
    setenv("MOBILEGLUES_PZ_UNIFORM_FASTPATH", "true", 1);
    setenv("MOBILEGLUES_PZ_BUFFER_STREAMING", "true", 1);
    setenv("MOBILEGLUES_PZ_BUFFER_DISCARD_COALESCE", "true", 1);
    setenv("MOBILEGLUES_PZ_STATE_SHADOW", "true", 1);
    setenv("MOBILEGLUES_PZ_RUNTIME_MIPMAP_SKIP", "true", 1);
    setenv("MOBILEGLUES_PZ_QUAD_INDEX_CACHE", "true", 1);
    mg_pz_census_init();
    expect(!mg_pz_vao_fastpath_active, "only the exact value 1 may enable the VAO fast path");
    expect(!mg_pz_attrib_fastpath_active, "only the exact value 1 may enable the attribute fast path");
    expect(!mg_pz_uniform_fastpath_active, "only the exact value 1 may enable the uniform fast path");
    expect(!mg_pz_buffer_streaming_active, "only the exact value 1 may enable buffer streaming");
    expect(!mg_pz_buffer_discard_coalesce_active,
           "only the exact value 1 may enable buffer discard coalescing");
    expect(!mg_pz_state_shadow_active, "only the exact value 1 may enable the fixed-state shadow");
    expect(!mg_pz_runtime_mipmap_skip_active, "only the exact value 1 may enable runtime mipmap skipping");
    expect(!mg_pz_quad_index_cache_active, "only the exact value 1 may enable the quad index cache");

    setenv("MOBILEGLUES_PZ_VAO_FASTPATH", "1", 1);
    setenv("MOBILEGLUES_PZ_ATTRIB_FASTPATH", "1", 1);
    setenv("MOBILEGLUES_PZ_UNIFORM_FASTPATH", "1", 1);
    setenv("MOBILEGLUES_PZ_BUFFER_STREAMING", "1", 1);
    setenv("MOBILEGLUES_PZ_BUFFER_DISCARD_COALESCE", "1", 1);
    setenv("MOBILEGLUES_PZ_STATE_SHADOW", "1", 1);
    setenv("MOBILEGLUES_PZ_RUNTIME_MIPMAP_SKIP", "1", 1);
    setenv("MOBILEGLUES_PZ_QUAD_INDEX_CACHE", "1", 1);
    setenv("MOBILEGLUES_PZ_CENSUS", "1", 1);
    mg_pz_census_init();
    expect(mg_pz_census_active, "1 must enable the census");
    expect(mg_pz_vao_fastpath_active, "1 must enable the VAO fast path");
    expect(mg_pz_attrib_fastpath_active, "1 must enable the attribute fast path");
    expect(mg_pz_uniform_fastpath_active, "1 must enable the uniform fast path");
    expect(mg_pz_buffer_streaming_active, "1 must enable buffer streaming");
    expect(mg_pz_buffer_discard_coalesce_active, "1 must enable buffer discard coalescing");
    expect(mg_pz_state_shadow_active, "1 must enable the fixed-state shadow");
    expect(mg_pz_runtime_mipmap_skip_active, "1 must enable runtime mipmap skipping");
    expect(mg_pz_quad_index_cache_active, "1 must enable the quad index cache");

    const GLfloat uniform_value[4] = {1.0f, 2.0f, 3.0f, 4.0f};

    for (int frame = 0; frame < 300; ++frame) {
        mg_pz_census_draw(false, GL_TRIANGLES, 6, 1);
        mg_pz_census_use_program(frame % 3 == 0);
        mg_pz_census_gl_call("glUniform4fv");
        const bool uniform_skipped = mg_pz_uniform_call(7, 3, 0x304U, 1, uniform_value, sizeof(uniform_value));
        expect(uniform_skipped == (frame != 0), "only repeated uniform payloads may be skipped");
        mg_pz_census_bind_vao(true, true, true);
        mg_pz_census_gl_call("glEnableVertexAttribArray");
        mg_pz_census_attrib(mg_pz_attrib_kind::enable, true, frame != 0, frame != 0);
        mg_pz_census_buffer_data(128, false);
        mg_pz_census_texture_upload(false, GL_RGBA, true, false, false, false, 256, 0);
        mg_pz_census_texture_upload(true, GL_BGRA, true, true, true, false, 512, 512);
        mg_pz_census_batch_draw(7, GL_TRIANGLES, GL_UNSIGNED_SHORT, 6, 3);
        mg_pz_census_batch_draw(7, GL_TRIANGLES, GL_UNSIGNED_SHORT, 12, 3);
        mg_pz_census_bind_texture(false);
        mg_pz_census_batch_draw(7, GL_TRIANGLES, GL_UNSIGNED_SHORT, 18, 3);
        mg_pz_census_present(true);
    }

    expect(last_file_log.find("frames=300") != std::string::npos, "report interval must be 300 frames");
    expect(last_file_log.find("draw_a=300") != std::string::npos, "array draws must be aggregated");
    expect(last_file_log.find("items=1800") != std::string::npos, "draw item count must be aggregated");
    expect(last_file_log.find("program=300/100") != std::string::npos, "redundant program calls must be split");
    expect(last_file_log.find("schema=5") != std::string::npos, "schema 5 must be reported");
    expect(last_file_log.find("vao=300/300/300/300") != std::string::npos,
           "VAO frontend, confirmed and skipped counts must be split");
    expect(last_file_log.find("uniform=300/300/299/299") != std::string::npos,
           "uniform tracked, exact and skipped counts must be split");
    expect(last_file_log.find("attrib=300/300/299/299") != std::string::npos,
           "attribute tracked, exact and skipped counts must be split");
    expect(last_file_log.find("upload=300+0/38400B") != std::string::npos, "buffer bytes must be aggregated");
    expect(last_file_log.find("tex_upload=300+300/600/230400B/512B") != std::string::npos,
           "texture calls, bytes and largest upload must be aggregated");
    expect(last_file_log.find("tex_src=300/300/0") != std::string::npos,
           "RGBA and BGRA sources must be split");
    expect(last_file_log.find("tex_convert=300/153600B tex_pbo=300 tex_drop=0") != std::string::npos,
           "CPU conversions and unpack-PBO uploads must be reported");
    expect(last_file_log.find("tex_frames=300/0/0/0/0") != std::string::npos,
           "texture frames must be correlated with frame-time buckets");
    expect(last_file_log.find("batch_e=900/300/600/2") != std::string::npos,
           "exact adjacent element-draw runs must be reported");
    expect(last_file_log.find("batch_break=300/0/0/0/0/0") != std::string::npos,
           "state barriers must terminate candidate runs");

    setenv("MOBILEGLUES_PZ_CENSUS", "0", 1);
    setenv("MOBILEGLUES_PZ_VAO_FASTPATH", "0", 1);
    setenv("MOBILEGLUES_PZ_ATTRIB_FASTPATH", "0", 1);
    setenv("MOBILEGLUES_PZ_UNIFORM_FASTPATH", "0", 1);
    setenv("MOBILEGLUES_PZ_BUFFER_STREAMING", "0", 1);
    setenv("MOBILEGLUES_PZ_BUFFER_DISCARD_COALESCE", "0", 1);
    setenv("MOBILEGLUES_PZ_STATE_SHADOW", "0", 1);
    setenv("MOBILEGLUES_PZ_RUNTIME_MIPMAP_SKIP", "0", 1);
    setenv("MOBILEGLUES_PZ_QUAD_INDEX_CACHE", "0", 1);
    mg_pz_census_init();
    expect(!mg_pz_census_active && !mg_pz_vao_fastpath_active && !mg_pz_attrib_fastpath_active &&
               !mg_pz_uniform_fastpath_active && !mg_pz_buffer_streaming_active &&
               !mg_pz_buffer_discard_coalesce_active && !mg_pz_state_shadow_active &&
               !mg_pz_runtime_mipmap_skip_active && !mg_pz_quad_index_cache_active,
           "all switches must remain disableable after use");

    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PZ census checks passed", failures);
    return failures != 0;
}
