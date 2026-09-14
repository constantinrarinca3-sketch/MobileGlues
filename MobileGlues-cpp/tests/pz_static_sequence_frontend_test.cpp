#include "gl/pz_static_sequence_capture.h"
#include "gl/pz_static_sequence_census.h"
#include "gl/mg.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>

extern "C" {
void glDrawArrays(GLenum, GLint, GLsizei);
void glDrawElements(GLenum, GLsizei, GLenum, const void*);
void glDrawElementsBaseVertex(GLenum, GLsizei, GLenum, const void*, GLint);
void glDrawArraysInstancedBaseInstance(GLenum, GLint, GLsizei, GLsizei, GLuint);
}

static std::string last_log;
static int failures = 0;
static int original_calls = 0;
static gl_state_s fake_state{7, 0, 3};
thread_local gl_state_t gl_state = &fake_state;

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

GLuint find_bound_buffer_by_target(GLenum target) { return target == GL_ARRAY_BUFFER ? 10U : 20U; }
GLuint find_bound_array() { return 2U; }
bool mg_pz_buffer_cache_identity(GLuint buffer, uint64_t* lifetime, uint64_t* version, GLsizeiptr* size) {
    if (buffer != 10U && buffer != 20U) return false;
    *lifetime = buffer == 10U ? 100U : 200U;
    *version = 1;
    *size = buffer == 10U ? 4096 : 2048;
    return true;
}
bool mg_driver_texture_binding_at_unit(int unit, GLenum target, GLuint* out) {
    if (unit < 0 || unit >= 4 || out == nullptr) return false;
    if (target != GL_TEXTURE_2D && target != GL_TEXTURE_2D_ARRAY) return false;
    *out = static_cast<GLuint>((target == GL_TEXTURE_2D ? 100 : 200) + unit);
    return true;
}

#define ORIGINAL(name, signature) extern "C" void name signature { ++original_calls; }
ORIGINAL(mg_pz_original_glDrawArrays, (GLenum, GLint, GLsizei))
ORIGINAL(mg_pz_original_glDrawArraysInstanced, (GLenum, GLint, GLsizei, GLsizei))
ORIGINAL(mg_pz_original_glDrawElements, (GLenum, GLsizei, GLenum, const void*))
ORIGINAL(mg_pz_original_glDrawElementsInstanced, (GLenum, GLsizei, GLenum, const void*, GLsizei))
ORIGINAL(mg_pz_original_glDrawElementsBaseVertex, (GLenum, GLsizei, GLenum, const void*, GLint))
ORIGINAL(mg_pz_original_glDrawRangeElements, (GLenum, GLuint, GLuint, GLsizei, GLenum, const void*))
ORIGINAL(mg_pz_original_glDrawRangeElementsBaseVertex,
         (GLenum, GLuint, GLuint, GLsizei, GLenum, const void*, GLint))
ORIGINAL(mg_pz_original_glDrawElementsInstancedBaseVertex,
         (GLenum, GLsizei, GLenum, const void*, GLsizei, GLint))
ORIGINAL(mg_pz_original_glDrawArraysInstancedBaseInstance, (GLenum, GLint, GLsizei, GLsizei, GLuint))
ORIGINAL(mg_pz_original_glDrawElementsInstancedBaseInstance,
         (GLenum, GLsizei, GLenum, const void*, GLsizei, GLuint))
ORIGINAL(mg_pz_original_glDrawElementsInstancedBaseVertexBaseInstance,
         (GLenum, GLsizei, GLenum, const void*, GLsizei, GLint, GLuint))
#undef ORIGINAL

static void expect(bool condition, const char* message) {
    if (condition) return;
    std::printf("FAIL %s\n", message);
    ++failures;
}

int main() {
    setenv("MOBILEGLUES_PZ_STATIC_SEQUENCE_CENSUS", "1", 1);
    mg_pz_static_sequence_init();
    original_calls = 0;
    for (unsigned frame = 0; frame < 300; ++frame) {
        for (unsigned i = 0; i < 32; ++i)
            glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT,
                           reinterpret_cast<const void*>(static_cast<uintptr_t>(i * 12U)));
        mg_pz_static_sequence_present();
    }
    expect(original_calls == 9600, "frontend wrapper must forward every draw exactly once");
    expect(last_log.find("shape_pos_repeat=9568") != std::string::npos,
           "frontend wrapper must feed stable draw identity into the census");

    mg_pz_static_sequence_init();
    for (unsigned frame = 0; frame < 300; ++frame) {
        for (unsigned i = 0; i < 32; ++i)
            glDrawElementsBaseVertex(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT,
                                     reinterpret_cast<const void*>(static_cast<uintptr_t>(i * 12U)),
                                     static_cast<GLint>(frame));
        mg_pz_static_sequence_present();
    }
    expect(last_log.find("shape_pos_repeat=0") != std::string::npos,
           "frontend wrapper must preserve basevertex in the signature");

    mg_pz_static_sequence_init();
    for (unsigned frame = 0; frame < 300; ++frame) {
        for (unsigned i = 0; i < 32; ++i)
            glDrawArraysInstancedBaseInstance(GL_TRIANGLES, static_cast<GLint>(i), 6, 1, frame);
        mg_pz_static_sequence_present();
    }
    expect(last_log.find("shape_pos_repeat=0") != std::string::npos,
           "frontend wrapper must preserve baseinstance in the signature");

    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PZ static frontend checks passed", failures);
    return failures != 0;
}
