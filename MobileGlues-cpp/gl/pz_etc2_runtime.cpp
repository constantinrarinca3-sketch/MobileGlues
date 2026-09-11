// MobileGlues - gl/pz_etc2_runtime.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#include "pz_etc2_runtime.h"

#include "log.h"
#include "pz_census.h"
#include "pz_etc2.h"

#include <atomic>
#include <climits>
#include <cstddef>
#include <cstdint>

namespace {

constexpr size_t kMinBasePixels = 512u * 512u;

std::atomic<uint64_t> g_images{0};
std::atomic<uint64_t> g_subimages{0};
std::atomic<uint64_t> g_source_bytes{0};
std::atomic<uint64_t> g_compressed_bytes{0};
std::atomic<uint64_t> g_rejected_subimages{0};

bool checked_source_size(GLsizei width, GLsizei height, int stride, size_t* out) {
    if (!out || width <= 0 || height <= 0 || stride <= 0) return false;
    const size_t w = static_cast<size_t>(width);
    const size_t h = static_cast<size_t>(height);
    if (w > SIZE_MAX / h || w * h > SIZE_MAX / static_cast<size_t>(stride)) return false;
    *out = w * h * static_cast<size_t>(stride);
    return true;
}

GLenum base_format(GLint internal_format, GLenum source_format) {
    switch (internal_format) {
    case GL_RGB:
    case GL_RGB8:
        return source_format == GL_RGB ? GL_COMPRESSED_RGB8_ETC2 : 0;
    case GL_SRGB8:
        return source_format == GL_RGB ? GL_COMPRESSED_SRGB8_ETC2 : 0;
    case GL_RGBA:
    case GL_RGBA8:
        return source_format == GL_RGBA ? GL_COMPRESSED_RGBA8_ETC2_EAC : 0;
    case GL_SRGB8_ALPHA8:
        return source_format == GL_RGBA ? GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC : 0;
    default:
        return 0;
    }
}

bool format_matches(GLenum compressed, GLenum source) {
    if (compressed == GL_COMPRESSED_RGB8_ETC2 || compressed == GL_COMPRESSED_SRGB8_ETC2)
        return source == GL_RGB;
    if (compressed == GL_COMPRESSED_RGBA8_ETC2_EAC || compressed == GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC)
        return source == GL_RGBA;
    return false;
}

void report_if_needed(uint64_t uploads) {
    if (!mg_pz_census_active || (uploads != 1 && uploads % 256 != 0)) return;
    const uint64_t source = g_source_bytes.load(std::memory_order_relaxed);
    const uint64_t compressed = g_compressed_bytes.load(std::memory_order_relaxed);
    const double ratio = compressed ? static_cast<double>(source) / static_cast<double>(compressed) : 0.0;
    LOG_I("ZOMDROID_PZ_ETC2 images=%llu subimages=%llu cache_hits=%llu encodes=%llu source=%lluB "
          "compressed=%lluB ratio=%.2f rejected_updates=%llu encode_ms=%llu io_ms=%llu evicted=%llu",
          static_cast<unsigned long long>(g_images.load(std::memory_order_relaxed)),
          static_cast<unsigned long long>(g_subimages.load(std::memory_order_relaxed)),
          static_cast<unsigned long long>(mg_pz_etc2_cache_hit_count()),
          static_cast<unsigned long long>(mg_pz_etc2_encode_count()), static_cast<unsigned long long>(source),
          static_cast<unsigned long long>(compressed), ratio,
          static_cast<unsigned long long>(g_rejected_subimages.load(std::memory_order_relaxed)),
          static_cast<unsigned long long>(mg_pz_etc2_total_ms()),
          static_cast<unsigned long long>(mg_pz_etc2_io_ms()),
          static_cast<unsigned long long>(mg_pz_etc2_eviction_count()))
}

} // namespace

bool mg_pz_etc2_try_encode(GLenum target, GLint level, GLint internal_format, GLsizei width, GLsizei height,
                           GLint border, GLenum source_format, GLenum source_type, const void* pixels,
                           bool tightly_packed, GLenum existing_format, bool subimage,
                           mg_pz_etc2_upload_t* out) {
    if (!out) return false;
    *out = {};
    if (!mg_pz_etc2_active || target != GL_TEXTURE_2D || level < 0 || border != 0 || !pixels || !tightly_packed ||
        source_type != GL_UNSIGNED_BYTE || width <= 0 || height <= 0 || width > 65535 || height > 65535)
        return false;

    GLenum compressed_format = existing_format;
    if (compressed_format == 0) {
        if (level != 0 || subimage) return false;
        size_t pixels_count = 0;
        if (!checked_source_size(width, height, 1, &pixels_count) || pixels_count < kMinBasePixels) return false;
        compressed_format = base_format(internal_format, source_format);
    }
    if (!format_matches(compressed_format, source_format)) return false;

    const int stride = source_format == GL_RGBA ? 4 : 3;
    size_t source_bytes = 0;
    if (!checked_source_size(width, height, stride, &source_bytes)) return false;

    size_t compressed_bytes = 0;
    const void* blocks = mg_pz_etc2_cache_active
                             ? mg_pz_etc2_cached(compressed_format, width, height,
                                                 static_cast<const uint8_t*>(pixels), stride, &compressed_bytes)
                             : mg_pz_etc2_encode(compressed_format, width, height,
                                                 static_cast<const uint8_t*>(pixels), stride, &compressed_bytes);
    if (!blocks || compressed_bytes == 0 || compressed_bytes > static_cast<size_t>(INT_MAX)) return false;

    out->format = compressed_format;
    out->size = static_cast<GLsizei>(compressed_bytes);
    out->blocks = blocks;
    const uint64_t uploads = (subimage ? g_subimages : g_images).fetch_add(1, std::memory_order_relaxed) + 1;
    g_source_bytes.fetch_add(source_bytes, std::memory_order_relaxed);
    g_compressed_bytes.fetch_add(compressed_bytes, std::memory_order_relaxed);
    report_if_needed(uploads);
    return true;
}

void mg_pz_etc2_rejected_update(void) {
    const uint64_t rejected = g_rejected_subimages.fetch_add(1, std::memory_order_relaxed) + 1;
    if (mg_pz_census_active && (rejected == 1 || rejected % 64 == 0)) {
        LOG_I("ZOMDROID_PZ_ETC2 rejected_updates=%llu", static_cast<unsigned long long>(rejected))
    }
}
