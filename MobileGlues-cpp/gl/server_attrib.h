// MobileGlues - gl/server_attrib.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#ifndef MOBILEGLUES_SERVER_ATTRIB_H
#define MOBILEGLUES_SERVER_ATTRIB_H

#include "enable.h"
#include <GL/gl.h>
#include <array>
#include <cstddef>

#if defined(ZOMDROID_EXPERIMENTAL)

enum { MG_SERVER_ATTRIB_STACK_LIMIT = 16 };

struct mg_server_attrib_snapshot_t {
    GLbitfield mask = 0;
    GLint viewport[4] = {0, 0, 0, 0};
    GLint scissor[4] = {0, 0, 0, 0};
    GLfloat depth_range[2] = {0.0f, 1.0f};
    mg_enable_state_t enable{};
};

struct mg_server_attrib_state_t {
    std::array<mg_server_attrib_snapshot_t, MG_SERVER_ATTRIB_STACK_LIMIT> stack{};
    std::size_t depth = 0;

    GLint viewport[4] = {0, 0, 0, 0};
    GLint scissor[4] = {0, 0, 0, 0};
    GLfloat depth_range[2] = {0.0f, 1.0f};
    bool viewport_known = false;
    bool scissor_known = false;
    bool depth_range_known = false;

    unsigned long long push_hits = 0;
    unsigned long long pop_hits = 0;
};

void mg_server_attrib_note_viewport(GLint x, GLint y, GLsizei width, GLsizei height);
void mg_server_attrib_note_scissor(GLint x, GLint y, GLsizei width, GLsizei height);
void mg_server_attrib_note_depth_range(GLfloat near_value, GLfloat far_value);
GLint mg_server_attrib_stack_depth();

#else

static inline void mg_server_attrib_note_viewport(GLint, GLint, GLsizei, GLsizei) {}
static inline void mg_server_attrib_note_scissor(GLint, GLint, GLsizei, GLsizei) {}
static inline void mg_server_attrib_note_depth_range(GLfloat, GLfloat) {}

#endif

#endif // MOBILEGLUES_SERVER_ATTRIB_H
