// MobileGlues - gl/shader.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header
#ifndef MOBILEGLUES_SHADER_H
#define MOBILEGLUES_SHADER_H

#include <GL/gl.h>
#include <string>
#include <vector>
#include "glsl/shader_compat.h"

struct shader_t {
    GLuint id;
    std::string converted;
    // Owned by value. It was a char* holding a `new char[]` that nothing ever
    // deleted: glBindFragDataLocation overwrote it, glLinkProgram nulled it and
    // glShaderSource abandoned it, so every patched shader source stayed on the
    // heap for the life of the process.
    std::string frag_data_changed_converted;
    int frag_data_changed;
};

extern struct shader_t shaderInfo;

const std::vector<mg_glsl_compat::uniform_default_value>* mg_shader_uniform_defaults(GLuint shader);
void mg_shader_deleted(GLuint shader);
#if defined(ZOMDROID_EXPERIMENTAL)
mg_glsl_compat::pz_alpha_shader_kind mg_shader_pz_alpha_kind(GLuint shader);
#endif

#ifdef __cplusplus
extern "C"
{
#endif

    GLAPI GLAPIENTRY void glShaderSource(GLuint shader, GLsizei count, const GLchar* const* string,
                                         const GLint* length);

    GLAPI GLAPIENTRY void glGetShaderiv(GLuint shader, GLenum pname, GLint* params);

#ifdef __cplusplus
}
#endif

#endif // FOLD_CRAFT_LAUNCHER_GL_LOADER_H
