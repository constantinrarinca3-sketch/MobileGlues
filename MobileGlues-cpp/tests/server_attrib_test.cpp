#include "../egl/context.h"
#include "../gl/server_attrib.h"
#include "../gles/loader.h"
#include <GL/gl.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <unordered_map>

thread_local MGContext* g_current_ctx = nullptr;
gles_func_t g_gles_func{};
gles_caps_t g_gles_caps{};

namespace {

GLint driver_viewport[4] = {4, 8, 640, 360};
GLint driver_scissor[4] = {10, 20, 300, 200};
GLfloat driver_depth_range[2] = {0.25f, 0.75f};
std::unordered_map<GLenum, GLboolean> driver_enables;
GLenum frontend_error = GL_NO_ERROR;
int failures = 0;

void expect(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

void fake_get_integerv(GLenum pname, GLint* out) {
    if (pname == GL_VIEWPORT) std::memcpy(out, driver_viewport, sizeof(driver_viewport));
    if (pname == GL_SCISSOR_BOX) std::memcpy(out, driver_scissor, sizeof(driver_scissor));
}

void fake_get_floatv(GLenum pname, GLfloat* out) {
    if (pname == GL_DEPTH_RANGE) std::memcpy(out, driver_depth_range, sizeof(driver_depth_range));
}

void fake_viewport(GLint x, GLint y, GLsizei width, GLsizei height) {
    const GLint value[4] = {x, y, width, height};
    std::memcpy(driver_viewport, value, sizeof(value));
}

void fake_scissor(GLint x, GLint y, GLsizei width, GLsizei height) {
    const GLint value[4] = {x, y, width, height};
    std::memcpy(driver_scissor, value, sizeof(value));
}

void fake_depth_range(GLfloat near_value, GLfloat far_value) {
    driver_depth_range[0] = near_value;
    driver_depth_range[1] = far_value;
}

void fake_enable(GLenum cap) { driver_enables[cap] = GL_TRUE; }
void fake_disable(GLenum cap) { driver_enables[cap] = GL_FALSE; }
void fake_enable_i(GLenum cap, GLuint) { driver_enables[cap] = GL_TRUE; }
void fake_disable_i(GLenum cap, GLuint) { driver_enables[cap] = GL_FALSE; }

bool same4(const GLint* a, const GLint* b) {
    return std::memcmp(a, b, 4 * sizeof(GLint)) == 0;
}

} // namespace

void mg_set_gl_error(GLenum error) {
    if (frontend_error == GL_NO_ERROR) frontend_error = error;
}

void write_log(const char*, ...) {}
void write_log_n(const char*, ...) {}
int __android_log_print(int, const char*, const char*, ...) { return 0; }

int main() {
    GLES.glGetIntegerv = fake_get_integerv;
    GLES.glGetFloatv = fake_get_floatv;
    GLES.glViewport = fake_viewport;
    GLES.glScissor = fake_scissor;
    GLES.glDepthRangef = fake_depth_range;
    GLES.glEnable = fake_enable;
    GLES.glDisable = fake_disable;
    GLES.glEnablei = fake_enable_i;
    GLES.glDisablei = fake_disable_i;

    MGContext context{};
    g_current_ctx = &context;
    mg_enable_reset(&context.enable);
    context.enable.driver_synced = true;
    driver_enables[GL_DITHER] = GL_TRUE;

    const GLint original_viewport[4] = {4, 8, 640, 360};
    const GLint original_scissor[4] = {10, 20, 300, 200};
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    expect(mg_server_attrib_stack_depth() == 1, "push increments stack depth");

    fake_viewport(0, 0, 1280, 720);
    mg_server_attrib_note_viewport(0, 0, 1280, 720);
    fake_scissor(0, 0, 100, 100);
    mg_server_attrib_note_scissor(0, 0, 100, 100);
    glDepthRange(0.0, 1.0);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_SCISSOR_TEST);

    glPopAttrib();
    expect(mg_server_attrib_stack_depth() == 0, "pop decrements stack depth");
    expect(same4(driver_viewport, original_viewport), "pop restores viewport");
    expect(same4(driver_scissor, original_scissor), "pop restores scissor box");
    expect(driver_depth_range[0] == 0.25f && driver_depth_range[1] == 0.75f, "pop restores depth range");
    expect(mg_enable_get(GL_DEPTH_TEST, 0) == GL_FALSE, "pop restores depth-test enable");
    expect(mg_enable_get(GL_SCISSOR_TEST, 0) == GL_FALSE, "pop restores scissor-test enable");

    glPushAttrib(GL_VIEWPORT_BIT);
    fake_viewport(1, 2, 320, 180);
    mg_server_attrib_note_viewport(1, 2, 320, 180);
    glPushAttrib(GL_VIEWPORT_BIT);
    fake_viewport(3, 4, 160, 90);
    mg_server_attrib_note_viewport(3, 4, 160, 90);
    glPopAttrib();
    const GLint middle_viewport[4] = {1, 2, 320, 180};
    expect(same4(driver_viewport, middle_viewport), "inner pop restores the outer viewport");
    glPopAttrib();
    expect(same4(driver_viewport, original_viewport), "outer pop restores the original viewport");

    // GL_SCISSOR_BIT restores its enable independently of GL_ENABLE_BIT.
    glPushAttrib(GL_SCISSOR_BIT);
    glEnable(GL_SCISSOR_TEST);
    glPopAttrib();
    expect(mg_enable_get(GL_SCISSOR_TEST, 0) == GL_FALSE, "scissor bit restores scissor enable");

    glPopAttrib();
    expect(frontend_error == GL_STACK_UNDERFLOW, "empty pop raises stack underflow");

    frontend_error = GL_NO_ERROR;
    for (int i = 0; i < MG_SERVER_ATTRIB_STACK_LIMIT; ++i) glPushAttrib(GL_VIEWPORT_BIT);
    glPushAttrib(GL_VIEWPORT_BIT);
    expect(frontend_error == GL_STACK_OVERFLOW, "full stack raises stack overflow");

    if (failures == 0) std::puts("server attrib checks passed (0 failures)");
    return failures == 0 ? 0 : 1;
}
