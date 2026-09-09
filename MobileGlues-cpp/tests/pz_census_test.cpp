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
    setenv("MOBILEGLUES_PZ_CENSUS", "0", 1);
    mg_pz_census_init();
    expect(!mg_pz_census_active, "0 must disable the census");
    expect(!mg_pz_vao_fastpath_active, "0 must disable the VAO fast path");

    setenv("MOBILEGLUES_PZ_CENSUS", "true", 1);
    mg_pz_census_init();
    expect(!mg_pz_census_active, "only the exact value 1 may enable the census");

    setenv("MOBILEGLUES_PZ_VAO_FASTPATH", "true", 1);
    mg_pz_census_init();
    expect(!mg_pz_vao_fastpath_active, "only the exact value 1 may enable the VAO fast path");

    setenv("MOBILEGLUES_PZ_VAO_FASTPATH", "1", 1);
    setenv("MOBILEGLUES_PZ_CENSUS", "1", 1);
    mg_pz_census_init();
    expect(mg_pz_census_active, "1 must enable the census");
    expect(mg_pz_vao_fastpath_active, "1 must enable the VAO fast path");

    const GLfloat uniform_value[4] = {1.0f, 2.0f, 3.0f, 4.0f};

    for (int frame = 0; frame < 300; ++frame) {
        mg_pz_census_draw(false, GL_TRIANGLES, 6, 1);
        mg_pz_census_use_program(frame % 3 == 0);
        mg_pz_census_gl_call("glUniform4fv");
        mg_pz_census_uniform(7, 3, 0x304U, 1, uniform_value, sizeof(uniform_value));
        mg_pz_census_bind_vao(true, true, true);
        mg_pz_census_gl_call("glEnableVertexAttribArray");
        mg_pz_census_attrib(mg_pz_attrib_kind::enable, true, frame != 0);
        mg_pz_census_buffer_data(128, false);
        mg_pz_census_present(true);
    }

    expect(last_file_log.find("frames=300") != std::string::npos, "report interval must be 300 frames");
    expect(last_file_log.find("draw_a=300") != std::string::npos, "array draws must be aggregated");
    expect(last_file_log.find("items=1800") != std::string::npos, "draw item count must be aggregated");
    expect(last_file_log.find("program=300/100") != std::string::npos, "redundant program calls must be split");
    expect(last_file_log.find("schema=2") != std::string::npos, "schema 2 must be reported");
    expect(last_file_log.find("vao=300/300/300/300") != std::string::npos,
           "VAO frontend, confirmed and skipped counts must be split");
    expect(last_file_log.find("uniform=300/300/299") != std::string::npos,
           "uniform tracked and exact counts must be split");
    expect(last_file_log.find("attrib=300/300/299") != std::string::npos,
           "attribute tracked and exact counts must be split");
    expect(last_file_log.find("upload=300+0/38400B") != std::string::npos, "buffer bytes must be aggregated");

    setenv("MOBILEGLUES_PZ_CENSUS", "0", 1);
    setenv("MOBILEGLUES_PZ_VAO_FASTPATH", "0", 1);
    mg_pz_census_init();
    expect(!mg_pz_census_active && !mg_pz_vao_fastpath_active, "both switches must remain disableable after use");

    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PZ census checks passed", failures);
    return failures != 0;
}
