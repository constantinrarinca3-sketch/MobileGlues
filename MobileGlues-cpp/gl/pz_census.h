// MobileGlues - gl/pz_census.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1.

#ifndef MOBILEGLUES_PZ_CENSUS_H
#define MOBILEGLUES_PZ_CENSUS_H

#include <GL/gl.h>
#include <cstddef>
#include <cstdint>

// Diagnostic-only counters for finding Project Zomboid submission and frame
// pacing hot spots. Exactly MOBILEGLUES_PZ_CENSUS=1 enables them; 0, an absent
// variable, and every other value leave the hot-path branches off.
extern bool mg_pz_census_active;
// Independent optimization switch. Exactly MOBILEGLUES_PZ_VAO_FASTPATH=1
// enables it; it remains usable with the diagnostic census disabled.
extern bool mg_pz_vao_fastpath_active;
extern bool mg_pz_attrib_fastpath_active;
extern bool mg_pz_uniform_fastpath_active;
extern bool mg_pz_buffer_streaming_active;
extern bool mg_pz_state_shadow_active;

enum class mg_pz_attrib_kind : uint8_t {
    enable,
    pointer,
    divisor,
    format,
    binding,
    vertex_buffer,
    constant,
};

void mg_pz_census_init(void);
void mg_pz_census_gl_call(const char* function);
void mg_pz_census_draw(bool indexed, GLenum mode, GLsizei count, GLsizei instances,
                       bool direct_elements_candidate = false);
void mg_pz_census_batch_draw(GLuint program, GLenum mode, GLenum type, GLsizei count, GLuint element_buffer);
void mg_pz_census_multidraw(GLsizei commands);
void mg_pz_census_use_program(bool redundant);
void mg_pz_census_bind_texture(bool redundant);
void mg_pz_census_active_texture(bool redundant);
void mg_pz_census_bind_buffer(bool same_frontend_binding);
void mg_pz_census_bind_vao(bool same_frontend_binding, bool driver_confirmed, bool skipped);
void mg_pz_census_bind_framebuffer(bool same_effective_binding);
void mg_pz_census_enable(bool redundant);
bool mg_pz_uniform_call(GLuint program, GLint location, uint32_t signature, GLsizei count, const void* value,
                        size_t bytes);
void mg_pz_uniform_driver_write(GLuint program, GLint location, uint32_t signature, GLsizei count, const void* value,
                                size_t bytes);
void mg_pz_census_forget_program(GLuint program);
void mg_pz_census_context_changed(unsigned long long context_id);
void mg_pz_census_attrib(mg_pz_attrib_kind kind, bool tracked, bool exact_redundant, bool skipped = false);
void mg_pz_census_attrib_value(GLuint index, uint32_t signature, const void* value, size_t bytes);
void mg_pz_census_buffer_data(GLsizeiptr bytes, bool sub_data);
void mg_pz_census_buffer_map(GLsizeiptr bytes);
void mg_pz_census_present(bool succeeded);

#if defined(ZOMDROID_EXPERIMENTAL)
#define MG_PZ_CENSUS(call)                                                                                             \
    do {                                                                                                               \
        if (mg_pz_census_active) call;                                                                                 \
    } while (0)
#else
#define MG_PZ_CENSUS(call)                                                                                             \
    do {                                                                                                               \
    } while (0)
#endif

#if defined(ZOMDROID_EXPERIMENTAL)
#define MG_PZ_UNIFORM_STATE(call)                                                                                      \
    do {                                                                                                               \
        if (mg_pz_census_active || mg_pz_uniform_fastpath_active) call;                                                \
    } while (0)
#else
#define MG_PZ_UNIFORM_STATE(call)                                                                                      \
    do {                                                                                                               \
    } while (0)
#endif

#endif // MOBILEGLUES_PZ_CENSUS_H
