#include "pz_static_sequence_capture.h"

#include "buffer.h"
#include "mg.h"
#include "pz_static_sequence_census.h"
#include "texture.h"

#include <cstdint>

namespace {

void capture_buffer_identity(GLuint buffer, bool* known, uint64_t* lifetime, uint64_t* version, uint64_t* size) {
    *known = false;
    *lifetime = 0;
    *version = 0;
    *size = 0;
    if (buffer == 0) return;

    uint64_t tracked_lifetime = 0;
    uint64_t tracked_version = 0;
    GLsizeiptr tracked_size = 0;
    if (!mg_pz_buffer_cache_identity(buffer, &tracked_lifetime, &tracked_version, &tracked_size)) return;

    *known = true;
    *lifetime = tracked_lifetime;
    *version = tracked_version;
    *size = tracked_size > 0 ? static_cast<uint64_t>(tracked_size) : 0;
}

void capture_tracked_state(mg_pz_static_draw_t* draw) {
    draw->program = gl_state != nullptr ? gl_state->current_program : 0;
    draw->vao = find_bound_array();
    draw->array_buffer = find_bound_buffer_by_target(GL_ARRAY_BUFFER);
    draw->element_buffer = draw->indexed ? find_bound_buffer_by_target(GL_ELEMENT_ARRAY_BUFFER) : 0;
    draw->draw_framebuffer = gl_state != nullptr ? gl_state->current_draw_fbo : 0;

    for (unsigned unit = 0; unit < MG_PZ_STATIC_TEXTURE_UNITS; ++unit) {
        GLuint texture = 0;
        if (mg_driver_texture_binding_at_unit(static_cast<int>(unit), GL_TEXTURE_2D, &texture)) {
            draw->texture_2d[unit] = texture;
            draw->texture_known_mask |= static_cast<uint8_t>(1U << unit);
        }
        texture = 0;
        if (mg_driver_texture_binding_at_unit(static_cast<int>(unit), GL_TEXTURE_2D_ARRAY, &texture)) {
            draw->texture_2d_array[unit] = texture;
            draw->texture_known_mask |= static_cast<uint8_t>(1U << (MG_PZ_STATIC_TEXTURE_UNITS + unit));
        }
    }

    capture_buffer_identity(draw->array_buffer, &draw->array_content_known, &draw->array_lifetime,
                            &draw->array_version, &draw->array_size);
    if (draw->indexed) {
        capture_buffer_identity(draw->element_buffer, &draw->element_content_known, &draw->element_lifetime,
                                &draw->element_version, &draw->element_size);
    }
}

} // namespace

void mg_pz_static_sequence_capture_arrays(GLenum mode, GLint first, GLsizei count, GLsizei instances,
                                          GLuint baseinstance) {
#if defined(ZOMDROID_EXPERIMENTAL)
    if (!mg_pz_static_sequence_census_active) return;
    mg_pz_static_draw_t draw{};
    draw.indexed = false;
    draw.mode = mode;
    draw.first = first;
    draw.count = count;
    draw.instances = instances;
    draw.baseinstance = baseinstance;
    capture_tracked_state(&draw);
    mg_pz_static_sequence_draw(draw);
#else
    (void)mode;
    (void)first;
    (void)count;
    (void)instances;
    (void)baseinstance;
#endif
}

void mg_pz_static_sequence_capture_elements(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                            GLsizei instances, GLint basevertex, GLuint baseinstance) {
#if defined(ZOMDROID_EXPERIMENTAL)
    if (!mg_pz_static_sequence_census_active) return;
    mg_pz_static_draw_t draw{};
    draw.indexed = true;
    draw.mode = mode;
    draw.count = count;
    draw.instances = instances;
    draw.type = type;
    draw.basevertex = basevertex;
    draw.baseinstance = baseinstance;
    draw.index_token = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(indices));
    capture_tracked_state(&draw);
    mg_pz_static_sequence_draw(draw);
#else
    (void)mode;
    (void)count;
    (void)type;
    (void)indices;
    (void)instances;
    (void)basevertex;
    (void)baseinstance;
#endif
}
