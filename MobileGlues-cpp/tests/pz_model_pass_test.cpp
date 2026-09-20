#include "gl/pz_model_pass.h"
#include "gl/pz_census.h"

#include <cstdarg>
#include <cstdio>
#include <string>

bool mg_pz_census_active = true;
static std::string last_log;
static int failures = 0;

extern "C" void write_log(const char* format, ...) {
    char line[2048]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    last_log = line;
}
extern "C" void write_log_n(const char*, ...) {}
int __android_log_print(int, const char*, const char*, ...) { return 0; }

static void expect(bool condition, const char* message) {
    if (condition) return;
    std::printf("FAIL %s\n", message);
    ++failures;
}

int main() {
    mg_pz_model_pass_reset();
    expect(!mg_pz_model_pass_handle_marker(MG_PZ_MARKER_SOURCE_APPLICATION, MG_PZ_MARKER_TYPE, 7),
           "unreserved debug markers must reach the backend");
    expect(mg_pz_model_pass_handle_marker(MG_PZ_MARKER_SOURCE_APPLICATION, MG_PZ_MARKER_TYPE,
                                          MG_PZ_MARKER_OPAQUE_BEGIN),
           "opaque begin was not consumed");
    expect(mg_pz_model_pass_current() == mg_pz_model_pass::opaque, "opaque pass did not become active");
    mg_pz_model_pass_gl_call();
    mg_pz_model_pass_draw(6, 2);
    expect(mg_pz_model_pass_handle_marker(MG_PZ_MARKER_SOURCE_APPLICATION, MG_PZ_MARKER_TYPE,
                                          MG_PZ_MARKER_OPAQUE_END),
           "opaque end was not consumed");
    expect(mg_pz_model_pass_current() == mg_pz_model_pass::none, "opaque pass did not close");

    for (int frame = 0; frame < 300; ++frame) mg_pz_model_pass_present();
    expect(last_log.find("opaque=1/1/1/1/12") != std::string::npos,
           "opaque marker census did not preserve counts");
    expect(last_log.find("transparent=0/0/0/0/0") != std::string::npos,
           "transparent zero counts changed");
    expect(last_log.find("malformed=0") != std::string::npos, "valid markers were reported malformed");

    mg_pz_model_pass_handle_marker(MG_PZ_MARKER_SOURCE_APPLICATION, MG_PZ_MARKER_TYPE,
                                   MG_PZ_MARKER_TRANSPARENT_END);
    for (int frame = 0; frame < 300; ++frame) mg_pz_model_pass_present();
    expect(last_log.find("malformed=1") != std::string::npos, "unmatched end was not diagnosed");
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PZ model-pass marker checks passed", failures);
    return failures != 0;
}
