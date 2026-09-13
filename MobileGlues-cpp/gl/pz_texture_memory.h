// MobileGlues - gl/pz_texture_memory.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1.

#ifndef MOBILEGLUES_PZ_TEXTURE_MEMORY_H
#define MOBILEGLUES_PZ_TEXTURE_MEMORY_H

#include <GL/gl.h>
#include <cstddef>

struct mg_pz_texture_memory_upload_t {
    const void* pixels = nullptr;
    GLsizei width = 0;
    GLsizei height = 0;
    size_t source_bytes = 0;
    size_t output_bytes = 0;
};

// Mode 0 is off. Low keeps one fewer mip for 1024+ textures and two fewer
// mips for 4096+ textures. Ultra moves the two-mip threshold down to 2048.
int mg_pz_texture_memory_pick_shift(GLsizei width, GLsizei height);

// Builds a tightly packed, alpha-aware box-filtered RGB8/RGBA8 image in
// thread-local storage. The returned pointer remains valid until the next call
// on this thread.
bool mg_pz_texture_memory_downsample(GLsizei width, GLsizei height, GLenum format, GLenum type,
                                     const void* pixels, int shift, mg_pz_texture_memory_upload_t* out);

void mg_pz_texture_memory_record_image(const mg_pz_texture_memory_upload_t& upload, int shift);
void mg_pz_texture_memory_record_dropped_level(void);
void mg_pz_texture_memory_record_rejected_update(void);

#endif // MOBILEGLUES_PZ_TEXTURE_MEMORY_H
