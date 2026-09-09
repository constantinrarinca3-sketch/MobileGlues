// MobileGlues - gl/pz_census.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1.

#ifndef MOBILEGLUES_PZ_CENSUS_H
#define MOBILEGLUES_PZ_CENSUS_H

#include <GL/gl.h>
#include <cstdint>

// Diagnostic-only counters for finding Project Zomboid submission and frame
// pacing hot spots. Exactly MOBILEGLUES_PZ_CENSUS=1 enables them; 0, an absent
// variable, and every other value leave the hot-path branches off.
extern bool mg_pz_census_active;

void mg_pz_census_init(void);
void mg_pz_census_gl_call(const char* function);
void mg_pz_census_draw(bool indexed, GLenum mode, GLsizei count, GLsizei instances);
void mg_pz_census_multidraw(GLsizei commands);
void mg_pz_census_use_program(bool redundant);
void mg_pz_census_bind_texture(bool redundant);
void mg_pz_census_active_texture(bool redundant);
void mg_pz_census_bind_buffer(bool same_frontend_binding);
void mg_pz_census_bind_vao(bool same_frontend_binding);
void mg_pz_census_bind_framebuffer(bool same_effective_binding);
void mg_pz_census_enable(bool redundant);
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

#endif // MOBILEGLUES_PZ_CENSUS_H
