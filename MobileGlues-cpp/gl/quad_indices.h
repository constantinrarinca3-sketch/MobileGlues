// MobileGlues - gl/quad_indices.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#ifndef MOBILEGLUES_QUAD_INDICES_H
#define MOBILEGLUES_QUAD_INDICES_H

#include <cstddef>
#include <cstdint>

namespace mg_quad_detail {

inline std::size_t triangle_index_capacity(std::size_t source_count) {
    return (source_count / 4u) * 6u;
}

inline std::size_t expand_sequence(std::uint32_t* dst, std::size_t source_count, std::uint32_t first = 0) {
    const std::size_t quads = source_count / 4u;
    for (std::size_t q = 0; q < quads; ++q) {
        const std::uint32_t v = first + static_cast<std::uint32_t>(q * 4u);
        const std::size_t out = q * 6u;
        dst[out + 0] = v + 0u;
        dst[out + 1] = v + 1u;
        dst[out + 2] = v + 2u;
        dst[out + 3] = v + 0u;
        dst[out + 4] = v + 2u;
        dst[out + 5] = v + 3u;
    }
    return quads * 6u;
}

// Primitive restart starts a new primitive, so a partial quad immediately
// before the sentinel is discarded and grouping resumes after it. The restart
// index itself is unnecessary in the triangle output because every emitted
// triangle is already an independent primitive.
template <typename Source>
inline std::size_t expand_elements(std::uint32_t* dst, const Source* src, std::size_t source_count,
                                   std::int32_t base_vertex, bool restart_enabled, std::uint32_t restart_value) {
    std::uint32_t quad[4];
    std::size_t in_quad = 0;
    std::size_t out = 0;
    const std::uint32_t base = static_cast<std::uint32_t>(base_vertex);

    for (std::size_t i = 0; i < source_count; ++i) {
        const std::uint32_t value = static_cast<std::uint32_t>(src[i]);
        if (restart_enabled && value == restart_value) {
            in_quad = 0;
            continue;
        }
        quad[in_quad++] = value + base;
        if (in_quad != 4) continue;

        dst[out + 0] = quad[0];
        dst[out + 1] = quad[1];
        dst[out + 2] = quad[2];
        dst[out + 3] = quad[0];
        dst[out + 4] = quad[2];
        dst[out + 5] = quad[3];
        out += 6;
        in_quad = 0;
    }
    return out;
}

} // namespace mg_quad_detail

#endif // MOBILEGLUES_QUAD_INDICES_H
