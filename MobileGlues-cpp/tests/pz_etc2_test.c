// MobileGlues - tests/pz_etc2_test.c
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1.

#include "../gl/pz_etc2.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { kWidth = 68, kHeight = 68, kStride = 4 };

int main(void) {
    char cache_dir[] = "/tmp/mg-etc2-XXXXXX";
    assert(mkdtemp(cache_dir) != NULL);
    assert(setenv("MOBILEGLUES_PZ_ETC2_CACHE_DIR", cache_dir, 1) == 0);
    assert(setenv("MOBILEGLUES_PZ_ETC2_THREADS", "1", 1) == 0);

    uint8_t* pixels = malloc((size_t)kWidth * kHeight * kStride);
    assert(pixels != NULL);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            uint8_t* p = pixels + ((size_t)y * kWidth + x) * kStride;
            p[0] = (uint8_t)(x * 3);
            p[1] = (uint8_t)(y * 5);
            p[2] = (uint8_t)(x ^ y);
            p[3] = (uint8_t)(255 - (x + y));
        }
    }

    size_t first_size = 0;
    const void* first = mg_pz_etc2_cached(0x9278u, kWidth, kHeight, pixels, kStride, &first_size);
    assert(first != NULL);
    assert(first_size == (size_t)((kWidth + 3) / 4) * ((kHeight + 3) / 4) * 16);
    uint8_t* saved = malloc(first_size);
    assert(saved != NULL);
    memcpy(saved, first, first_size);
    assert(mg_pz_etc2_encode_count() == 1);
    assert(mg_pz_etc2_cache_hit_count() == 0);

    size_t second_size = 0;
    const void* second = mg_pz_etc2_cached(0x9278u, kWidth, kHeight, pixels, kStride, &second_size);
    assert(second != NULL && second_size == first_size);
    assert(memcmp(saved, second, first_size) == 0);
    assert(mg_pz_etc2_encode_count() == 1);
    assert(mg_pz_etc2_cache_hit_count() == 1);

    assert(mg_pz_etc2_encode(0x9278u, 0, kHeight, pixels, kStride, &second_size) == NULL);
    uint8_t rgb[5 * 7 * 3];
    memset(rgb, 127, sizeof(rgb));
    const void* rgb_blocks = mg_pz_etc2_encode(0x9274u, 5, 7, rgb, 3, &second_size);
    assert(rgb_blocks != NULL && second_size == 2u * 2u * 8u);
    free(saved);
    free(pixels);

    char command[256];
    snprintf(command, sizeof(command), "rm -rf -- '%s'", cache_dir);
    assert(system(command) == 0);
    puts("pz_etc2_test: PASS");
    return 0;
}
