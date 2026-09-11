// MobileGlues - gl/pz_etc2.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#ifndef MOBILEGLUES_PZ_ETC2_H
#define MOBILEGLUES_PZ_ETC2_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Encode tightly packed RGB8/RGBA8 pixels into ETC2 blocks. The returned
// thread-local buffer remains valid until the next encoder call on this thread.
const void* mg_pz_etc2_encode(uint32_t etc2fmt, int32_t width, int32_t height, const uint8_t* pixels, int stride,
                              size_t* out_size);

// The cached form uses a content-addressed disk entry and falls back to the
// encoder when the cache is unavailable or misses.
const void* mg_pz_etc2_cached(uint32_t etc2fmt, int32_t width, int32_t height, const uint8_t* pixels, int stride,
                              size_t* out_size);

uint64_t mg_pz_etc2_total_ms(void);
uint64_t mg_pz_etc2_io_ms(void);
uint64_t mg_pz_etc2_encode_count(void);
uint64_t mg_pz_etc2_cache_hit_count(void);
uint64_t mg_pz_etc2_eviction_count(void);

#ifdef __cplusplus
}
#endif

#endif // MOBILEGLUES_PZ_ETC2_H
