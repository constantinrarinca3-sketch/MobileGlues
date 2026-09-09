// Contract checks for the opt-in fixed-state shadow in gl_native.cpp.
#include "egl/context.h"
#include "gles/gles.h"
#include "gl/pz_census.h"

#include <cstdio>

extern "C" {
struct gles_func_t g_gles_func = {};
void* gles = nullptr;
void* egl = nullptr;
bool g_angle_in_use = false;
}

thread_local MGContext* g_current_ctx = nullptr;

bool mg_pz_census_active = false;
bool mg_pz_vao_fastpath_active = false;
bool mg_pz_attrib_fastpath_active = false;
bool mg_pz_uniform_fastpath_active = false;
bool mg_pz_buffer_streaming_active = false;
bool mg_pz_buffer_discard_coalesce_active = false;
bool mg_pz_gpu_buffer_pool_active = false;
bool mg_pz_state_shadow_active = true;
bool mg_pz_runtime_mipmap_skip_active = false;
void mg_pz_census_gl_call(const char*) {}

static int failures = 0;
static int blend_equation_calls = 0;
static int blend_equation_separate_calls = 0;
static int blend_func_calls = 0;
static int blend_func_separate_calls = 0;
static int stencil_func_calls = 0;
static int stencil_func_separate_calls = 0;
static int depth_func_calls = 0;

static void expect(bool condition, const char* message) {
    if (condition) return;
    std::printf("FAIL %s\n", message);
    ++failures;
}

static void fake_blend_equation(GLenum) { ++blend_equation_calls; }
static void fake_blend_equation_separate(GLenum, GLenum) { ++blend_equation_separate_calls; }
static void fake_blend_func(GLenum, GLenum) { ++blend_func_calls; }
static void fake_blend_func_separate(GLenum, GLenum, GLenum, GLenum) { ++blend_func_separate_calls; }
static void fake_stencil_func(GLenum, GLint, GLuint) { ++stencil_func_calls; }
static void fake_stencil_func_separate(GLenum, GLenum, GLint, GLuint) { ++stencil_func_separate_calls; }
static void fake_depth_func(GLenum) { ++depth_func_calls; }

extern "C" void glBlendEquation(GLenum mode);
extern "C" void glBlendEquationSeparate(GLenum mode_rgb, GLenum mode_alpha);
extern "C" void glBlendFunc(GLenum src, GLenum dst);
extern "C" void glBlendFuncSeparate(GLenum src_rgb, GLenum dst_rgb, GLenum src_alpha, GLenum dst_alpha);
extern "C" void glStencilFunc(GLenum func, GLint reference, GLuint mask);
extern "C" void glStencilFuncSeparate(GLenum face, GLenum func, GLint reference, GLuint mask);
extern "C" void glDepthFunc(GLenum func);

int main() {
    GLES.glBlendEquation = fake_blend_equation;
    GLES.glBlendEquationSeparate = fake_blend_equation_separate;
    GLES.glBlendFunc = fake_blend_func;
    GLES.glBlendFuncSeparate = fake_blend_func_separate;
    GLES.glStencilFunc = fake_stencil_func;
    GLES.glStencilFuncSeparate = fake_stencil_func_separate;
    GLES.glDepthFunc = fake_depth_func;

    MGContext first{};
    first.id = 1;
    g_current_ctx = &first;

    glBlendEquation(GL_FUNC_ADD);
    glBlendEquation(GL_FUNC_ADD);
    expect(blend_equation_calls == 1, "an exact blend-equation repeat must be skipped");

    glBlendEquationSeparate(GL_FUNC_SUBTRACT, GL_FUNC_ADD);
    glBlendEquation(GL_FUNC_ADD);
    expect(blend_equation_separate_calls == 1 && blend_equation_calls == 2,
           "combined blend equation must observe a prior separate-state change");

    glBlendFunc(GL_ONE, GL_ZERO);
    glBlendFunc(GL_ONE, GL_ZERO);
    expect(blend_func_calls == 1, "an exact blend-function repeat must be skipped");
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
    glBlendFunc(GL_ONE, GL_ZERO);
    expect(blend_func_separate_calls == 1 && blend_func_calls == 2,
           "combined blend function must observe a prior separate-state change");

    glStencilFunc(GL_ALWAYS, 1, 0xff);
    glStencilFunc(GL_ALWAYS, 1, 0xff);
    expect(stencil_func_calls == 1, "an exact two-face stencil repeat must be skipped");
    glStencilFuncSeparate(GL_FRONT, GL_LESS, 2, 0x0f);
    glStencilFunc(GL_ALWAYS, 1, 0xff);
    expect(stencil_func_separate_calls == 1 && stencil_func_calls == 2,
           "two-face stencil state must observe a one-face change");

    glDepthFunc(GL_LEQUAL);
    glDepthFunc(GL_LEQUAL);
    expect(depth_func_calls == 1, "an exact depth-function repeat must be skipped");

    MGContext second{};
    second.id = 2;
    g_current_ctx = &second;
    glDepthFunc(GL_LEQUAL);
    expect(depth_func_calls == 2, "a context switch must invalidate the shadow");

    mg_pz_state_shadow_active = false;
    glDepthFunc(GL_LEQUAL);
    expect(depth_func_calls == 3, "disabling the optimization must restore direct calls");

    std::printf("%s (%d failures)\n", failures ? "FAILED" : "fixed-state shadow checks passed", failures);
    return failures != 0;
}
