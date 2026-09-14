#ifndef MOBILEGLUES_PZ_STATIC_SEQUENCE_CENSUS_H
#define MOBILEGLUES_PZ_STATIC_SEQUENCE_CENSUS_H

#include <GL/gl.h>
#include <cstdint>

constexpr unsigned MG_PZ_STATIC_TEXTURE_UNITS = 4;

struct mg_pz_static_draw_t {
    bool indexed = false;
    GLenum mode = 0;
    GLsizei count = 0;
    GLsizei instances = 1;
    GLenum type = 0;
    GLint first = 0;
    GLint basevertex = 0;
    GLuint baseinstance = 0;
    uint64_t index_token = 0;
    GLuint program = 0;
    GLuint vao = 0;
    GLuint array_buffer = 0;
    GLuint element_buffer = 0;
    GLuint draw_framebuffer = 0;
    uint8_t texture_known_mask = 0;
    bool array_content_known = false;
    bool element_content_known = false;
    uint64_t array_lifetime = 0;
    uint64_t array_version = 0;
    uint64_t array_size = 0;
    uint64_t element_lifetime = 0;
    uint64_t element_version = 0;
    uint64_t element_size = 0;
    GLuint texture_2d[MG_PZ_STATIC_TEXTURE_UNITS]{};
    GLuint texture_2d_array[MG_PZ_STATIC_TEXTURE_UNITS]{};
};

extern bool mg_pz_static_sequence_census_active;

void mg_pz_static_sequence_init(void);
void mg_pz_static_sequence_ensure_initialized(void);
void mg_pz_static_sequence_context_changed(unsigned long long context_id);
void mg_pz_static_sequence_draw(const mg_pz_static_draw_t& draw);
void mg_pz_static_sequence_present(void);

#endif
