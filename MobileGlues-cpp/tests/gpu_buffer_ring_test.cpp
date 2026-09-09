// Checks the three-frame retirement policy used by the dynamic GPU buffer ring.
#include "gl/pz_census.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>

bool mg_pz_census_active = false;
bool mg_pz_vao_fastpath_active = false;
bool mg_pz_attrib_fastpath_active = false;
bool mg_pz_uniform_fastpath_active = false;
bool mg_pz_buffer_streaming_active = true;
bool mg_pz_gpu_buffer_ring_active = true;
bool mg_pz_state_shadow_active = false;
bool mg_pz_runtime_mipmap_skip_active = false;

int mg_test_choose_gpu_ring_slot(const uint64_t* last_uploads, size_t count, size_t current, uint64_t frame,
                                 bool primed);

static int failures = 0;

static void expect(bool condition, const char* message) {
    if (condition) return;
    std::printf("FAIL %s\n", message);
    ++failures;
}

int main() {
    uint64_t uploads[3] = {8, 0, 0};
    expect(mg_test_choose_gpu_ring_slot(uploads, 1, 0, 10, false) == -2,
           "an unprimed buffer must upload into its existing backing");
    expect(mg_test_choose_gpu_ring_slot(uploads, 1, 0, 10, true) == 1,
           "a hot second upload must allocate the next backing");

    uploads[1] = 9;
    uploads[2] = 10;
    expect(mg_test_choose_gpu_ring_slot(uploads, 3, 2, 10, true) == -1,
           "three recent backings must fall back without waiting");
    expect(mg_test_choose_gpu_ring_slot(uploads, 3, 2, 11, true) == 0,
           "a backing may be reused after three presented frames");
    expect(mg_test_choose_gpu_ring_slot(uploads, 3, 2, 13, true) == -2,
           "an aged current backing needs no rotation");

    std::printf("%s (%d failures)\n", failures ? "FAILED" : "GPU buffer ring checks passed", failures);
    return failures != 0;
}
