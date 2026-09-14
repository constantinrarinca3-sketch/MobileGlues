#include "gl/pz_static_sequence_capture.h"
#include "gl/pz_static_sequence_census.h"
#include "gl/mg.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static std::string last_log;
static int failures = 0;
static gl_state_s fake_state{7, 0, 3};
thread_local gl_state_t gl_state = &fake_state;
static uint64_t array_version = 1;

extern "C" void write_log(const char* format, ...) {
    char line[8192] = {};
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    last_log = line;
}
extern "C" void write_log_n(const char*, ...) {}
int __android_log_print(int, const char*, const char*, ...) { return 0; }

GLuint find_bound_buffer_by_target(GLenum target) {
    return target == GL_ARRAY_BUFFER ? 10U : target == GL_ELEMENT_ARRAY_BUFFER ? 20U : 0U;
}
GLuint find_bound_array() { return 2U; }
bool mg_pz_buffer_cache_identity(GLuint buffer, uint64_t* lifetime, uint64_t* version, GLsizeiptr* size) {
    if (buffer == 10U) {
        *lifetime = 100;
        *version = array_version;
        *size = 4096;
        return true;
    }
    if (buffer == 20U) {
        *lifetime = 200;
        *version = 1;
        *size = 2048;
        return true;
    }
    return false;
}
bool mg_driver_texture_binding_at_unit(int unit, GLenum target, GLuint* out) {
    if (unit < 0 || unit >= 4 || out == nullptr) return false;
    *out = target == GL_TEXTURE_2D ? static_cast<GLuint>(100 + unit) : static_cast<GLuint>(200 + unit);
    return target == GL_TEXTURE_2D || target == GL_TEXTURE_2D_ARRAY;
}

static void expect(bool condition, const char* message) {
    if (condition) return;
    std::printf("FAIL %s\n", message);
    ++failures;
}

int main() {
    setenv("MOBILEGLUES_PZ_STATIC_SEQUENCE_CENSUS", "1", 1);

    mg_pz_static_sequence_init();
    array_version = 1;
    for (unsigned frame = 0; frame < 300; ++frame) {
        for (unsigned i = 0; i < 32; ++i) {
            const void* offset = reinterpret_cast<const void*>(static_cast<uintptr_t>(i * 12U));
            mg_pz_static_sequence_capture_elements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, offset, 1, 0, 0);
        }
        mg_pz_static_sequence_present();
    }
    expect(last_log.find("tracked_resource_repeat=9568") != std::string::npos,
           "capture adapter must include stable tracked buffers and textures");

    mg_pz_static_sequence_init();
    for (unsigned frame = 0; frame < 300; ++frame) {
        array_version = frame + 1;
        for (unsigned i = 0; i < 32; ++i) {
            const void* offset = reinterpret_cast<const void*>(static_cast<uintptr_t>(i * 12U));
            mg_pz_static_sequence_capture_elements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, offset, 1, 0, 0);
        }
        mg_pz_static_sequence_present();
    }
    expect(last_log.find("shape_pos_repeat=9568") != std::string::npos,
           "buffer content churn must not hide repeated command shape");
    expect(last_log.find("tracked_resource_repeat=0") != std::string::npos,
           "buffer content churn must invalidate resource reuse");

    // Index offsets are part of draw identity. A census that ignores them would
    // call these 300 different indexed frames identical and overstate replay potential.
    mg_pz_static_sequence_init();
    array_version = 1;
    for (unsigned frame = 0; frame < 300; ++frame) {
        for (unsigned i = 0; i < 32; ++i) {
            const uintptr_t byte_offset = static_cast<uintptr_t>(frame * 4096U + i * 12U);
            mg_pz_static_sequence_capture_elements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT,
                                                   reinterpret_cast<const void*>(byte_offset), 1, 0, 0);
        }
        mg_pz_static_sequence_present();
    }
    expect(last_log.find("shape_pos_repeat=0") != std::string::npos,
           "indexed draw offsets must participate in the shape signature");

    // Array first/base-instance are likewise semantic draw identity.
    mg_pz_static_sequence_init();
    for (unsigned frame = 0; frame < 300; ++frame) {
        for (unsigned i = 0; i < 32; ++i)
            mg_pz_static_sequence_capture_arrays(GL_TRIANGLES, static_cast<GLint>(frame * 64U + i), 6, 1, frame);
        mg_pz_static_sequence_present();
    }
    expect(last_log.find("shape_pos_repeat=0") != std::string::npos,
           "array first/base-instance must participate in the shape signature");

    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PZ static sequence capture checks passed", failures);
    return failures != 0;
}
