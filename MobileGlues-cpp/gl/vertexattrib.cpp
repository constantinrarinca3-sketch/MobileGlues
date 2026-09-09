// MobileGlues - gl/vertexattrib.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#include "vertexattrib.h"

#define DEBUG 0

namespace {
template <typename... Rest>
void census_uint_attrib(GLuint index, GLuint first, Rest... rest) {
    const GLuint values[] = {first, static_cast<GLuint>(rest)...};
    mg_pz_census_attrib_value(index, 0x200U | static_cast<uint32_t>(sizeof...(rest) + 1), values, sizeof(values));
}
} // namespace

void glVertexAttribI1ui(GLuint index, GLuint x) {
    LOG()
    MG_PZ_CENSUS(census_uint_attrib(index, x));
    GLES.glVertexAttribI4ui(index, x, 0, 0, 0);
}

void glVertexAttribI2ui(GLuint index, GLuint x, GLuint y) {
    LOG()
    MG_PZ_CENSUS(census_uint_attrib(index, x, y));
    GLES.glVertexAttribI4ui(index, x, y, 0, 0);
}

void glVertexAttribI3ui(GLuint index, GLuint x, GLuint y, GLuint z) {
    LOG()
    MG_PZ_CENSUS(census_uint_attrib(index, x, y, z));
    GLES.glVertexAttribI4ui(index, x, y, z, 0);
}
