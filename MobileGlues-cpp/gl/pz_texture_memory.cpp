// MobileGlues - gl/pz_texture_memory.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1.

#include "pz_texture_memory.h"

#include "log.h"
#include "pz_census.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <vector>

namespace {

std::vector<uint8_t>& scratch() {
    static thread_local std::vector<uint8_t> value;
    return value;
}

std::atomic<uint64_t> g_images{0};
std::atomic<uint64_t> g_source_bytes{0};
std::atomic<uint64_t> g_output_bytes{0};
std::atomic<uint64_t> g_dropped_levels{0};
std::atomic<uint64_t> g_rejected_updates{0};

bool checked_bytes(GLsizei width, GLsizei height, int channels, size_t* out) {
    if (!out || width <= 0 || height <= 0 || channels <= 0) return false;
    const size_t w = static_cast<size_t>(width);
    const size_t h = static_cast<size_t>(height);
    if (w > SIZE_MAX / h || w * h > SIZE_MAX / static_cast<size_t>(channels)) return false;
    *out = w * h * static_cast<size_t>(channels);
    return true;
}

void report(uint64_t images) {
    if (!mg_pz_census_active || (images != 1 && images % 32 != 0)) return;
    const uint64_t source = g_source_bytes.load(std::memory_order_relaxed);
    const uint64_t output = g_output_bytes.load(std::memory_order_relaxed);
    LOG_I("ZOMDROID_PZ_TEXTURE_MEMORY mode=%d images=%llu source=%lluB output=%lluB saved=%lluB "
          "dropped_levels=%llu rejected_updates=%llu",
          mg_pz_texture_memory_mode, static_cast<unsigned long long>(images),
          static_cast<unsigned long long>(source), static_cast<unsigned long long>(output),
          static_cast<unsigned long long>(source >= output ? source - output : 0),
          static_cast<unsigned long long>(g_dropped_levels.load(std::memory_order_relaxed)),
          static_cast<unsigned long long>(g_rejected_updates.load(std::memory_order_relaxed)))
}

} // namespace

int mg_pz_texture_memory_pick_shift(GLsizei width, GLsizei height) {
    if (mg_pz_texture_memory_mode <= 0 || width <= 0 || height <= 0) return 0;
    const GLsizei side = std::max(width, height);
    if (side < 1024) return 0;
    const GLsizei deep_min = mg_pz_texture_memory_mode >= 2 ? 2048 : 4096;
    return side >= deep_min ? 2 : 1;
}

bool mg_pz_texture_memory_downsample(GLsizei width, GLsizei height, GLenum format, GLenum type,
                                     const void* pixels, int shift, mg_pz_texture_memory_upload_t* out) {
    if (!out) return false;
    *out = {};
    if (!pixels || type != GL_UNSIGNED_BYTE || (format != GL_RGB && format != GL_RGBA) ||
        shift < 1 || shift > 2 || width <= 0 || height <= 0)
        return false;

    const int channels = format == GL_RGBA ? 4 : 3;
    const int scale = 1 << shift;
    const GLsizei out_width = std::max<GLsizei>(1, width / scale);
    const GLsizei out_height = std::max<GLsizei>(1, height / scale);
    size_t source_bytes = 0;
    size_t output_bytes = 0;
    if (!checked_bytes(width, height, channels, &source_bytes) ||
        !checked_bytes(out_width, out_height, channels, &output_bytes))
        return false;

    try {
        scratch().resize(output_bytes);
    } catch (...) {
        return false;
    }

    const auto* src = static_cast<const uint8_t*>(pixels);
    auto* dst = scratch().data();
    for (GLsizei oy = 0; oy < out_height; ++oy) {
        const GLsizei sy0 = oy * scale;
        const GLsizei sy1 = std::min<GLsizei>(height, sy0 + scale);
        for (GLsizei ox = 0; ox < out_width; ++ox) {
            const GLsizei sx0 = ox * scale;
            const GLsizei sx1 = std::min<GLsizei>(width, sx0 + scale);
            const uint32_t samples = static_cast<uint32_t>((sx1 - sx0) * (sy1 - sy0));
            uint32_t sum[4] = {0, 0, 0, 0};
            uint32_t weighted[3] = {0, 0, 0};
            for (GLsizei sy = sy0; sy < sy1; ++sy) {
                for (GLsizei sx = sx0; sx < sx1; ++sx) {
                    const uint8_t* p = src + (static_cast<size_t>(sy) * width + sx) * channels;
                    if (channels == 4) {
                        const uint32_t alpha = p[3];
                        weighted[0] += p[0] * alpha;
                        weighted[1] += p[1] * alpha;
                        weighted[2] += p[2] * alpha;
                        sum[3] += alpha;
                    } else {
                        sum[0] += p[0];
                        sum[1] += p[1];
                        sum[2] += p[2];
                    }
                }
            }
            if (channels == 4) {
                if (sum[3] != 0) {
                    dst[0] = static_cast<uint8_t>((weighted[0] + sum[3] / 2) / sum[3]);
                    dst[1] = static_cast<uint8_t>((weighted[1] + sum[3] / 2) / sum[3]);
                    dst[2] = static_cast<uint8_t>((weighted[2] + sum[3] / 2) / sum[3]);
                } else {
                    dst[0] = dst[1] = dst[2] = 0;
                }
                dst[3] = static_cast<uint8_t>((sum[3] + samples / 2) / samples);
            } else {
                dst[0] = static_cast<uint8_t>((sum[0] + samples / 2) / samples);
                dst[1] = static_cast<uint8_t>((sum[1] + samples / 2) / samples);
                dst[2] = static_cast<uint8_t>((sum[2] + samples / 2) / samples);
            }
            dst += channels;
        }
    }

    out->pixels = scratch().data();
    out->width = out_width;
    out->height = out_height;
    out->source_bytes = source_bytes;
    out->output_bytes = output_bytes;
    return true;
}

void mg_pz_texture_memory_record_image(const mg_pz_texture_memory_upload_t& upload, int shift) {
    (void)shift;
    const uint64_t images = g_images.fetch_add(1, std::memory_order_relaxed) + 1;
    g_source_bytes.fetch_add(upload.source_bytes, std::memory_order_relaxed);
    g_output_bytes.fetch_add(upload.output_bytes, std::memory_order_relaxed);
    report(images);
}

void mg_pz_texture_memory_record_dropped_level(void) {
    g_dropped_levels.fetch_add(1, std::memory_order_relaxed);
}

void mg_pz_texture_memory_record_rejected_update(void) {
    g_rejected_updates.fetch_add(1, std::memory_order_relaxed);
}
