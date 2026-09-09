// MobileGlues - gl/server_attrib.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#include "server_attrib.h"

#if defined(ZOMDROID_EXPERIMENTAL)

#include "../egl/context.h"
#include "../gles/loader.h"
#include "log.h"
#include "mg.h"
#include <algorithm>
#include <cstring>

#define DEBUG 0

namespace {

thread_local mg_server_attrib_state_t g_fallback_server_attrib;

mg_server_attrib_state_t& current_state() {
    return g_current_ctx ? g_current_ctx->server_attrib : g_fallback_server_attrib;
}

bool trace_milestone(unsigned long long hit) {
    return hit == 1 || hit == 1024 || hit == 65536;
}

void seed_viewport(mg_server_attrib_state_t& state) {
    if (state.viewport_known) return;
    if (GLES.glGetIntegerv) GLES.glGetIntegerv(GL_VIEWPORT, state.viewport);
    state.viewport_known = true;
}

void seed_scissor(mg_server_attrib_state_t& state) {
    if (state.scissor_known) return;
    if (GLES.glGetIntegerv) GLES.glGetIntegerv(GL_SCISSOR_BOX, state.scissor);
    state.scissor_known = true;
}

void seed_depth_range(mg_server_attrib_state_t& state) {
    if (state.depth_range_known) return;
    if (GLES.glGetFloatv) GLES.glGetFloatv(GL_DEPTH_RANGE, state.depth_range);
    state.depth_range_known = true;
}

bool different4(const GLint* a, const GLint* b) {
    return std::memcmp(a, b, 4 * sizeof(GLint)) != 0;
}

} // namespace

void mg_server_attrib_note_viewport(GLint x, GLint y, GLsizei width, GLsizei height) {
    if (width < 0 || height < 0) return;
    mg_server_attrib_state_t& state = current_state();
    state.viewport[0] = x;
    state.viewport[1] = y;
    state.viewport[2] = width;
    state.viewport[3] = height;
    state.viewport_known = true;
}

void mg_server_attrib_note_scissor(GLint x, GLint y, GLsizei width, GLsizei height) {
    if (width < 0 || height < 0) return;
    mg_server_attrib_state_t& state = current_state();
    state.scissor[0] = x;
    state.scissor[1] = y;
    state.scissor[2] = width;
    state.scissor[3] = height;
    state.scissor_known = true;
}

void mg_server_attrib_note_depth_range(GLfloat near_value, GLfloat far_value) {
    mg_server_attrib_state_t& state = current_state();
    state.depth_range[0] = std::clamp(near_value, 0.0f, 1.0f);
    state.depth_range[1] = std::clamp(far_value, 0.0f, 1.0f);
    state.depth_range_known = true;
}

GLint mg_server_attrib_stack_depth() {
    return static_cast<GLint>(current_state().depth);
}

extern "C" GLAPI GLAPIENTRY void glPushAttrib(GLbitfield mask) {
    LOG()
    mg_server_attrib_state_t& state = current_state();
    if (state.depth >= MG_SERVER_ATTRIB_STACK_LIMIT) {
        mg_set_gl_error(GL_STACK_OVERFLOW);
        return;
    }

    mg_server_attrib_snapshot_t& snapshot = state.stack[state.depth++];
    snapshot.mask = mask;
    if ((mask & GL_VIEWPORT_BIT) != 0) {
        seed_viewport(state);
        seed_depth_range(state);
        std::memcpy(snapshot.viewport, state.viewport, sizeof(snapshot.viewport));
        std::memcpy(snapshot.depth_range, state.depth_range, sizeof(snapshot.depth_range));
    }
    if ((mask & GL_SCISSOR_BIT) != 0) {
        seed_scissor(state);
        std::memcpy(snapshot.scissor, state.scissor, sizeof(snapshot.scissor));
    }
    if ((mask & (GL_ENABLE_BIT | GL_SCISSOR_BIT)) != 0) snapshot.enable = *mg_enable_state();

    ++state.push_hits;
#if defined(ZOMDROID_GL_BREADCRUMBS)
    if (trace_milestone(state.push_hits)) {
        write_log("ZOMDROID_SERVER_ATTRIB_CENSUS mask=0x%x depth=%zu semantic_applied=1 hit=%llu", mask,
                  state.depth, state.push_hits);
    }
#endif
}

extern "C" GLAPI GLAPIENTRY void glPopAttrib(void) {
    LOG()
    mg_server_attrib_state_t& state = current_state();
    if (state.depth == 0) {
        mg_set_gl_error(GL_STACK_UNDERFLOW);
        return;
    }

    const mg_server_attrib_snapshot_t snapshot = state.stack[--state.depth];
    unsigned changed = 0;

    if ((snapshot.mask & GL_VIEWPORT_BIT) != 0) {
        seed_viewport(state);
        seed_depth_range(state);
        if (different4(state.viewport, snapshot.viewport)) {
            if (GLES.glViewport) {
                GLES.glViewport(snapshot.viewport[0], snapshot.viewport[1], snapshot.viewport[2], snapshot.viewport[3]);
            }
            std::memcpy(state.viewport, snapshot.viewport, sizeof(state.viewport));
            changed |= GL_VIEWPORT_BIT;
        }
        if (state.depth_range[0] != snapshot.depth_range[0] || state.depth_range[1] != snapshot.depth_range[1]) {
            if (GLES.glDepthRangef) GLES.glDepthRangef(snapshot.depth_range[0], snapshot.depth_range[1]);
            std::memcpy(state.depth_range, snapshot.depth_range, sizeof(state.depth_range));
            changed |= GL_VIEWPORT_BIT;
        }
    }

    if ((snapshot.mask & GL_SCISSOR_BIT) != 0) {
        seed_scissor(state);
        if (different4(state.scissor, snapshot.scissor)) {
            if (GLES.glScissor) {
                GLES.glScissor(snapshot.scissor[0], snapshot.scissor[1], snapshot.scissor[2], snapshot.scissor[3]);
            }
            std::memcpy(state.scissor, snapshot.scissor, sizeof(state.scissor));
            changed |= GL_SCISSOR_BIT;
        }
    }

    const bool restore_all_enables = (snapshot.mask & GL_ENABLE_BIT) != 0;
    const bool restore_scissor_enable = !restore_all_enables && (snapshot.mask & GL_SCISSOR_BIT) != 0;
    const unsigned enable_changes =
        (restore_all_enables || restore_scissor_enable)
            ? mg_enable_restore(&snapshot.enable, restore_all_enables, restore_scissor_enable)
            : 0;
    if (enable_changes != 0) changed |= restore_all_enables ? GL_ENABLE_BIT : GL_SCISSOR_BIT;

    ++state.pop_hits;
#if defined(ZOMDROID_GL_BREADCRUMBS)
    if (trace_milestone(state.pop_hits)) {
        write_log("ZOMDROID_SERVER_ATTRIB_RESTORE mask=0x%x changed=0x%x enable_changes=%u depth=%zu "
                  "semantic_applied=1 hit=%llu",
                  snapshot.mask, changed, enable_changes, state.depth, state.pop_hits);
    }
#endif
}

extern "C" GLAPI GLAPIENTRY void glDepthRange(GLclampd near_value, GLclampd far_value) {
    LOG()
    const GLfloat near_f = static_cast<GLfloat>(std::clamp(near_value, 0.0, 1.0));
    const GLfloat far_f = static_cast<GLfloat>(std::clamp(far_value, 0.0, 1.0));
    mg_server_attrib_note_depth_range(near_f, far_f);
    if (GLES.glDepthRangef) GLES.glDepthRangef(near_f, far_f);
}

#endif // ZOMDROID_EXPERIMENTAL
