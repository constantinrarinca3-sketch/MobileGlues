// MobileGlues - gl/pz_etc2_runtime.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#ifndef MOBILEGLUES_PZ_ETC2_RUNTIME_H
#define MOBILEGLUES_PZ_ETC2_RUNTIME_H

#include <GL/gl.h>

struct mg_pz_etc2_upload_t {
    GLenum format = 0;
    GLsizei size = 0;
    const void* blocks = nullptr;
};

// existing_format is zero for a base-level definition and the ETC2 format
// selected for level zero when encoding a mip or subimage.
bool mg_pz_etc2_try_encode(GLenum target, GLint level, GLint internal_format, GLsizei width, GLsizei height,
                           GLint border, GLenum source_format, GLenum source_type, const void* pixels,
                           bool tightly_packed, GLenum existing_format, bool subimage,
                           mg_pz_etc2_upload_t* out);

// Records an update which could not legally be expressed as ETC2 blocks.
void mg_pz_etc2_rejected_update(void);

#endif // MOBILEGLUES_PZ_ETC2_RUNTIME_H
