// MobileGlues - gl/gl_native.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#include "../includes.h"
#include <GL/gl.h>
#include "glcorearb.h"
#include "log.h"
#include "program.h"
#include "shader.h"
#include "server_attrib.h"
#include "texture.h"
#include "../gles/loader.h"
#include "mg.h"
#include "glsl/shader_compat.h"
#include "../egl/context.h"
#include <GLES3/gl32.h>

#define DEBUG 0

namespace {
#if defined(ZOMDROID_EXPERIMENTAL)
struct stencil_face_shadow_t {
    bool func_known = false;
    GLenum func = 0;
    GLint reference = 0;
    GLuint value_mask = 0;
    bool write_mask_known = false;
    GLuint write_mask = 0;
    bool op_known = false;
    GLenum stencil_fail = 0;
    GLenum depth_fail = 0;
    GLenum depth_pass = 0;
};

struct fixed_state_shadow_t {
    unsigned long long context_id = 0;
    unsigned long long calls = 0;
    unsigned long long skipped = 0;

    bool blend_color_known = false;
    GLfloat blend_color[4] = {};
    bool blend_equation_known = false;
    GLenum blend_equation_rgb = 0;
    GLenum blend_equation_alpha = 0;
    bool blend_func_known = false;
    GLenum blend_src_rgb = 0;
    GLenum blend_dst_rgb = 0;
    GLenum blend_src_alpha = 0;
    GLenum blend_dst_alpha = 0;
    bool color_mask_known = false;
    GLboolean color_mask[4] = {};
    bool cull_face_known = false;
    GLenum cull_face = 0;
    bool depth_func_known = false;
    GLenum depth_func = 0;
    bool depth_mask_known = false;
    GLboolean depth_mask = GL_FALSE;
    bool front_face_known = false;
    GLenum front_face = 0;
    stencil_face_shadow_t stencil_front;
    stencil_face_shadow_t stencil_back;
};

thread_local fixed_state_shadow_t g_fixed_state_shadow;

fixed_state_shadow_t* fixed_state_shadow() {
    if (!mg_pz_state_shadow_active || !g_current_ctx) return nullptr;
    if (g_fixed_state_shadow.context_id != g_current_ctx->id) {
        g_fixed_state_shadow = {};
        g_fixed_state_shadow.context_id = g_current_ctx->id;
    }
    return &g_fixed_state_shadow;
}

bool fixed_state_result(fixed_state_shadow_t& state, bool exact, const char* function) {
#if defined(ZOMDROID_GL_BREADCRUMBS)
    if (mg_pz_census_active) ++state.calls;
#endif
    if (!exact) return false;
#if defined(ZOMDROID_GL_BREADCRUMBS)
    if (mg_pz_census_active) {
        ++state.skipped;
        if (state.skipped == 1 || state.skipped == 1024 || state.skipped == 65536) {
            ZOMDROID_DIAGNOSTIC_LOG("ZOMDROID_PZ_STATE_SHADOW_SKIP function=%s skipped=%llu calls=%llu", function,
                                    state.skipped, state.calls);
        }
    }
#else
    (void)function;
#endif
    return true;
}

bool fixed_blend_color(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha) {
    fixed_state_shadow_t* state = fixed_state_shadow();
    if (!state) return false;
    const bool exact = state->blend_color_known && state->blend_color[0] == red && state->blend_color[1] == green &&
                       state->blend_color[2] == blue && state->blend_color[3] == alpha;
    state->blend_color_known = true;
    state->blend_color[0] = red;
    state->blend_color[1] = green;
    state->blend_color[2] = blue;
    state->blend_color[3] = alpha;
    return fixed_state_result(*state, exact, "glBlendColor");
}

bool fixed_blend_equation(GLenum rgb, GLenum alpha, const char* function) {
    fixed_state_shadow_t* state = fixed_state_shadow();
    if (!state) return false;
    const bool exact = state->blend_equation_known && state->blend_equation_rgb == rgb &&
                       state->blend_equation_alpha == alpha;
    state->blend_equation_known = true;
    state->blend_equation_rgb = rgb;
    state->blend_equation_alpha = alpha;
    return fixed_state_result(*state, exact, function);
}

bool fixed_blend_func(GLenum src_rgb, GLenum dst_rgb, GLenum src_alpha, GLenum dst_alpha, const char* function) {
    fixed_state_shadow_t* state = fixed_state_shadow();
    if (!state) return false;
    const bool exact = state->blend_func_known && state->blend_src_rgb == src_rgb && state->blend_dst_rgb == dst_rgb &&
                       state->blend_src_alpha == src_alpha && state->blend_dst_alpha == dst_alpha;
    state->blend_func_known = true;
    state->blend_src_rgb = src_rgb;
    state->blend_dst_rgb = dst_rgb;
    state->blend_src_alpha = src_alpha;
    state->blend_dst_alpha = dst_alpha;
    return fixed_state_result(*state, exact, function);
}

bool fixed_color_mask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha) {
    fixed_state_shadow_t* state = fixed_state_shadow();
    if (!state) return false;
    const bool exact = state->color_mask_known && state->color_mask[0] == red && state->color_mask[1] == green &&
                       state->color_mask[2] == blue && state->color_mask[3] == alpha;
    state->color_mask_known = true;
    state->color_mask[0] = red;
    state->color_mask[1] = green;
    state->color_mask[2] = blue;
    state->color_mask[3] = alpha;
    return fixed_state_result(*state, exact, "glColorMask");
}

bool fixed_cull_face(GLenum value) {
    fixed_state_shadow_t* state = fixed_state_shadow();
    if (!state) return false;
    const bool exact = state->cull_face_known && state->cull_face == value;
    state->cull_face_known = true;
    state->cull_face = value;
    return fixed_state_result(*state, exact, "glCullFace");
}

bool fixed_depth_func(GLenum value) {
    fixed_state_shadow_t* state = fixed_state_shadow();
    if (!state) return false;
    const bool exact = state->depth_func_known && state->depth_func == value;
    state->depth_func_known = true;
    state->depth_func = value;
    return fixed_state_result(*state, exact, "glDepthFunc");
}

bool fixed_front_face(GLenum value) {
    fixed_state_shadow_t* state = fixed_state_shadow();
    if (!state) return false;
    const bool exact = state->front_face_known && state->front_face == value;
    state->front_face_known = true;
    state->front_face = value;
    return fixed_state_result(*state, exact, "glFrontFace");
}

bool fixed_depth_mask(GLboolean value) {
    fixed_state_shadow_t* state = fixed_state_shadow();
    if (!state) return false;
    const bool exact = state->depth_mask_known && state->depth_mask == value;
    state->depth_mask_known = true;
    state->depth_mask = value;
    return fixed_state_result(*state, exact, "glDepthMask");
}

bool stencil_face_selected(GLenum face, GLenum selected) {
    return face == selected || face == GL_FRONT_AND_BACK;
}

bool fixed_stencil_func(GLenum face, GLenum func, GLint reference, GLuint mask, const char* function) {
    fixed_state_shadow_t* state = fixed_state_shadow();
    if (!state) return false;
    const bool front_selected = stencil_face_selected(face, GL_FRONT);
    const bool back_selected = stencil_face_selected(face, GL_BACK);
    if (!front_selected && !back_selected) return fixed_state_result(*state, false, function);
    auto exact_for = [func, reference, mask](const stencil_face_shadow_t& side) {
        return side.func_known && side.func == func && side.reference == reference && side.value_mask == mask;
    };
    bool exact = true;
    if (front_selected) exact = exact && exact_for(state->stencil_front);
    if (back_selected) exact = exact && exact_for(state->stencil_back);
    auto update = [func, reference, mask](stencil_face_shadow_t& side) {
        side.func_known = true;
        side.func = func;
        side.reference = reference;
        side.value_mask = mask;
    };
    if (front_selected) update(state->stencil_front);
    if (back_selected) update(state->stencil_back);
    return fixed_state_result(*state, exact, function);
}

bool fixed_stencil_mask(GLenum face, GLuint mask, const char* function) {
    fixed_state_shadow_t* state = fixed_state_shadow();
    if (!state) return false;
    const bool front_selected = stencil_face_selected(face, GL_FRONT);
    const bool back_selected = stencil_face_selected(face, GL_BACK);
    if (!front_selected && !back_selected) return fixed_state_result(*state, false, function);
    bool exact = true;
    if (front_selected)
        exact = exact && state->stencil_front.write_mask_known && state->stencil_front.write_mask == mask;
    if (back_selected) exact = exact && state->stencil_back.write_mask_known && state->stencil_back.write_mask == mask;
    if (front_selected) {
        state->stencil_front.write_mask_known = true;
        state->stencil_front.write_mask = mask;
    }
    if (back_selected) {
        state->stencil_back.write_mask_known = true;
        state->stencil_back.write_mask = mask;
    }
    return fixed_state_result(*state, exact, function);
}

bool fixed_stencil_op(GLenum face, GLenum stencil_fail, GLenum depth_fail, GLenum depth_pass, const char* function) {
    fixed_state_shadow_t* state = fixed_state_shadow();
    if (!state) return false;
    const bool front_selected = stencil_face_selected(face, GL_FRONT);
    const bool back_selected = stencil_face_selected(face, GL_BACK);
    if (!front_selected && !back_selected) return fixed_state_result(*state, false, function);
    auto exact_for = [stencil_fail, depth_fail, depth_pass](const stencil_face_shadow_t& side) {
        return side.op_known && side.stencil_fail == stencil_fail && side.depth_fail == depth_fail &&
               side.depth_pass == depth_pass;
    };
    bool exact = true;
    if (front_selected) exact = exact && exact_for(state->stencil_front);
    if (back_selected) exact = exact && exact_for(state->stencil_back);
    auto update = [stencil_fail, depth_fail, depth_pass](stencil_face_shadow_t& side) {
        side.op_known = true;
        side.stencil_fail = stencil_fail;
        side.depth_fail = depth_fail;
        side.depth_pass = depth_pass;
    };
    if (front_selected) update(state->stencil_front);
    if (back_selected) update(state->stencil_back);
    return fixed_state_result(*state, exact, function);
}
#endif

template <typename T, typename... Rest>
bool uniform_scalars_should_skip(GLuint program, GLint location, uint32_t signature, T first, Rest... rest) {
    const T values[] = {first, static_cast<T>(rest)...};
    return mg_pz_uniform_call(program, location, signature, 1, values, sizeof(values));
}

template <typename T, typename... Rest>
void census_attrib_scalars(GLuint index, uint32_t signature, T first, Rest... rest) {
    const T values[] = {first, static_cast<T>(rest)...};
    mg_pz_census_attrib_value(index, signature, values, sizeof(values));
}
} // namespace

#if defined(ZOMDROID_EXPERIMENTAL)
#define MG_STATE_RETURN_IF_REDUNDANT(call)                                                                             \
    do {                                                                                                               \
        if (call) return;                                                                                              \
    } while (0)
#else
#define MG_STATE_RETURN_IF_REDUNDANT(call)                                                                             \
    do {                                                                                                               \
    } while (0)
#endif

#if defined(ZOMDROID_EXPERIMENTAL)
#define MG_UNIFORM_RETURN_IF_REDUNDANT(call)                                                                           \
    do {                                                                                                               \
        if ((mg_pz_census_active || mg_pz_uniform_fastpath_active) && (call)) return;                                  \
    } while (0)
#else
#define MG_UNIFORM_RETURN_IF_REDUNDANT(call)                                                                           \
    do {                                                                                                               \
    } while (0)
#endif

#define MG_UNIFORM_SCALAR1(name, type, signature)                                                                      \
    NATIVE_FUNCTION_HEAD(void, name, GLint location, type v0)                                                         \
    MG_UNIFORM_RETURN_IF_REDUNDANT(                                                                                    \
        uniform_scalars_should_skip(gl_state->current_program, location, signature, v0));                              \
    NATIVE_FUNCTION_END_NO_RETURN(void, name, location, v0)
#define MG_UNIFORM_SCALAR2(name, type, signature)                                                                      \
    NATIVE_FUNCTION_HEAD(void, name, GLint location, type v0, type v1)                                                \
    MG_UNIFORM_RETURN_IF_REDUNDANT(                                                                                    \
        uniform_scalars_should_skip(gl_state->current_program, location, signature, v0, v1));                          \
    NATIVE_FUNCTION_END_NO_RETURN(void, name, location, v0, v1)
#define MG_UNIFORM_SCALAR3(name, type, signature)                                                                      \
    NATIVE_FUNCTION_HEAD(void, name, GLint location, type v0, type v1, type v2)                                       \
    MG_UNIFORM_RETURN_IF_REDUNDANT(                                                                                    \
        uniform_scalars_should_skip(gl_state->current_program, location, signature, v0, v1, v2));                      \
    NATIVE_FUNCTION_END_NO_RETURN(void, name, location, v0, v1, v2)
#define MG_UNIFORM_SCALAR4(name, type, signature)                                                                      \
    NATIVE_FUNCTION_HEAD(void, name, GLint location, type v0, type v1, type v2, type v3)                              \
    MG_UNIFORM_RETURN_IF_REDUNDANT(                                                                                    \
        uniform_scalars_should_skip(gl_state->current_program, location, signature, v0, v1, v2, v3));                  \
    NATIVE_FUNCTION_END_NO_RETURN(void, name, location, v0, v1, v2, v3)
#define MG_UNIFORM_VECTOR(name, type, components, signature)                                                           \
    NATIVE_FUNCTION_HEAD(void, name, GLint location, GLsizei count, const type* value)                                \
    MG_UNIFORM_RETURN_IF_REDUNDANT(mg_pz_uniform_call(gl_state->current_program, location, signature, count, value,     \
                                                       components * sizeof(type)));                                    \
    NATIVE_FUNCTION_END_NO_RETURN(void, name, location, count, value)
#define MG_UNIFORM_MATRIX(name, columns, rows)                                                                         \
    NATIVE_FUNCTION_HEAD(void, name, GLint location, GLsizei count, GLboolean transpose, const GLfloat* value)         \
    MG_UNIFORM_RETURN_IF_REDUNDANT(                                                                                    \
        mg_pz_uniform_call(gl_state->current_program, location,                                                        \
                           0x400U | (columns << 4U) | rows | (transpose ? 0x1000U : 0U), count, value,                 \
                           columns * rows * sizeof(GLfloat)));                                                          \
    NATIVE_FUNCTION_END_NO_RETURN(void, name, location, count, transpose, value)
#define MG_PROGRAM_UNIFORM_SCALAR1(name, type, signature)                                                              \
    NATIVE_FUNCTION_HEAD(void, name, GLuint program, GLint location, type v0)                                          \
    MG_UNIFORM_RETURN_IF_REDUNDANT(uniform_scalars_should_skip(program, location, signature, v0));                     \
    NATIVE_FUNCTION_END_NO_RETURN(void, name, program, location, v0)
#define MG_PROGRAM_UNIFORM_SCALAR2(name, type, signature)                                                              \
    NATIVE_FUNCTION_HEAD(void, name, GLuint program, GLint location, type v0, type v1)                                 \
    MG_UNIFORM_RETURN_IF_REDUNDANT(uniform_scalars_should_skip(program, location, signature, v0, v1));                 \
    NATIVE_FUNCTION_END_NO_RETURN(void, name, program, location, v0, v1)
#define MG_PROGRAM_UNIFORM_SCALAR3(name, type, signature)                                                              \
    NATIVE_FUNCTION_HEAD(void, name, GLuint program, GLint location, type v0, type v1, type v2)                        \
    MG_UNIFORM_RETURN_IF_REDUNDANT(uniform_scalars_should_skip(program, location, signature, v0, v1, v2));             \
    NATIVE_FUNCTION_END_NO_RETURN(void, name, program, location, v0, v1, v2)
#define MG_PROGRAM_UNIFORM_SCALAR4(name, type, signature)                                                              \
    NATIVE_FUNCTION_HEAD(void, name, GLuint program, GLint location, type v0, type v1, type v2, type v3)               \
    MG_UNIFORM_RETURN_IF_REDUNDANT(uniform_scalars_should_skip(program, location, signature, v0, v1, v2, v3));         \
    NATIVE_FUNCTION_END_NO_RETURN(void, name, program, location, v0, v1, v2, v3)
#define MG_PROGRAM_UNIFORM_VECTOR(name, type, components, signature)                                                   \
    NATIVE_FUNCTION_HEAD(void, name, GLuint program, GLint location, GLsizei count, const type* value)                 \
    MG_UNIFORM_RETURN_IF_REDUNDANT(                                                                                    \
        mg_pz_uniform_call(program, location, signature, count, value, components * sizeof(type)));                    \
    NATIVE_FUNCTION_END_NO_RETURN(void, name, program, location, count, value)
#define MG_PROGRAM_UNIFORM_MATRIX(name, columns, rows)                                                                 \
    NATIVE_FUNCTION_HEAD(void, name, GLuint program, GLint location, GLsizei count, GLboolean transpose,               \
                         const GLfloat* value)                                                                         \
    MG_UNIFORM_RETURN_IF_REDUNDANT(                                                                                    \
        mg_pz_uniform_call(program, location,                                                                          \
                           0x400U | (columns << 4U) | rows | (transpose ? 0x1000U : 0U), count, value,                 \
                           columns * rows * sizeof(GLfloat)));                                                          \
    NATIVE_FUNCTION_END_NO_RETURN(void, name, program, location, count, transpose, value)
#define MG_ATTRIB_SCALAR1(name, type, signature)                                                                       \
    NATIVE_FUNCTION_HEAD(void, name, GLuint index, type v0)                                                           \
    MG_PZ_CENSUS(census_attrib_scalars(index, signature, v0));                                                        \
    NATIVE_FUNCTION_END_NO_RETURN(void, name, index, v0)
#define MG_ATTRIB_SCALAR2(name, type, signature)                                                                       \
    NATIVE_FUNCTION_HEAD(void, name, GLuint index, type v0, type v1)                                                  \
    MG_PZ_CENSUS(census_attrib_scalars(index, signature, v0, v1));                                                    \
    NATIVE_FUNCTION_END_NO_RETURN(void, name, index, v0, v1)
#define MG_ATTRIB_SCALAR3(name, type, signature)                                                                       \
    NATIVE_FUNCTION_HEAD(void, name, GLuint index, type v0, type v1, type v2)                                         \
    MG_PZ_CENSUS(census_attrib_scalars(index, signature, v0, v1, v2));                                                \
    NATIVE_FUNCTION_END_NO_RETURN(void, name, index, v0, v1, v2)
#define MG_ATTRIB_SCALAR4(name, type, signature)                                                                       \
    NATIVE_FUNCTION_HEAD(void, name, GLuint index, type v0, type v1, type v2, type v3)                                \
    MG_PZ_CENSUS(census_attrib_scalars(index, signature, v0, v1, v2, v3));                                            \
    NATIVE_FUNCTION_END_NO_RETURN(void, name, index, v0, v1, v2, v3)
#define MG_ATTRIB_VECTOR(name, type, components, signature)                                                            \
    NATIVE_FUNCTION_HEAD(void, name, GLuint index, const type* value)                                                 \
    MG_PZ_CENSUS(mg_pz_census_attrib_value(index, signature, value, components * sizeof(type)));                      \
    NATIVE_FUNCTION_END_NO_RETURN(void, name, index, value)

//NATIVE_FUNCTION_HEAD(void, glActiveTexture, GLenum texture) NATIVE_FUNCTION_END_NO_RETURN(void, glActiveTexture, texture)
//NATIVE_FUNCTION_HEAD(void, glAttachShader, GLuint program, GLuint shader) NATIVE_FUNCTION_END_NO_RETURN(void, glAttachShader, program,shader)
NATIVE_FUNCTION_HEAD(void, glBindAttribLocation, GLuint program, GLuint index, const GLchar *name) NATIVE_FUNCTION_END_NO_RETURN(void, glBindAttribLocation, program,index,name)
//NATIVE_FUNCTION_HEAD(void, glBindBuffer, GLenum target, GLuint buffer) NATIVE_FUNCTION_END_NO_RETURN(void, glBindBuffer, target,buffer)
//NATIVE_FUNCTION_HEAD(void, glBindFramebuffer, GLenum target, GLuint framebuffer) NATIVE_FUNCTION_END_NO_RETURN(void, glBindFramebuffer, target,framebuffer)
NATIVE_FUNCTION_HEAD(void, glBindRenderbuffer, GLenum target, GLuint renderbuffer) NATIVE_FUNCTION_END_NO_RETURN(void, glBindRenderbuffer, target,renderbuffer)
//NATIVE_FUNCTION_HEAD(void, glBindTexture, GLenum target, GLuint texture) NATIVE_FUNCTION_END_NO_RETURN(void, glBindTexture, target,texture)
NATIVE_FUNCTION_HEAD(void, glBlendColor, GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha)
    MG_STATE_RETURN_IF_REDUNDANT(fixed_blend_color(red, green, blue, alpha));
NATIVE_FUNCTION_END_NO_RETURN(void, glBlendColor, red,green,blue,alpha)
NATIVE_FUNCTION_HEAD(void, glBlendEquation, GLenum mode)
    MG_STATE_RETURN_IF_REDUNDANT(fixed_blend_equation(mode, mode, "glBlendEquation"));
NATIVE_FUNCTION_END_NO_RETURN(void, glBlendEquation, mode)
NATIVE_FUNCTION_HEAD(void, glBlendEquationSeparate, GLenum modeRGB, GLenum modeAlpha)
    MG_STATE_RETURN_IF_REDUNDANT(fixed_blend_equation(modeRGB, modeAlpha, "glBlendEquationSeparate"));
NATIVE_FUNCTION_END_NO_RETURN(void, glBlendEquationSeparate, modeRGB,modeAlpha)
NATIVE_FUNCTION_HEAD(void, glBlendFunc, GLenum sfactor, GLenum dfactor)
    MG_STATE_RETURN_IF_REDUNDANT(fixed_blend_func(sfactor, dfactor, sfactor, dfactor, "glBlendFunc"));
NATIVE_FUNCTION_END_NO_RETURN(void, glBlendFunc, sfactor,dfactor)
NATIVE_FUNCTION_HEAD(void, glBlendFuncSeparate, GLenum sfactorRGB, GLenum dfactorRGB, GLenum sfactorAlpha, GLenum dfactorAlpha)
    MG_STATE_RETURN_IF_REDUNDANT(
        fixed_blend_func(sfactorRGB, dfactorRGB, sfactorAlpha, dfactorAlpha, "glBlendFuncSeparate"));
NATIVE_FUNCTION_END_NO_RETURN(void, glBlendFuncSeparate, sfactorRGB,dfactorRGB,sfactorAlpha,dfactorAlpha)
//NATIVE_FUNCTION_HEAD(void, glBufferData, GLenum target, GLsizeiptr size, const void *data, GLenum usage) NATIVE_FUNCTION_END_NO_RETURN(void, glBufferData, target,size,data,usage)
// NATIVE_FUNCTION_HEAD(void, glBufferSubData, GLenum target, GLintptr offset, GLsizeiptr size, const void *data) NATIVE_FUNCTION_END_NO_RETURN(void, glBufferSubData, target,offset,size,data)   // moved to gl/buffer.cpp so GL_PARAMETER_BUFFER reaches a target GLES understands
//NATIVE_FUNCTION_HEAD(GLenum, glCheckFramebufferStatus, GLenum target) NATIVE_FUNCTION_END(GLenum, glCheckFramebufferStatus, target)
//NATIVE_FUNCTION_HEAD(void, glClear, GLbitfield mask) NATIVE_FUNCTION_END_NO_RETURN(void, glClear, mask)
NATIVE_FUNCTION_HEAD(void, glClearColor, GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha) NATIVE_FUNCTION_END_NO_RETURN(void, glClearColor, red,green,blue,alpha)
NATIVE_FUNCTION_HEAD(void, glClearDepthf, GLfloat d) NATIVE_FUNCTION_END_NO_RETURN(void, glClearDepthf, d)
NATIVE_FUNCTION_HEAD(void, glClearStencil, GLint s) NATIVE_FUNCTION_END_NO_RETURN(void, glClearStencil, s)
NATIVE_FUNCTION_HEAD(void, glColorMask, GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha)
    MG_STATE_RETURN_IF_REDUNDANT(fixed_color_mask(red, green, blue, alpha));
NATIVE_FUNCTION_END_NO_RETURN(void, glColorMask, red,green,blue,alpha)
NATIVE_FUNCTION_HEAD(void, glCompileShader, GLuint shader) NATIVE_FUNCTION_END_NO_RETURN(void, glCompileShader, shader)
NATIVE_FUNCTION_HEAD(void, glCompressedTexImage2D, GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const void *data) NATIVE_FUNCTION_END_NO_RETURN(void, glCompressedTexImage2D, target,level,internalformat,width,height,border,imageSize,data)
NATIVE_FUNCTION_HEAD(void, glCompressedTexSubImage2D, GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLsizei imageSize, const void *data) NATIVE_FUNCTION_END_NO_RETURN(void, glCompressedTexSubImage2D, target,level,xoffset,yoffset,width,height,format,imageSize,data)
//NATIVE_FUNCTION_HEAD(void, glCopyTexImage2D, GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width, GLsizei height, GLint border) NATIVE_FUNCTION_END_NO_RETURN(void, glCopyTexImage2D, target,level,internalformat,x,y,width,height,border)
//NATIVE_FUNCTION_HEAD(void, glCopyTexSubImage2D, GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height) NATIVE_FUNCTION_END_NO_RETURN(void, glCopyTexSubImage2D, target,level,xoffset,yoffset,x,y,width,height)
//NATIVE_FUNCTION_HEAD(GLuint, glCreateProgram) NATIVE_FUNCTION_END(GLuint, glCreateProgram)
//NATIVE_FUNCTION_HEAD(GLuint, glCreateShader, GLenum type) NATIVE_FUNCTION_END(GLuint, glCreateShader, type)
NATIVE_FUNCTION_HEAD(void, glCullFace, GLenum mode)
    MG_STATE_RETURN_IF_REDUNDANT(fixed_cull_face(mode));
NATIVE_FUNCTION_END_NO_RETURN(void, glCullFace, mode)
//NATIVE_FUNCTION_HEAD(void, glDeleteBuffers, GLsizei n, const GLuint *buffers) NATIVE_FUNCTION_END_NO_RETURN(void, glDeleteBuffers, n,buffers)
// NATIVE_FUNCTION_HEAD(void, glDeleteFramebuffers, GLsizei n, const GLuint *framebuffers) NATIVE_FUNCTION_END_NO_RETURN(void, glDeleteFramebuffers, n,framebuffers)   // implemented in gl/framebuffer.cpp
NATIVE_FUNCTION_HEAD(void, glDeleteProgram, GLuint program)
    MG_PZ_UNIFORM_STATE(mg_pz_census_forget_program(program));
    mg_program_deleted(program);
NATIVE_FUNCTION_END_NO_RETURN(void, glDeleteProgram, program)
NATIVE_FUNCTION_HEAD(void, glDeleteRenderbuffers, GLsizei n, const GLuint *renderbuffers) NATIVE_FUNCTION_END_NO_RETURN(void, glDeleteRenderbuffers, n,renderbuffers)
NATIVE_FUNCTION_HEAD(void, glDeleteShader, GLuint shader) mg_shader_deleted(shader); NATIVE_FUNCTION_END_NO_RETURN(void, glDeleteShader, shader)
//NATIVE_FUNCTION_HEAD(void, glDeleteTextures, GLsizei n, const GLuint *textures) NATIVE_FUNCTION_END_NO_RETURN(void, glDeleteTextures, n,textures)
NATIVE_FUNCTION_HEAD(void, glDepthFunc, GLenum func)
    MG_STATE_RETURN_IF_REDUNDANT(fixed_depth_func(func));
NATIVE_FUNCTION_END_NO_RETURN(void, glDepthFunc, func)
NATIVE_FUNCTION_HEAD(void, glDepthMask, GLboolean flag)
    MG_STATE_RETURN_IF_REDUNDANT(fixed_depth_mask(flag));
NATIVE_FUNCTION_END_NO_RETURN(void, glDepthMask, flag)
NATIVE_FUNCTION_HEAD(void, glDepthRangef, GLfloat n, GLfloat f)
    mg_server_attrib_note_depth_range(n, f);
NATIVE_FUNCTION_END_NO_RETURN(void, glDepthRangef, n,f)
NATIVE_FUNCTION_HEAD(void, glDetachShader, GLuint program, GLuint shader) mg_shader_detached(program, shader); NATIVE_FUNCTION_END_NO_RETURN(void, glDetachShader, program,shader)
// NATIVE_FUNCTION_HEAD(void, glDisable, GLenum cap) NATIVE_FUNCTION_END_NO_RETURN(void, glDisable, cap)   // moved to gl/enable.cpp (virtual enable state table)
#if !defined(ZOMDROID_EXPERIMENTAL)
NATIVE_FUNCTION_HEAD(void, glDisableVertexAttribArray, GLuint index) NATIVE_FUNCTION_END_NO_RETURN(void, glDisableVertexAttribArray, index)
#endif
// NATIVE_FUNCTION_HEAD(void, glDrawArrays, GLenum mode, GLint first, GLsizei count) NATIVE_FUNCTION_END_NO_RETURN(void, glDrawArrays, mode,first,count)   // moved to gl/drawing.cpp: converts desktop GL_QUADS to GLES triangles
//NATIVE_FUNCTION_HEAD(void, glDrawElements, GLenum mode, GLsizei count, GLenum type, const void *indices) NATIVE_FUNCTION_END_NO_RETURN(void, glDrawElements, mode,count,type,indices)
// NATIVE_FUNCTION_HEAD(void, glEnable, GLenum cap) NATIVE_FUNCTION_END_NO_RETURN(void, glEnable, cap)   // moved to gl/enable.cpp (virtual enable state table)
#if !defined(ZOMDROID_EXPERIMENTAL)
NATIVE_FUNCTION_HEAD(void, glEnableVertexAttribArray, GLuint index) NATIVE_FUNCTION_END_NO_RETURN(void, glEnableVertexAttribArray, index)
#endif
NATIVE_FUNCTION_HEAD(void, glFinish) NATIVE_FUNCTION_END_NO_RETURN(void, glFinish)
NATIVE_FUNCTION_HEAD(void, glFlush) NATIVE_FUNCTION_END_NO_RETURN(void, glFlush)
// NATIVE_FUNCTION_HEAD(void, glFramebufferRenderbuffer, GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer) NATIVE_FUNCTION_END_NO_RETURN(void, glFramebufferRenderbuffer, target,attachment,renderbuffertarget,renderbuffer)   // implemented in gl/framebuffer.cpp
//NATIVE_FUNCTION_HEAD(void, glFramebufferTexture2D, GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level) NATIVE_FUNCTION_END_NO_RETURN(void, glFramebufferTexture2D, target,attachment,textarget,texture,level)
NATIVE_FUNCTION_HEAD(void, glFrontFace, GLenum mode)
    MG_STATE_RETURN_IF_REDUNDANT(fixed_front_face(mode));
NATIVE_FUNCTION_END_NO_RETURN(void, glFrontFace, mode)
//NATIVE_FUNCTION_HEAD(void, glGenBuffers, GLsizei n, GLuint *buffers) NATIVE_FUNCTION_END_NO_RETURN(void, glGenBuffers, n,buffers)
NATIVE_FUNCTION_HEAD(void, glGenerateMipmap, GLenum target)
    if (!mg_texture_prepare_generate_mipmap(target)) return;
NATIVE_FUNCTION_END_NO_RETURN(void, glGenerateMipmap, target)
NATIVE_FUNCTION_HEAD(void, glGenFramebuffers, GLsizei n, GLuint *framebuffers) NATIVE_FUNCTION_END_NO_RETURN(void, glGenFramebuffers, n,framebuffers)
NATIVE_FUNCTION_HEAD(void, glGenRenderbuffers, GLsizei n, GLuint *renderbuffers) NATIVE_FUNCTION_END_NO_RETURN(void, glGenRenderbuffers, n,renderbuffers)
NATIVE_FUNCTION_HEAD(void, glGenTextures, GLsizei n, GLuint *textures) NATIVE_FUNCTION_END_NO_RETURN(void, glGenTextures, n,textures)
NATIVE_FUNCTION_HEAD(void, glGetActiveAttrib, GLuint program, GLuint index, GLsizei bufSize, GLsizei *length, GLint *size, GLenum *type, GLchar *name) NATIVE_FUNCTION_END_NO_RETURN(void, glGetActiveAttrib, program,index,bufSize,length,size,type,name)
NATIVE_FUNCTION_HEAD(void, glGetActiveUniform, GLuint program, GLuint index, GLsizei bufSize, GLsizei *length, GLint *size, GLenum *type, GLchar *name) NATIVE_FUNCTION_END_NO_RETURN(void, glGetActiveUniform, program,index,bufSize,length,size,type,name)
NATIVE_FUNCTION_HEAD(void, glGetAttachedShaders, GLuint program, GLsizei maxCount, GLsizei *count, GLuint *shaders) NATIVE_FUNCTION_END_NO_RETURN(void, glGetAttachedShaders, program,maxCount,count,shaders)
NATIVE_FUNCTION_HEAD(GLint, glGetAttribLocation, GLuint program, const GLchar *name) NATIVE_FUNCTION_END(GLint, glGetAttribLocation, program,name)
// NATIVE_FUNCTION_HEAD(void, glGetBooleanv, GLenum pname, GLboolean *data) NATIVE_FUNCTION_END_NO_RETURN(void, glGetBooleanv, pname,data)   // moved to gl/enable.cpp so it agrees with glIsEnabled
// NATIVE_FUNCTION_HEAD(void, glGetBufferParameteriv, GLenum target, GLenum pname, GLint *params) NATIVE_FUNCTION_END_NO_RETURN(void, glGetBufferParameteriv, target,pname,params)   // moved to gl/buffer.cpp so GL_PARAMETER_BUFFER reaches a target GLES understands
//NATIVE_FUNCTION_HEAD(GLenum, glGetError) NATIVE_FUNCTION_END(GLenum, glGetError)
// NATIVE_FUNCTION_HEAD(void, glGetFloatv, GLenum pname, GLfloat *data) NATIVE_FUNCTION_END_NO_RETURN(void, glGetFloatv, pname,data)   // moved to gl/enable.cpp so it agrees with glIsEnabled
NATIVE_FUNCTION_HEAD(void, glGetFramebufferAttachmentParameteriv, GLenum target, GLenum attachment, GLenum pname, GLint *params) NATIVE_FUNCTION_END_NO_RETURN(void, glGetFramebufferAttachmentParameteriv, target,attachment,pname,params)
//NATIVE_FUNCTION_HEAD(void, glGetIntegerv, GLenum pname, GLint *data) NATIVE_FUNCTION_END_NO_RETURN(void, glGetIntegerv, pname,data)
//NATIVE_FUNCTION_HEAD(void, glGetProgramiv, GLuint program, GLenum pname, GLint *params) NATIVE_FUNCTION_END_NO_RETURN(void, glGetProgramiv, program,pname,params)
NATIVE_FUNCTION_HEAD(void, glGetProgramInfoLog, GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog) NATIVE_FUNCTION_END_NO_RETURN(void, glGetProgramInfoLog, program,bufSize,length,infoLog)
NATIVE_FUNCTION_HEAD(void, glGetRenderbufferParameteriv, GLenum target, GLenum pname, GLint *params) NATIVE_FUNCTION_END_NO_RETURN(void, glGetRenderbufferParameteriv, target,pname,params)
//NATIVE_FUNCTION_HEAD(void, glGetShaderiv, GLuint shader, GLenum pname, GLint *params) NATIVE_FUNCTION_END_NO_RETURN(void, glGetShaderiv, shader,pname,params)
NATIVE_FUNCTION_HEAD(void, glGetShaderInfoLog, GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog) NATIVE_FUNCTION_END_NO_RETURN(void, glGetShaderInfoLog, shader,bufSize,length,infoLog)
NATIVE_FUNCTION_HEAD(void, glGetShaderPrecisionFormat, GLenum shadertype, GLenum precisiontype, GLint *range, GLint *precision) NATIVE_FUNCTION_END_NO_RETURN(void, glGetShaderPrecisionFormat, shadertype,precisiontype,range,precision)
NATIVE_FUNCTION_HEAD(void, glGetShaderSource, GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *source) NATIVE_FUNCTION_END_NO_RETURN(void, glGetShaderSource, shader,bufSize,length,source)
NATIVE_FUNCTION_HEAD(void, glGetTexParameterfv, GLenum target, GLenum pname, GLfloat *params) NATIVE_FUNCTION_END_NO_RETURN(void, glGetTexParameterfv, target,pname,params)
NATIVE_FUNCTION_HEAD(void, glGetTexParameteriv, GLenum target, GLenum pname, GLint *params) NATIVE_FUNCTION_END_NO_RETURN(void, glGetTexParameteriv, target,pname,params)
NATIVE_FUNCTION_HEAD(void, glGetUniformfv, GLuint program, GLint location, GLfloat *params) NATIVE_FUNCTION_END_NO_RETURN(void, glGetUniformfv, program,location,params)
NATIVE_FUNCTION_HEAD(void, glGetUniformiv, GLuint program, GLint location, GLint *params) NATIVE_FUNCTION_END_NO_RETURN(void, glGetUniformiv, program,location,params)
NATIVE_FUNCTION_HEAD(GLint, glGetUniformLocation, GLuint program, const GLchar* name)
    const GLint original_location = GLES.glGetUniformLocation(program, name);
    if (original_location >= 0 || !name) {
        CHECK_GL_ERROR
        return original_location;
    }

    std::string remapped_name;
    const GLchar* driver_name = mg_glsl_compat::remap_texture_sampler_uniform_name(name, remapped_name);
    if (driver_name == name) {
        CHECK_GL_ERROR
        return original_location;
    }

    const GLint location = GLES.glGetUniformLocation(program, driver_name);
#if defined(ZOMDROID_GL_BREADCRUMBS)
    ZOMDROID_DIAGNOSTIC_LOG("ZOMDROID_UNIFORM_ALIAS program=%u requested=%s driver=%s location=%d", program, name, driver_name,
              location);
#endif
    CHECK_GL_ERROR
    return location;
}
NATIVE_FUNCTION_HEAD(void, glGetVertexAttribfv, GLuint index, GLenum pname, GLfloat *params) NATIVE_FUNCTION_END_NO_RETURN(void, glGetVertexAttribfv, index,pname,params)
NATIVE_FUNCTION_HEAD(void, glGetVertexAttribiv, GLuint index, GLenum pname, GLint *params) NATIVE_FUNCTION_END_NO_RETURN(void, glGetVertexAttribiv, index,pname,params)
NATIVE_FUNCTION_HEAD(void, glGetVertexAttribPointerv, GLuint index, GLenum pname, void **pointer) NATIVE_FUNCTION_END_NO_RETURN(void, glGetVertexAttribPointerv, index,pname,pointer)
//NATIVE_FUNCTION_HEAD(void, glHint, GLenum target, GLenum mode) NATIVE_FUNCTION_END_NO_RETURN(void, glHint, target,mode)
//NATIVE_FUNCTION_HEAD(GLboolean, glIsBuffer, GLuint buffer) NATIVE_FUNCTION_END(GLboolean, glIsBuffer, buffer)
// NATIVE_FUNCTION_HEAD(GLboolean, glIsEnabled, GLenum cap) NATIVE_FUNCTION_END(GLboolean, glIsEnabled, cap)   // moved to gl/enable.cpp (virtual enable state table)
NATIVE_FUNCTION_HEAD(GLboolean, glIsFramebuffer, GLuint framebuffer) NATIVE_FUNCTION_END(GLboolean, glIsFramebuffer, framebuffer)
NATIVE_FUNCTION_HEAD(GLboolean, glIsProgram, GLuint program) NATIVE_FUNCTION_END(GLboolean, glIsProgram, program)
NATIVE_FUNCTION_HEAD(GLboolean, glIsRenderbuffer, GLuint renderbuffer) NATIVE_FUNCTION_END(GLboolean, glIsRenderbuffer, renderbuffer)
NATIVE_FUNCTION_HEAD(GLboolean, glIsShader, GLuint shader) NATIVE_FUNCTION_END(GLboolean, glIsShader, shader)
NATIVE_FUNCTION_HEAD(GLboolean, glIsTexture, GLuint texture) NATIVE_FUNCTION_END(GLboolean, glIsTexture, texture)
NATIVE_FUNCTION_HEAD(void, glLineWidth, GLfloat width) NATIVE_FUNCTION_END_NO_RETURN(void, glLineWidth, width)
//NATIVE_FUNCTION_HEAD(void, glLinkProgram, GLuint program) NATIVE_FUNCTION_END_NO_RETURN(void, glLinkProgram, program)
//NATIVE_FUNCTION_HEAD(void, glPixelStorei, GLenum pname, GLint param) NATIVE_FUNCTION_END_NO_RETURN(void, glPixelStorei, pname,param)
NATIVE_FUNCTION_HEAD(void, glPolygonOffset, GLfloat factor, GLfloat units) NATIVE_FUNCTION_END_NO_RETURN(void, glPolygonOffset, factor,units)
//NATIVE_FUNCTION_HEAD(void, glReadPixels, GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void *pixels) NATIVE_FUNCTION_END_NO_RETURN(void, glReadPixels, x,y,width,height,format,type,pixels)
NATIVE_FUNCTION_HEAD(void, glReleaseShaderCompiler) NATIVE_FUNCTION_END_NO_RETURN(void, glReleaseShaderCompiler)
//NATIVE_FUNCTION_HEAD(void, glRenderbufferStorage, GLenum target, GLenum internalformat, GLsizei width, GLsizei height) NATIVE_FUNCTION_END_NO_RETURN(void, glRenderbufferStorage, target,internalformat,width,height)
NATIVE_FUNCTION_HEAD(void, glSampleCoverage, GLfloat value, GLboolean invert) NATIVE_FUNCTION_END_NO_RETURN(void, glSampleCoverage, value,invert)
NATIVE_FUNCTION_HEAD(void, glScissor, GLint x, GLint y, GLsizei width, GLsizei height)
    mg_server_attrib_note_scissor(x, y, width, height);
NATIVE_FUNCTION_END_NO_RETURN(void, glScissor, x,y,width,height)
NATIVE_FUNCTION_HEAD(void, glShaderBinary, GLsizei count, const GLuint *shaders, GLenum binaryformat, const void *binary, GLsizei length) NATIVE_FUNCTION_END_NO_RETURN(void, glShaderBinary, count,shaders,binaryformat,binary,length)
//NATIVE_FUNCTION_HEAD(void, glShaderSource, GLuint shader, GLsizei count, const GLchar *const*string, const GLint *length) NATIVE_FUNCTION_END_NO_RETURN(void, glShaderSource, shader,count,string,length)
NATIVE_FUNCTION_HEAD(void, glStencilFunc, GLenum func, GLint ref, GLuint mask)
    MG_STATE_RETURN_IF_REDUNDANT(fixed_stencil_func(GL_FRONT_AND_BACK, func, ref, mask, "glStencilFunc"));
NATIVE_FUNCTION_END_NO_RETURN(void, glStencilFunc, func,ref,mask)
NATIVE_FUNCTION_HEAD(void, glStencilFuncSeparate, GLenum face, GLenum func, GLint ref, GLuint mask)
    MG_STATE_RETURN_IF_REDUNDANT(fixed_stencil_func(face, func, ref, mask, "glStencilFuncSeparate"));
NATIVE_FUNCTION_END_NO_RETURN(void, glStencilFuncSeparate, face,func,ref,mask)
NATIVE_FUNCTION_HEAD(void, glStencilMask, GLuint mask)
    MG_STATE_RETURN_IF_REDUNDANT(fixed_stencil_mask(GL_FRONT_AND_BACK, mask, "glStencilMask"));
NATIVE_FUNCTION_END_NO_RETURN(void, glStencilMask, mask)
NATIVE_FUNCTION_HEAD(void, glStencilMaskSeparate, GLenum face, GLuint mask)
    MG_STATE_RETURN_IF_REDUNDANT(fixed_stencil_mask(face, mask, "glStencilMaskSeparate"));
NATIVE_FUNCTION_END_NO_RETURN(void, glStencilMaskSeparate, face,mask)
NATIVE_FUNCTION_HEAD(void, glStencilOp, GLenum fail, GLenum zfail, GLenum zpass)
    MG_STATE_RETURN_IF_REDUNDANT(fixed_stencil_op(GL_FRONT_AND_BACK, fail, zfail, zpass, "glStencilOp"));
NATIVE_FUNCTION_END_NO_RETURN(void, glStencilOp, fail,zfail,zpass)
NATIVE_FUNCTION_HEAD(void, glStencilOpSeparate, GLenum face, GLenum sfail, GLenum dpfail, GLenum dppass)
    MG_STATE_RETURN_IF_REDUNDANT(fixed_stencil_op(face, sfail, dpfail, dppass, "glStencilOpSeparate"));
NATIVE_FUNCTION_END_NO_RETURN(void, glStencilOpSeparate, face,sfail,dpfail,dppass)
//NATIVE_FUNCTION_HEAD(void, glTexImage2D, GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels) NATIVE_FUNCTION_END_NO_RETURN(void, glTexImage2D, target,level,internalformat,width,height,border,format,type,pixels)
//NATIVE_FUNCTION_HEAD(void, glTexParameterf, GLenum target, GLenum pname, GLfloat param) NATIVE_FUNCTION_END_NO_RETURN(void, glTexParameterf, target,pname,param)
NATIVE_FUNCTION_HEAD(void, glTexParameterfv, GLenum target, GLenum pname, const GLfloat *params) NATIVE_FUNCTION_END_NO_RETURN(void, glTexParameterfv, target,pname,params)
//NATIVE_FUNCTION_HEAD(void, glTexParameteri, GLenum target, GLenum pname, GLint param) NATIVE_FUNCTION_END_NO_RETURN(void, glTexParameteri, target,pname,param)
//NATIVE_FUNCTION_HEAD(void, glTexParameteriv, GLenum target, GLenum pname, const GLint *params) NATIVE_FUNCTION_END_NO_RETURN(void, glTexParameteriv, target,pname,params)
//NATIVE_FUNCTION_HEAD(void, glTexSubImage2D, GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void *pixels) NATIVE_FUNCTION_END_NO_RETURN(void, glTexSubImage2D, target,level,xoffset,yoffset,width,height,format,type,pixels)
MG_UNIFORM_SCALAR1(glUniform1f, GLfloat, 0x301U)
MG_UNIFORM_VECTOR(glUniform1fv, GLfloat, 1, 0x301U)
//NATIVE_FUNCTION_HEAD(void, glUniform1i, GLint location, GLint v0) NATIVE_FUNCTION_END_NO_RETURN(void, glUniform1i, location,v0)
MG_UNIFORM_VECTOR(glUniform1iv, GLint, 1, 0x101U)
MG_UNIFORM_SCALAR2(glUniform2f, GLfloat, 0x302U)
MG_UNIFORM_VECTOR(glUniform2fv, GLfloat, 2, 0x302U)
MG_UNIFORM_SCALAR2(glUniform2i, GLint, 0x102U)
MG_UNIFORM_VECTOR(glUniform2iv, GLint, 2, 0x102U)
MG_UNIFORM_SCALAR3(glUniform3f, GLfloat, 0x303U)
MG_UNIFORM_VECTOR(glUniform3fv, GLfloat, 3, 0x303U)
MG_UNIFORM_SCALAR3(glUniform3i, GLint, 0x103U)
MG_UNIFORM_VECTOR(glUniform3iv, GLint, 3, 0x103U)
MG_UNIFORM_SCALAR4(glUniform4f, GLfloat, 0x304U)
MG_UNIFORM_VECTOR(glUniform4fv, GLfloat, 4, 0x304U)
MG_UNIFORM_SCALAR4(glUniform4i, GLint, 0x104U)
MG_UNIFORM_VECTOR(glUniform4iv, GLint, 4, 0x104U)
MG_UNIFORM_MATRIX(glUniformMatrix2fv, 2, 2)
MG_UNIFORM_MATRIX(glUniformMatrix3fv, 3, 3)
MG_UNIFORM_MATRIX(glUniformMatrix4fv, 4, 4)
//NATIVE_FUNCTION_HEAD(void, glUseProgram, GLuint program) NATIVE_FUNCTION_END_NO_RETURN(void, glUseProgram, program)
NATIVE_FUNCTION_HEAD(void, glValidateProgram, GLuint program) NATIVE_FUNCTION_END_NO_RETURN(void, glValidateProgram, program)
MG_ATTRIB_SCALAR1(glVertexAttrib1f, GLfloat, 0x301U)
MG_ATTRIB_VECTOR(glVertexAttrib1fv, GLfloat, 1, 0x301U)
MG_ATTRIB_SCALAR2(glVertexAttrib2f, GLfloat, 0x302U)
MG_ATTRIB_VECTOR(glVertexAttrib2fv, GLfloat, 2, 0x302U)
MG_ATTRIB_SCALAR3(glVertexAttrib3f, GLfloat, 0x303U)
MG_ATTRIB_VECTOR(glVertexAttrib3fv, GLfloat, 3, 0x303U)
MG_ATTRIB_SCALAR4(glVertexAttrib4f, GLfloat, 0x304U)
MG_ATTRIB_VECTOR(glVertexAttrib4fv, GLfloat, 4, 0x304U)
#if !defined(ZOMDROID_EXPERIMENTAL)
NATIVE_FUNCTION_HEAD(void, glVertexAttribPointer, GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer) NATIVE_FUNCTION_END_NO_RETURN(void, glVertexAttribPointer, index,size,type,normalized,stride,pointer)
#endif
//NATIVE_FUNCTION_HEAD(void, glViewport, GLint x, GLint y, GLsizei width, GLsizei height) NATIVE_FUNCTION_END_NO_RETURN(void, glViewport, x,y,width,height)
//NATIVE_FUNCTION_HEAD(void, glReadBuffer, GLenum src) NATIVE_FUNCTION_END_NO_RETURN(void, glReadBuffer, src)
// NATIVE_FUNCTION_HEAD(void, glDrawRangeElements, GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void *indices) NATIVE_FUNCTION_END_NO_RETURN(void, glDrawRangeElements, mode,start,end,count,type,indices)   // moved to gl/drawing.cpp: honours GL_PRIMITIVE_RESTART
//NATIVE_FUNCTION_HEAD(void, glTexImage3D, GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLenum format, GLenum type, const void *pixels) NATIVE_FUNCTION_END_NO_RETURN(void, glTexImage3D, target,level,internalformat,width,height,depth,border,format,type,pixels)
// NATIVE_FUNCTION_HEAD(void, glTexSubImage3D, GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, const void *pixels) NATIVE_FUNCTION_END_NO_RETURN(void, glTexSubImage3D, target,level,xoffset,yoffset,zoffset,width,height,depth,format,type,pixels)   // moved to gl/texture.cpp so BGRA and the packed 8888 types are converted
NATIVE_FUNCTION_HEAD(void, glCopyTexSubImage3D, GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLint x, GLint y, GLsizei width, GLsizei height) NATIVE_FUNCTION_END_NO_RETURN(void, glCopyTexSubImage3D, target,level,xoffset,yoffset,zoffset,x,y,width,height)
NATIVE_FUNCTION_HEAD(void, glCompressedTexImage3D, GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLsizei imageSize, const void *data) NATIVE_FUNCTION_END_NO_RETURN(void, glCompressedTexImage3D, target,level,internalformat,width,height,depth,border,imageSize,data)
NATIVE_FUNCTION_HEAD(void, glCompressedTexSubImage3D, GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLsizei imageSize, const void *data) NATIVE_FUNCTION_END_NO_RETURN(void, glCompressedTexSubImage3D, target,level,xoffset,yoffset,zoffset,width,height,depth,format,imageSize,data)
NATIVE_FUNCTION_HEAD(void, glGenQueries, GLsizei n, GLuint *ids) NATIVE_FUNCTION_END_NO_RETURN(void, glGenQueries, n,ids)
NATIVE_FUNCTION_HEAD(void, glDeleteQueries, GLsizei n, const GLuint *ids) NATIVE_FUNCTION_END_NO_RETURN(void, glDeleteQueries, n,ids)
NATIVE_FUNCTION_HEAD(GLboolean, glIsQuery, GLuint id) NATIVE_FUNCTION_END(GLboolean, glIsQuery, id)
NATIVE_FUNCTION_HEAD(void, glBeginQuery, GLenum target, GLuint id) NATIVE_FUNCTION_END_NO_RETURN(void, glBeginQuery, target,id)
NATIVE_FUNCTION_HEAD(void, glEndQuery, GLenum target) NATIVE_FUNCTION_END_NO_RETURN(void, glEndQuery, target)
NATIVE_FUNCTION_HEAD(void, glGetQueryiv, GLenum target, GLenum pname, GLint *params) NATIVE_FUNCTION_END_NO_RETURN(void, glGetQueryiv, target,pname,params)
NATIVE_FUNCTION_HEAD(void, glGetQueryObjectuiv, GLuint id, GLenum pname, GLuint *params) NATIVE_FUNCTION_END_NO_RETURN(void, glGetQueryObjectuiv, id,pname,params)
//NATIVE_FUNCTION_HEAD(GLboolean, glUnmapBuffer, GLenum target) NATIVE_FUNCTION_END(GLboolean, glUnmapBuffer, target)
// NATIVE_FUNCTION_HEAD(void, glGetBufferPointerv, GLenum target, GLenum pname, void **params) NATIVE_FUNCTION_END_NO_RETURN(void, glGetBufferPointerv, target,pname,params)   // moved to gl/buffer.cpp so CPU staging maps expose their frontend pointer
//NATIVE_FUNCTION_HEAD(void, glDrawBuffers, GLsizei n, const GLenum *bufs) NATIVE_FUNCTION_END_NO_RETURN(void, glDrawBuffers, n,bufs)
MG_UNIFORM_MATRIX(glUniformMatrix2x3fv, 2, 3)
MG_UNIFORM_MATRIX(glUniformMatrix3x2fv, 3, 2)
MG_UNIFORM_MATRIX(glUniformMatrix2x4fv, 2, 4)
MG_UNIFORM_MATRIX(glUniformMatrix4x2fv, 4, 2)
MG_UNIFORM_MATRIX(glUniformMatrix3x4fv, 3, 4)
MG_UNIFORM_MATRIX(glUniformMatrix4x3fv, 4, 3)
// NATIVE_FUNCTION_HEAD(void, glBlitFramebuffer, GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter) NATIVE_FUNCTION_END_NO_RETURN(void, glBlitFramebuffer, srcX0,srcY0,srcX1,srcY1,dstX0,dstY0,dstX1,dstY1,mask,filter)   // implemented in gl/framebuffer.cpp
//NATIVE_FUNCTION_HEAD(void, glRenderbufferStorageMultisample, GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height) NATIVE_FUNCTION_END_NO_RETURN(void, glRenderbufferStorageMultisample, target,samples,internalformat,width,height)
// NATIVE_FUNCTION_HEAD(void, glFramebufferTextureLayer, GLenum target, GLenum attachment, GLuint texture, GLint level, GLint layer) NATIVE_FUNCTION_END_NO_RETURN(void, glFramebufferTextureLayer, target,attachment,texture,level,layer)   // implemented in gl/framebuffer.cpp
//NATIVE_FUNCTION_HEAD(void, glFlushMappedBufferRange, GLenum target, GLintptr offset, GLsizeiptr length) NATIVE_FUNCTION_END_NO_RETURN(void, glFlushMappedBufferRange, target,offset,length)
//NATIVE_FUNCTION_HEAD(void, glBindVertexArray, GLuint array) NATIVE_FUNCTION_END_NO_RETURN(void, glBindVertexArray, array)
//NATIVE_FUNCTION_HEAD(void, glDeleteVertexArrays, GLsizei n, const GLuint *arrays) NATIVE_FUNCTION_END_NO_RETURN(void, glDeleteVertexArrays, n,arrays)
//NATIVE_FUNCTION_HEAD(void, glGenVertexArrays, GLsizei n, GLuint *arrays) NATIVE_FUNCTION_END_NO_RETURN(void, glGenVertexArrays, n,arrays)
//NATIVE_FUNCTION_HEAD(GLboolean, glIsVertexArray, GLuint array) NATIVE_FUNCTION_END(GLboolean, glIsVertexArray, array)
NATIVE_FUNCTION_HEAD(void, glGetIntegeri_v, GLenum target, GLuint index, GLint *data) NATIVE_FUNCTION_END_NO_RETURN(void, glGetIntegeri_v, target,index,data)
NATIVE_FUNCTION_HEAD(void, glBeginTransformFeedback, GLenum primitiveMode) NATIVE_FUNCTION_END_NO_RETURN(void, glBeginTransformFeedback, primitiveMode)
NATIVE_FUNCTION_HEAD(void, glEndTransformFeedback) NATIVE_FUNCTION_END_NO_RETURN(void, glEndTransformFeedback)
//NATIVE_FUNCTION_HEAD(void, glBindBufferRange, GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size) NATIVE_FUNCTION_END_NO_RETURN(void, glBindBufferRange, target,index,buffer,offset,size)
//NATIVE_FUNCTION_HEAD(void, glBindBufferBase, GLenum target, GLuint index, GLuint buffer) NATIVE_FUNCTION_END_NO_RETURN(void, glBindBufferBase, target,index,buffer)
NATIVE_FUNCTION_HEAD(void, glTransformFeedbackVaryings, GLuint program, GLsizei count, const GLchar *const*varyings, GLenum bufferMode) NATIVE_FUNCTION_END_NO_RETURN(void, glTransformFeedbackVaryings, program,count,varyings,bufferMode)
NATIVE_FUNCTION_HEAD(void, glGetTransformFeedbackVarying, GLuint program, GLuint index, GLsizei bufSize, GLsizei *length, GLsizei *size, GLenum *type, GLchar *name) NATIVE_FUNCTION_END_NO_RETURN(void, glGetTransformFeedbackVarying, program,index,bufSize,length,size,type,name)
#if !defined(ZOMDROID_EXPERIMENTAL)
NATIVE_FUNCTION_HEAD(void, glVertexAttribIPointer, GLuint index, GLint size, GLenum type, GLsizei stride, const void *pointer) NATIVE_FUNCTION_END_NO_RETURN(void, glVertexAttribIPointer, index,size,type,stride,pointer)
#endif
NATIVE_FUNCTION_HEAD(void, glGetVertexAttribIiv, GLuint index, GLenum pname, GLint *params) NATIVE_FUNCTION_END_NO_RETURN(void, glGetVertexAttribIiv, index,pname,params)
NATIVE_FUNCTION_HEAD(void, glGetVertexAttribIuiv, GLuint index, GLenum pname, GLuint *params) NATIVE_FUNCTION_END_NO_RETURN(void, glGetVertexAttribIuiv, index,pname,params)
MG_ATTRIB_SCALAR4(glVertexAttribI4i, GLint, 0x104U)
MG_ATTRIB_SCALAR4(glVertexAttribI4ui, GLuint, 0x204U)
MG_ATTRIB_VECTOR(glVertexAttribI4iv, GLint, 4, 0x104U)
MG_ATTRIB_VECTOR(glVertexAttribI4uiv, GLuint, 4, 0x204U)
NATIVE_FUNCTION_HEAD(void, glGetUniformuiv, GLuint program, GLint location, GLuint *params) NATIVE_FUNCTION_END_NO_RETURN(void, glGetUniformuiv, program,location,params)
NATIVE_FUNCTION_HEAD(GLint, glGetFragDataLocation, GLuint program, const GLchar *name) NATIVE_FUNCTION_END(GLint, glGetFragDataLocation, program,name)
MG_UNIFORM_SCALAR1(glUniform1ui, GLuint, 0x201U)
MG_UNIFORM_SCALAR2(glUniform2ui, GLuint, 0x202U)
MG_UNIFORM_SCALAR3(glUniform3ui, GLuint, 0x203U)
MG_UNIFORM_SCALAR4(glUniform4ui, GLuint, 0x204U)
MG_UNIFORM_VECTOR(glUniform1uiv, GLuint, 1, 0x201U)
MG_UNIFORM_VECTOR(glUniform2uiv, GLuint, 2, 0x202U)
MG_UNIFORM_VECTOR(glUniform3uiv, GLuint, 3, 0x203U)
MG_UNIFORM_VECTOR(glUniform4uiv, GLuint, 4, 0x204U)
NATIVE_FUNCTION_HEAD(void, glClearBufferiv, GLenum buffer, GLint drawbuffer, const GLint *value) NATIVE_FUNCTION_END_NO_RETURN(void, glClearBufferiv, buffer,drawbuffer,value)
NATIVE_FUNCTION_HEAD(void, glClearBufferuiv, GLenum buffer, GLint drawbuffer, const GLuint *value) NATIVE_FUNCTION_END_NO_RETURN(void, glClearBufferuiv, buffer,drawbuffer,value)
NATIVE_FUNCTION_HEAD(void, glClearBufferfv, GLenum buffer, GLint drawbuffer, const GLfloat *value) NATIVE_FUNCTION_END_NO_RETURN(void, glClearBufferfv, buffer,drawbuffer,value)
NATIVE_FUNCTION_HEAD(void, glClearBufferfi, GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil) NATIVE_FUNCTION_END_NO_RETURN(void, glClearBufferfi, buffer,drawbuffer,depth,stencil)
NATIVE_FUNCTION_HEAD(void, glCopyBufferSubData, GLenum readTarget, GLenum writeTarget, GLintptr readOffset, GLintptr writeOffset, GLsizeiptr size) NATIVE_FUNCTION_END_NO_RETURN(void, glCopyBufferSubData, readTarget,writeTarget,readOffset,writeOffset,size)
NATIVE_FUNCTION_HEAD(void, glGetUniformIndices, GLuint program, GLsizei uniformCount, const GLchar *const*uniformNames, GLuint *uniformIndices) NATIVE_FUNCTION_END_NO_RETURN(void, glGetUniformIndices, program,uniformCount,uniformNames,uniformIndices)
NATIVE_FUNCTION_HEAD(void, glGetActiveUniformsiv, GLuint program, GLsizei uniformCount, const GLuint *uniformIndices, GLenum pname, GLint *params) NATIVE_FUNCTION_END_NO_RETURN(void, glGetActiveUniformsiv, program,uniformCount,uniformIndices,pname,params)
NATIVE_FUNCTION_HEAD(GLuint, glGetUniformBlockIndex, GLuint program, const GLchar *uniformBlockName) NATIVE_FUNCTION_END(GLuint, glGetUniformBlockIndex, program,uniformBlockName)
NATIVE_FUNCTION_HEAD(void, glGetActiveUniformBlockiv, GLuint program, GLuint uniformBlockIndex, GLenum pname, GLint *params) NATIVE_FUNCTION_END_NO_RETURN(void, glGetActiveUniformBlockiv, program,uniformBlockIndex,pname,params)
NATIVE_FUNCTION_HEAD(void, glGetActiveUniformBlockName, GLuint program, GLuint uniformBlockIndex, GLsizei bufSize, GLsizei *length, GLchar *uniformBlockName) NATIVE_FUNCTION_END_NO_RETURN(void, glGetActiveUniformBlockName, program,uniformBlockIndex,bufSize,length,uniformBlockName)
NATIVE_FUNCTION_HEAD(void, glUniformBlockBinding, GLuint program, GLuint uniformBlockIndex, GLuint uniformBlockBinding) NATIVE_FUNCTION_END_NO_RETURN(void, glUniformBlockBinding, program,uniformBlockIndex,uniformBlockBinding)
// NATIVE_FUNCTION_HEAD(void, glDrawArraysInstanced, GLenum mode, GLint first, GLsizei count, GLsizei instancecount) NATIVE_FUNCTION_END_NO_RETURN(void, glDrawArraysInstanced, mode,first,count,instancecount)   // moved to gl/drawing.cpp: converts desktop GL_QUADS to GLES triangles
// NATIVE_FUNCTION_HEAD(void, glDrawElementsInstanced, GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei instancecount) NATIVE_FUNCTION_END_NO_RETURN(void, glDrawElementsInstanced, mode,count,type,indices,instancecount)
NATIVE_FUNCTION_HEAD(GLsync, glFenceSync, GLenum condition, GLbitfield flags) NATIVE_FUNCTION_END(GLsync, glFenceSync, condition,flags)
NATIVE_FUNCTION_HEAD(GLboolean, glIsSync, GLsync sync) NATIVE_FUNCTION_END(GLboolean, glIsSync, sync)
NATIVE_FUNCTION_HEAD(void, glDeleteSync, GLsync sync) NATIVE_FUNCTION_END_NO_RETURN(void, glDeleteSync, sync)
NATIVE_FUNCTION_HEAD(GLenum, glClientWaitSync, GLsync sync, GLbitfield flags, GLuint64 timeout) NATIVE_FUNCTION_END(GLenum, glClientWaitSync, sync,flags,timeout)
NATIVE_FUNCTION_HEAD(void, glWaitSync, GLsync sync, GLbitfield flags, GLuint64 timeout) NATIVE_FUNCTION_END_NO_RETURN(void, glWaitSync, sync,flags,timeout)
// NATIVE_FUNCTION_HEAD(void, glGetInteger64v, GLenum pname, GLint64 *data) NATIVE_FUNCTION_END_NO_RETURN(void, glGetInteger64v, pname,data)   // moved to gl/enable.cpp so it agrees with glIsEnabled
NATIVE_FUNCTION_HEAD(void, glGetSynciv, GLsync sync, GLenum pname, GLsizei bufSize, GLsizei *length, GLint *values) NATIVE_FUNCTION_END_NO_RETURN(void, glGetSynciv, sync,pname,bufSize,length,values)
NATIVE_FUNCTION_HEAD(void, glGetInteger64i_v, GLenum target, GLuint index, GLint64 *data) NATIVE_FUNCTION_END_NO_RETURN(void, glGetInteger64i_v, target,index,data)
NATIVE_FUNCTION_HEAD(void, glGetBufferParameteri64v, GLenum target, GLenum pname, GLint64 *params) NATIVE_FUNCTION_END_NO_RETURN(void, glGetBufferParameteri64v, target,pname,params)
NATIVE_FUNCTION_HEAD(void, glGenSamplers, GLsizei count, GLuint *samplers) NATIVE_FUNCTION_END_NO_RETURN(void, glGenSamplers, count,samplers)
NATIVE_FUNCTION_HEAD(void, glDeleteSamplers, GLsizei count, const GLuint *samplers) NATIVE_FUNCTION_END_NO_RETURN(void, glDeleteSamplers, count,samplers)
NATIVE_FUNCTION_HEAD(GLboolean, glIsSampler, GLuint sampler) NATIVE_FUNCTION_END(GLboolean, glIsSampler, sampler)
NATIVE_FUNCTION_HEAD(void, glBindSampler, GLuint unit, GLuint sampler) NATIVE_FUNCTION_END_NO_RETURN(void, glBindSampler, unit,sampler)
NATIVE_FUNCTION_HEAD(void, glSamplerParameteri, GLuint sampler, GLenum pname, GLint param) NATIVE_FUNCTION_END_NO_RETURN(void, glSamplerParameteri, sampler,pname,param)
NATIVE_FUNCTION_HEAD(void, glSamplerParameteriv, GLuint sampler, GLenum pname, const GLint *param) NATIVE_FUNCTION_END_NO_RETURN(void, glSamplerParameteriv, sampler,pname,param)
NATIVE_FUNCTION_HEAD(void, glSamplerParameterf, GLuint sampler, GLenum pname, GLfloat param) NATIVE_FUNCTION_END_NO_RETURN(void, glSamplerParameterf, sampler,pname,param)
NATIVE_FUNCTION_HEAD(void, glSamplerParameterfv, GLuint sampler, GLenum pname, const GLfloat *param) NATIVE_FUNCTION_END_NO_RETURN(void, glSamplerParameterfv, sampler,pname,param)
NATIVE_FUNCTION_HEAD(void, glGetSamplerParameteriv, GLuint sampler, GLenum pname, GLint *params) NATIVE_FUNCTION_END_NO_RETURN(void, glGetSamplerParameteriv, sampler,pname,params)
NATIVE_FUNCTION_HEAD(void, glGetSamplerParameterfv, GLuint sampler, GLenum pname, GLfloat *params) NATIVE_FUNCTION_END_NO_RETURN(void, glGetSamplerParameterfv, sampler,pname,params)
#if !defined(ZOMDROID_EXPERIMENTAL)
NATIVE_FUNCTION_HEAD(void, glVertexAttribDivisor, GLuint index, GLuint divisor) NATIVE_FUNCTION_END_NO_RETURN(void, glVertexAttribDivisor, index,divisor)
#endif
NATIVE_FUNCTION_HEAD(void, glBindTransformFeedback, GLenum target, GLuint id) NATIVE_FUNCTION_END_NO_RETURN(void, glBindTransformFeedback, target,id)
NATIVE_FUNCTION_HEAD(void, glDeleteTransformFeedbacks, GLsizei n, const GLuint *ids) NATIVE_FUNCTION_END_NO_RETURN(void, glDeleteTransformFeedbacks, n,ids)
NATIVE_FUNCTION_HEAD(void, glGenTransformFeedbacks, GLsizei n, GLuint *ids) NATIVE_FUNCTION_END_NO_RETURN(void, glGenTransformFeedbacks, n,ids)
NATIVE_FUNCTION_HEAD(GLboolean, glIsTransformFeedback, GLuint id) NATIVE_FUNCTION_END(GLboolean, glIsTransformFeedback, id)
NATIVE_FUNCTION_HEAD(void, glPauseTransformFeedback) NATIVE_FUNCTION_END_NO_RETURN(void, glPauseTransformFeedback)
NATIVE_FUNCTION_HEAD(void, glResumeTransformFeedback) NATIVE_FUNCTION_END_NO_RETURN(void, glResumeTransformFeedback)
NATIVE_FUNCTION_HEAD(void, glGetProgramBinary, GLuint program, GLsizei bufSize, GLsizei *length, GLenum *binaryFormat, void *binary) NATIVE_FUNCTION_END_NO_RETURN(void, glGetProgramBinary, program,bufSize,length,binaryFormat,binary)
NATIVE_FUNCTION_HEAD(void, glProgramBinary, GLuint program, GLenum binaryFormat, const void *binary, GLsizei length) NATIVE_FUNCTION_END_NO_RETURN(void, glProgramBinary, program,binaryFormat,binary,length)
NATIVE_FUNCTION_HEAD(void, glProgramParameteri, GLuint program, GLenum pname, GLint value) NATIVE_FUNCTION_END_NO_RETURN(void, glProgramParameteri, program,pname,value)
NATIVE_FUNCTION_HEAD(void, glInvalidateFramebuffer, GLenum target, GLsizei numAttachments, const GLenum *attachments) NATIVE_FUNCTION_END_NO_RETURN(void, glInvalidateFramebuffer, target,numAttachments,attachments)
NATIVE_FUNCTION_HEAD(void, glInvalidateSubFramebuffer, GLenum target, GLsizei numAttachments, const GLenum *attachments, GLint x, GLint y, GLsizei width, GLsizei height) NATIVE_FUNCTION_END_NO_RETURN(void, glInvalidateSubFramebuffer, target,numAttachments,attachments,x,y,width,height)
//NATIVE_FUNCTION_HEAD(void, glTexStorage2D, GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height) NATIVE_FUNCTION_END_NO_RETURN(void, glTexStorage2D, target,levels,internalformat,width,height)
//NATIVE_FUNCTION_HEAD(void, glTexStorage3D, GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth) NATIVE_FUNCTION_END_NO_RETURN(void, glTexStorage3D, target,levels,internalformat,width,height,depth)
NATIVE_FUNCTION_HEAD(void, glGetInternalformativ, GLenum target, GLenum internalformat, GLenum pname, GLsizei bufSize, GLint *params) NATIVE_FUNCTION_END_NO_RETURN(void, glGetInternalformativ, target,internalformat,pname,bufSize,params)
//NATIVE_FUNCTION_HEAD(void, glDispatchCompute, GLuint num_groups_x, GLuint num_groups_y, GLuint num_groups_z) NATIVE_FUNCTION_END_NO_RETURN(void, glDispatchCompute, num_groups_x,num_groups_y,num_groups_z)
NATIVE_FUNCTION_HEAD(void, glDispatchComputeIndirect, GLintptr indirect) NATIVE_FUNCTION_END_NO_RETURN(void, glDispatchComputeIndirect, indirect)
NATIVE_FUNCTION_HEAD(void, glDrawArraysIndirect, GLenum mode, const void *indirect) NATIVE_FUNCTION_END_NO_RETURN(void, glDrawArraysIndirect, mode,indirect)
NATIVE_FUNCTION_HEAD(void, glDrawElementsIndirect, GLenum mode, GLenum type, const void *indirect) NATIVE_FUNCTION_END_NO_RETURN(void, glDrawElementsIndirect, mode,type,indirect)
NATIVE_FUNCTION_HEAD(void, glFramebufferParameteri, GLenum target, GLenum pname, GLint param) NATIVE_FUNCTION_END_NO_RETURN(void, glFramebufferParameteri, target,pname,param)
NATIVE_FUNCTION_HEAD(void, glGetFramebufferParameteriv, GLenum target, GLenum pname, GLint *params) NATIVE_FUNCTION_END_NO_RETURN(void, glGetFramebufferParameteriv, target,pname,params)
NATIVE_FUNCTION_HEAD(void, glGetProgramInterfaceiv, GLuint program, GLenum programInterface, GLenum pname, GLint *params) NATIVE_FUNCTION_END_NO_RETURN(void, glGetProgramInterfaceiv, program,programInterface,pname,params)
NATIVE_FUNCTION_HEAD(GLuint, glGetProgramResourceIndex, GLuint program, GLenum programInterface, const GLchar *name) NATIVE_FUNCTION_END(GLuint, glGetProgramResourceIndex, program,programInterface,name)
NATIVE_FUNCTION_HEAD(void, glGetProgramResourceName, GLuint program, GLenum programInterface, GLuint index, GLsizei bufSize, GLsizei *length, GLchar *name) NATIVE_FUNCTION_END_NO_RETURN(void, glGetProgramResourceName, program,programInterface,index,bufSize,length,name)
NATIVE_FUNCTION_HEAD(void, glGetProgramResourceiv, GLuint program, GLenum programInterface, GLuint index, GLsizei propCount, const GLenum *props, GLsizei bufSize, GLsizei *length, GLint *params) NATIVE_FUNCTION_END_NO_RETURN(void, glGetProgramResourceiv, program,programInterface,index,propCount,props,bufSize,length,params)
NATIVE_FUNCTION_HEAD(GLint, glGetProgramResourceLocation, GLuint program, GLenum programInterface, const GLchar *name) NATIVE_FUNCTION_END(GLint, glGetProgramResourceLocation, program,programInterface,name)
NATIVE_FUNCTION_HEAD(void, glUseProgramStages, GLuint pipeline, GLbitfield stages, GLuint program) NATIVE_FUNCTION_END_NO_RETURN(void, glUseProgramStages, pipeline,stages,program)
NATIVE_FUNCTION_HEAD(void, glActiveShaderProgram, GLuint pipeline, GLuint program) NATIVE_FUNCTION_END_NO_RETURN(void, glActiveShaderProgram, pipeline,program)
NATIVE_FUNCTION_HEAD(GLuint, glCreateShaderProgramv, GLenum type, GLsizei count, const GLchar *const*strings) NATIVE_FUNCTION_END(GLuint, glCreateShaderProgramv, type,count,strings)
NATIVE_FUNCTION_HEAD(void, glBindProgramPipeline, GLuint pipeline) NATIVE_FUNCTION_END_NO_RETURN(void, glBindProgramPipeline, pipeline)
NATIVE_FUNCTION_HEAD(void, glDeleteProgramPipelines, GLsizei n, const GLuint *pipelines) NATIVE_FUNCTION_END_NO_RETURN(void, glDeleteProgramPipelines, n,pipelines)
NATIVE_FUNCTION_HEAD(void, glGenProgramPipelines, GLsizei n, GLuint *pipelines) NATIVE_FUNCTION_END_NO_RETURN(void, glGenProgramPipelines, n,pipelines)
NATIVE_FUNCTION_HEAD(GLboolean, glIsProgramPipeline, GLuint pipeline) NATIVE_FUNCTION_END(GLboolean, glIsProgramPipeline, pipeline)
NATIVE_FUNCTION_HEAD(void, glGetProgramPipelineiv, GLuint pipeline, GLenum pname, GLint *params) NATIVE_FUNCTION_END_NO_RETURN(void, glGetProgramPipelineiv, pipeline,pname,params)
MG_PROGRAM_UNIFORM_SCALAR1(glProgramUniform1i, GLint, 0x101U)
MG_PROGRAM_UNIFORM_SCALAR2(glProgramUniform2i, GLint, 0x102U)
MG_PROGRAM_UNIFORM_SCALAR3(glProgramUniform3i, GLint, 0x103U)
MG_PROGRAM_UNIFORM_SCALAR4(glProgramUniform4i, GLint, 0x104U)
MG_PROGRAM_UNIFORM_SCALAR1(glProgramUniform1ui, GLuint, 0x201U)
MG_PROGRAM_UNIFORM_SCALAR2(glProgramUniform2ui, GLuint, 0x202U)
MG_PROGRAM_UNIFORM_SCALAR3(glProgramUniform3ui, GLuint, 0x203U)
MG_PROGRAM_UNIFORM_SCALAR4(glProgramUniform4ui, GLuint, 0x204U)
MG_PROGRAM_UNIFORM_SCALAR1(glProgramUniform1f, GLfloat, 0x301U)
MG_PROGRAM_UNIFORM_SCALAR2(glProgramUniform2f, GLfloat, 0x302U)
MG_PROGRAM_UNIFORM_SCALAR3(glProgramUniform3f, GLfloat, 0x303U)
MG_PROGRAM_UNIFORM_SCALAR4(glProgramUniform4f, GLfloat, 0x304U)
MG_PROGRAM_UNIFORM_VECTOR(glProgramUniform1iv, GLint, 1, 0x101U)
MG_PROGRAM_UNIFORM_VECTOR(glProgramUniform2iv, GLint, 2, 0x102U)
MG_PROGRAM_UNIFORM_VECTOR(glProgramUniform3iv, GLint, 3, 0x103U)
MG_PROGRAM_UNIFORM_VECTOR(glProgramUniform4iv, GLint, 4, 0x104U)
MG_PROGRAM_UNIFORM_VECTOR(glProgramUniform1uiv, GLuint, 1, 0x201U)
MG_PROGRAM_UNIFORM_VECTOR(glProgramUniform2uiv, GLuint, 2, 0x202U)
MG_PROGRAM_UNIFORM_VECTOR(glProgramUniform3uiv, GLuint, 3, 0x203U)
MG_PROGRAM_UNIFORM_VECTOR(glProgramUniform4uiv, GLuint, 4, 0x204U)
MG_PROGRAM_UNIFORM_VECTOR(glProgramUniform1fv, GLfloat, 1, 0x301U)
MG_PROGRAM_UNIFORM_VECTOR(glProgramUniform2fv, GLfloat, 2, 0x302U)
MG_PROGRAM_UNIFORM_VECTOR(glProgramUniform3fv, GLfloat, 3, 0x303U)
MG_PROGRAM_UNIFORM_VECTOR(glProgramUniform4fv, GLfloat, 4, 0x304U)
MG_PROGRAM_UNIFORM_MATRIX(glProgramUniformMatrix2fv, 2, 2)
MG_PROGRAM_UNIFORM_MATRIX(glProgramUniformMatrix3fv, 3, 3)
MG_PROGRAM_UNIFORM_MATRIX(glProgramUniformMatrix4fv, 4, 4)
MG_PROGRAM_UNIFORM_MATRIX(glProgramUniformMatrix2x3fv, 2, 3)
MG_PROGRAM_UNIFORM_MATRIX(glProgramUniformMatrix3x2fv, 3, 2)
MG_PROGRAM_UNIFORM_MATRIX(glProgramUniformMatrix2x4fv, 2, 4)
MG_PROGRAM_UNIFORM_MATRIX(glProgramUniformMatrix4x2fv, 4, 2)
MG_PROGRAM_UNIFORM_MATRIX(glProgramUniformMatrix3x4fv, 3, 4)
MG_PROGRAM_UNIFORM_MATRIX(glProgramUniformMatrix4x3fv, 4, 3)
NATIVE_FUNCTION_HEAD(void, glValidateProgramPipeline, GLuint pipeline) NATIVE_FUNCTION_END_NO_RETURN(void, glValidateProgramPipeline, pipeline)
NATIVE_FUNCTION_HEAD(void, glGetProgramPipelineInfoLog, GLuint pipeline, GLsizei bufSize, GLsizei *length, GLchar *infoLog) NATIVE_FUNCTION_END_NO_RETURN(void, glGetProgramPipelineInfoLog, pipeline,bufSize,length,infoLog)
//NATIVE_FUNCTION_HEAD(void, glBindImageTexture, GLuint unit, GLuint texture, GLint level, GLboolean layered, GLint layer, GLenum access, GLenum format) NATIVE_FUNCTION_END_NO_RETURN(void, glBindImageTexture, unit,texture,level,layered,layer,access,format)
NATIVE_FUNCTION_HEAD(void, glGetBooleani_v, GLenum target, GLuint index, GLboolean *data) NATIVE_FUNCTION_END_NO_RETURN(void, glGetBooleani_v, target,index,data)
//NATIVE_FUNCTION_HEAD(void, glMemoryBarrier, GLbitfield barriers) NATIVE_FUNCTION_END_NO_RETURN(void, glMemoryBarrier, barriers)
NATIVE_FUNCTION_HEAD(void, glMemoryBarrierByRegion, GLbitfield barriers) NATIVE_FUNCTION_END_NO_RETURN(void, glMemoryBarrierByRegion, barriers)
NATIVE_FUNCTION_HEAD(void, glTexStorage2DMultisample, GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height, GLboolean fixedsamplelocations) NATIVE_FUNCTION_END_NO_RETURN(void, glTexStorage2DMultisample, target,samples,internalformat,width,height,fixedsamplelocations)
NATIVE_FUNCTION_HEAD(void, glGetMultisamplefv, GLenum pname, GLuint index, GLfloat *val) NATIVE_FUNCTION_END_NO_RETURN(void, glGetMultisamplefv, pname,index,val)
NATIVE_FUNCTION_HEAD(void, glSampleMaski, GLuint maskNumber, GLbitfield mask) NATIVE_FUNCTION_END_NO_RETURN(void, glSampleMaski, maskNumber,mask)
//NATIVE_FUNCTION_HEAD(void, glGetTexLevelParameteriv, GLenum target, GLint level, GLenum pname, GLint *params) NATIVE_FUNCTION_END_NO_RETURN(void, glGetTexLevelParameteriv, target,level,pname,params)
//NATIVE_FUNCTION_HEAD(void, glGetTexLevelParameterfv, GLenum target, GLint level, GLenum pname, GLfloat *params) NATIVE_FUNCTION_END_NO_RETURN(void, glGetTexLevelParameterfv, target,level,pname,params)
//NATIVE_FUNCTION_HEAD(void, glBindVertexBuffer, GLuint bindingindex, GLuint buffer, GLintptr offset, GLsizei stride) NATIVE_FUNCTION_END_NO_RETURN(void, glBindVertexBuffer, bindingindex,buffer,offset,stride)
#if !defined(ZOMDROID_EXPERIMENTAL)
NATIVE_FUNCTION_HEAD(void, glVertexAttribFormat, GLuint attribindex, GLint size, GLenum type, GLboolean normalized, GLuint relativeoffset) NATIVE_FUNCTION_END_NO_RETURN(void, glVertexAttribFormat, attribindex,size,type,normalized,relativeoffset)
NATIVE_FUNCTION_HEAD(void, glVertexAttribIFormat, GLuint attribindex, GLint size, GLenum type, GLuint relativeoffset) NATIVE_FUNCTION_END_NO_RETURN(void, glVertexAttribIFormat, attribindex,size,type,relativeoffset)
NATIVE_FUNCTION_HEAD(void, glVertexAttribBinding, GLuint attribindex, GLuint bindingindex) NATIVE_FUNCTION_END_NO_RETURN(void, glVertexAttribBinding, attribindex,bindingindex)
NATIVE_FUNCTION_HEAD(void, glVertexBindingDivisor, GLuint bindingindex, GLuint divisor) NATIVE_FUNCTION_END_NO_RETURN(void, glVertexBindingDivisor, bindingindex,divisor)
#endif
NATIVE_FUNCTION_HEAD(void, glBlendBarrier) NATIVE_FUNCTION_END_NO_RETURN(void, glBlendBarrier)
NATIVE_FUNCTION_HEAD(void, glCopyImageSubData, GLuint srcName, GLenum srcTarget, GLint srcLevel, GLint srcX, GLint srcY, GLint srcZ, GLuint dstName, GLenum dstTarget, GLint dstLevel, GLint dstX, GLint dstY, GLint dstZ, GLsizei srcWidth, GLsizei srcHeight, GLsizei srcDepth) NATIVE_FUNCTION_END_NO_RETURN(void, glCopyImageSubData, srcName,srcTarget,srcLevel,srcX,srcY,srcZ,dstName,dstTarget,dstLevel,dstX,dstY,dstZ,srcWidth,srcHeight,srcDepth)
NATIVE_FUNCTION_HEAD(void, glDebugMessageControl, GLenum source, GLenum type, GLenum severity, GLsizei count, const GLuint *ids, GLboolean enabled) NATIVE_FUNCTION_END_NO_RETURN(void, glDebugMessageControl, source,type,severity,count,ids,enabled)
NATIVE_FUNCTION_HEAD(void, glDebugMessageInsert, GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar *buf) NATIVE_FUNCTION_END_NO_RETURN(void, glDebugMessageInsert, source,type,id,severity,length,buf)
NATIVE_FUNCTION_HEAD(void, glDebugMessageCallback, GLDEBUGPROC callback, const void *userParam) NATIVE_FUNCTION_END_NO_RETURN(void, glDebugMessageCallback, callback,userParam)
NATIVE_FUNCTION_HEAD(GLuint, glGetDebugMessageLog, GLuint count, GLsizei bufSize, GLenum *sources, GLenum *types, GLuint *ids, GLenum *severities, GLsizei *lengths, GLchar *messageLog) NATIVE_FUNCTION_END(GLuint, glGetDebugMessageLog, count,bufSize,sources,types,ids,severities,lengths,messageLog)
NATIVE_FUNCTION_HEAD(void, glPushDebugGroup, GLenum source, GLuint id, GLsizei length, const GLchar *message) NATIVE_FUNCTION_END_NO_RETURN(void, glPushDebugGroup, source,id,length,message)
NATIVE_FUNCTION_HEAD(void, glPopDebugGroup) NATIVE_FUNCTION_END_NO_RETURN(void, glPopDebugGroup)
NATIVE_FUNCTION_HEAD(void, glObjectLabel, GLenum identifier, GLuint name, GLsizei length, const GLchar *label) NATIVE_FUNCTION_END_NO_RETURN(void, glObjectLabel, identifier,name,length,label)
NATIVE_FUNCTION_HEAD(void, glGetObjectLabel, GLenum identifier, GLuint name, GLsizei bufSize, GLsizei *length, GLchar *label) NATIVE_FUNCTION_END_NO_RETURN(void, glGetObjectLabel, identifier,name,bufSize,length,label)
NATIVE_FUNCTION_HEAD(void, glObjectPtrLabel, const void *ptr, GLsizei length, const GLchar *label) NATIVE_FUNCTION_END_NO_RETURN(void, glObjectPtrLabel, ptr,length,label)
NATIVE_FUNCTION_HEAD(void, glGetObjectPtrLabel, const void *ptr, GLsizei bufSize, GLsizei *length, GLchar *label) NATIVE_FUNCTION_END_NO_RETURN(void, glGetObjectPtrLabel, ptr,bufSize,length,label)
NATIVE_FUNCTION_HEAD(void, glGetPointerv, GLenum pname, void **params) NATIVE_FUNCTION_END_NO_RETURN(void, glGetPointerv, pname,params)
// NATIVE_FUNCTION_HEAD(void, glEnablei, GLenum target, GLuint index) NATIVE_FUNCTION_END_NO_RETURN(void, glEnablei, target,index)   // moved to gl/enable.cpp (virtual enable state table)
// NATIVE_FUNCTION_HEAD(void, glDisablei, GLenum target, GLuint index) NATIVE_FUNCTION_END_NO_RETURN(void, glDisablei, target,index)   // moved to gl/enable.cpp (virtual enable state table)
NATIVE_FUNCTION_HEAD(void, glBlendEquationi, GLuint buf, GLenum mode) NATIVE_FUNCTION_END_NO_RETURN(void, glBlendEquationi, buf,mode)
NATIVE_FUNCTION_HEAD(void, glBlendEquationSeparatei, GLuint buf, GLenum modeRGB, GLenum modeAlpha) NATIVE_FUNCTION_END_NO_RETURN(void, glBlendEquationSeparatei, buf,modeRGB,modeAlpha)
NATIVE_FUNCTION_HEAD(void, glBlendFunci, GLuint buf, GLenum src, GLenum dst) NATIVE_FUNCTION_END_NO_RETURN(void, glBlendFunci, buf,src,dst)
NATIVE_FUNCTION_HEAD(void, glBlendFuncSeparatei, GLuint buf, GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha) NATIVE_FUNCTION_END_NO_RETURN(void, glBlendFuncSeparatei, buf,srcRGB,dstRGB,srcAlpha,dstAlpha)
NATIVE_FUNCTION_HEAD(void, glColorMaski, GLuint index, GLboolean r, GLboolean g, GLboolean b, GLboolean a) NATIVE_FUNCTION_END_NO_RETURN(void, glColorMaski, index,r,g,b,a)
// NATIVE_FUNCTION_HEAD(GLboolean, glIsEnabledi, GLenum target, GLuint index) NATIVE_FUNCTION_END(GLboolean, glIsEnabledi, target,index)   // moved to gl/enable.cpp (virtual enable state table)
//NATIVE_FUNCTION_HEAD(void, glDrawElementsBaseVertex, GLenum mode, GLsizei count, GLenum type, const void *indices, GLint basevertex) NATIVE_FUNCTION_END_NO_RETURN(void, glDrawElementsBaseVertex, mode,count,type,indices,basevertex)
// NATIVE_FUNCTION_HEAD(void, glDrawRangeElementsBaseVertex, GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void *indices, GLint basevertex) NATIVE_FUNCTION_END_NO_RETURN(void, glDrawRangeElementsBaseVertex, mode,start,end,count,type,indices,basevertex)   // moved to gl/drawing.cpp: honours GL_PRIMITIVE_RESTART
// NATIVE_FUNCTION_HEAD(void, glDrawElementsInstancedBaseVertex, GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei instancecount, GLint basevertex) NATIVE_FUNCTION_END_NO_RETURN(void, glDrawElementsInstancedBaseVertex, mode,count,type,indices,instancecount,basevertex)   // moved to gl/drawing.cpp: honours GL_PRIMITIVE_RESTART
//NATIVE_FUNCTION_HEAD(void, glFramebufferTexture, GLenum target, GLenum attachment, GLuint texture, GLint level) NATIVE_FUNCTION_END_NO_RETURN(void, glFramebufferTexture, target,attachment,texture,level)
NATIVE_FUNCTION_HEAD(void, glPrimitiveBoundingBox, GLfloat minX, GLfloat minY, GLfloat minZ, GLfloat minW, GLfloat maxX, GLfloat maxY, GLfloat maxZ, GLfloat maxW) NATIVE_FUNCTION_END_NO_RETURN(void, glPrimitiveBoundingBox, minX,minY,minZ,minW,maxX,maxY,maxZ,maxW)
NATIVE_FUNCTION_HEAD(GLenum, glGetGraphicsResetStatus) NATIVE_FUNCTION_END(GLenum, glGetGraphicsResetStatus)
NATIVE_FUNCTION_HEAD(void, glReadnPixels, GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLsizei bufSize, void *data) NATIVE_FUNCTION_END_NO_RETURN(void, glReadnPixels, x,y,width,height,format,type,bufSize,data)
NATIVE_FUNCTION_HEAD(void, glGetnUniformfv, GLuint program, GLint location, GLsizei bufSize, GLfloat *params) NATIVE_FUNCTION_END_NO_RETURN(void, glGetnUniformfv, program,location,bufSize,params)
NATIVE_FUNCTION_HEAD(void, glGetnUniformiv, GLuint program, GLint location, GLsizei bufSize, GLint *params) NATIVE_FUNCTION_END_NO_RETURN(void, glGetnUniformiv, program,location,bufSize,params)
NATIVE_FUNCTION_HEAD(void, glGetnUniformuiv, GLuint program, GLint location, GLsizei bufSize, GLuint *params) NATIVE_FUNCTION_END_NO_RETURN(void, glGetnUniformuiv, program,location,bufSize,params)
NATIVE_FUNCTION_HEAD(void, glMinSampleShading, GLfloat value) NATIVE_FUNCTION_END_NO_RETURN(void, glMinSampleShading, value)
NATIVE_FUNCTION_HEAD(void, glPatchParameteri, GLenum pname, GLint value) NATIVE_FUNCTION_END_NO_RETURN(void, glPatchParameteri, pname,value)
NATIVE_FUNCTION_HEAD(void, glTexParameterIiv, GLenum target, GLenum pname, const GLint *params) NATIVE_FUNCTION_END_NO_RETURN(void, glTexParameterIiv, target,pname,params)
NATIVE_FUNCTION_HEAD(void, glTexParameterIuiv, GLenum target, GLenum pname, const GLuint *params) NATIVE_FUNCTION_END_NO_RETURN(void, glTexParameterIuiv, target,pname,params)
NATIVE_FUNCTION_HEAD(void, glGetTexParameterIiv, GLenum target, GLenum pname, GLint *params) NATIVE_FUNCTION_END_NO_RETURN(void, glGetTexParameterIiv, target,pname,params)
NATIVE_FUNCTION_HEAD(void, glGetTexParameterIuiv, GLenum target, GLenum pname, GLuint *params) NATIVE_FUNCTION_END_NO_RETURN(void, glGetTexParameterIuiv, target,pname,params)
NATIVE_FUNCTION_HEAD(void, glSamplerParameterIiv, GLuint sampler, GLenum pname, const GLint *param) NATIVE_FUNCTION_END_NO_RETURN(void, glSamplerParameterIiv, sampler,pname,param)
NATIVE_FUNCTION_HEAD(void, glSamplerParameterIuiv, GLuint sampler, GLenum pname, const GLuint *param) NATIVE_FUNCTION_END_NO_RETURN(void, glSamplerParameterIuiv, sampler,pname,param)
NATIVE_FUNCTION_HEAD(void, glGetSamplerParameterIiv, GLuint sampler, GLenum pname, GLint *params) NATIVE_FUNCTION_END_NO_RETURN(void, glGetSamplerParameterIiv, sampler,pname,params)
NATIVE_FUNCTION_HEAD(void, glGetSamplerParameterIuiv, GLuint sampler, GLenum pname, GLuint *params) NATIVE_FUNCTION_END_NO_RETURN(void, glGetSamplerParameterIuiv, sampler,pname,params)
//NATIVE_FUNCTION_HEAD(void, glTexBuffer, GLenum target, GLenum internalformat, GLuint buffer) NATIVE_FUNCTION_END_NO_RETURN(void, glTexBuffer, target,internalformat,buffer)
//NATIVE_FUNCTION_HEAD(void, glTexBufferRange, GLenum target, GLenum internalformat, GLuint buffer, GLintptr offset, GLsizeiptr size) NATIVE_FUNCTION_END_NO_RETURN(void, glTexBufferRange, target,internalformat,buffer,offset,size)
NATIVE_FUNCTION_HEAD(void, glTexStorage3DMultisample, GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth, GLboolean fixedsamplelocations) NATIVE_FUNCTION_END_NO_RETURN(void, glTexStorage3DMultisample, target,samples,internalformat,width,height,depth,fixedsamplelocations)
//NATIVE_FUNCTION_HEAD(void*, glMapBufferRange, GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access) NATIVE_FUNCTION_END(void*, glMapBufferRange, target,offset,length,access)
