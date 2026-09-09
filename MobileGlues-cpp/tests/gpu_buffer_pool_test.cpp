// Checks selection for the non-blocking, fence-backed GPU buffer pool.
#include "gl/pz_census.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>

bool mg_pz_census_active = false;
bool mg_pz_vao_fastpath_active = false;
bool mg_pz_attrib_fastpath_active = false;
bool mg_pz_uniform_fastpath_active = false;
bool mg_pz_buffer_streaming_active = true;
bool mg_pz_buffer_discard_coalesce_active = false;
bool mg_pz_gpu_buffer_pool_active = true;
bool mg_pz_state_shadow_active = false;
bool mg_pz_runtime_mipmap_skip_active = false;

int mg_test_choose_gpu_ring_slot(const bool* retired, size_t count, size_t current);

static int failures = 0;

static void expect(bool condition, const char* message) {
    if (condition) return;
    std::printf("FAIL %s\n", message);
    ++failures;
}

int main() {
    bool retired[3] = {true, false, false};
    expect(mg_test_choose_gpu_ring_slot(retired, 1, 0) == -2,
           "a retired current backing must be reused directly");
    retired[0] = false;
    expect(mg_test_choose_gpu_ring_slot(retired, 1, 0) == 1,
           "a busy current backing must allocate the next slot");
    expect(mg_test_choose_gpu_ring_slot(retired, 3, 2) == -1,
           "three busy backings must fall back without waiting");
    retired[0] = true;
    expect(mg_test_choose_gpu_ring_slot(retired, 3, 2) == 0,
           "a signaled alternate backing must be reused");
    retired[2] = true;
    expect(mg_test_choose_gpu_ring_slot(retired, 3, 2) == -2,
           "a signaled current backing needs no rotation");

    std::printf("%s (%d failures)\n", failures ? "FAILED" : "GPU buffer pool checks passed", failures);
    return failures != 0;
}
