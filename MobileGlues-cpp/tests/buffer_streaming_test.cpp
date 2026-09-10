// Exercises the opt-in CPU staging decision and map state without a GLES driver.
#include "gl/buffer.h"
#include "gl/pz_census.h"

#include <cstdint>
#include <cstring>
#include <cstdio>

bool mg_pz_census_active = false;
bool mg_pz_vao_fastpath_active = false;
bool mg_pz_attrib_fastpath_active = false;
bool mg_pz_uniform_fastpath_active = false;
bool mg_pz_buffer_streaming_active = true;
bool mg_pz_buffer_discard_coalesce_active = true;
bool mg_pz_state_shadow_active = false;
bool mg_pz_runtime_mipmap_skip_active = false;
bool mg_pz_quad_index_cache_active = false;
bool mg_pz_threaded_submission_active = false;
bool mg_pz_buffer_zero_copy_active = false;
struct gles_func_t g_gles_func{};
struct gles_caps_t g_gles_caps{};

static GLenum last_error = GL_NO_ERROR;
static int failures = 0;

extern "C" void mg_set_gl_error(GLenum error) {
    if (last_error == GL_NO_ERROR) last_error = error;
}

void mg_pz_census_buffer_map(GLsizeiptr) {}
void mg_pz_census_buffer_zero_copy(bool, bool, GLsizeiptr) {}

void mg_test_record_buffer_storage(GLuint buffer, GLsizeiptr size, GLenum usage, bool immutable);
void mg_test_replace_buffer_index_shadow(GLenum target, GLuint buffer, const void* data, GLsizeiptr size);
void mg_test_patch_buffer_index_shadow(GLenum target, GLuint buffer, GLintptr offset, GLsizeiptr size,
                                       const void* data);
void* mg_test_try_staging_map(GLuint buffer, GLintptr offset, GLsizeiptr length, GLbitfield access, bool* handled);
void mg_test_cancel_staging_map(GLuint buffer);
void mg_test_complete_staging_upload(GLuint buffer);
bool mg_test_try_elide_buffer_discard(GLenum target, GLuint buffer, GLsizeiptr size, GLenum usage);
bool mg_test_buffer_discard_is_elided(GLuint buffer);
int mg_test_choose_gpu_ring_slot(const bool* retired, size_t count, size_t current);

static void expect(bool condition, const char* message) {
    if (condition) return;
    std::printf("FAIL %s\n", message);
    ++failures;
}

int main() {
    InitBufferMap(8);
    const GLuint buffer = gen_buffer();
    mg_test_record_buffer_storage(buffer, 4096, GL_STREAM_DRAW, false);

    uint64_t lifetime = 0;
    uint64_t first_version = 0;
    GLsizeiptr cache_size = 0;
    expect(mg_pz_buffer_cache_identity(buffer, &lifetime, &first_version, &cache_size),
           "a CPU-authored element buffer has a cache identity");
    expect(lifetime != 0 && first_version != 0 && cache_size == 4096,
           "cache identity components and size are valid");
    mg_test_record_buffer_storage(buffer, 4096, GL_STREAM_DRAW, false);
    uint64_t second_lifetime = 0;
    uint64_t second_version = 0;
    expect(mg_pz_buffer_cache_identity(buffer, &second_lifetime, &second_version, &cache_size),
           "rewritten buffer keeps a cache identity");
    expect(second_lifetime == lifetime && second_version != first_version,
           "rewriting storage invalidates cached index data without changing lifetime");

    const GLuint shadow_buffer = gen_buffer();
    const uint16_t initial_indices[] = {1, 2, 3, 4};
    mg_pz_quad_index_cache_active = true;
    mg_test_record_buffer_storage(shadow_buffer, sizeof(initial_indices), GL_STREAM_DRAW, false);
    expect(mg_pz_buffer_cache_identity(shadow_buffer, &lifetime, &first_version, &cache_size),
           "shadowed element buffer has a cache identity");
    mg_test_replace_buffer_index_shadow(GL_ELEMENT_ARRAY_BUFFER, shadow_buffer, initial_indices,
                                        sizeof(initial_indices));
    const void* shadow = mg_pz_buffer_cache_source(shadow_buffer, lifetime, first_version, cache_size);
    expect(shadow != nullptr && std::memcmp(shadow, initial_indices, sizeof(initial_indices)) == 0,
           "cache identity exposes the exact CPU-authored element bytes");
    const uint16_t replacement = 9;
    mg_test_patch_buffer_index_shadow(GL_ELEMENT_ARRAY_BUFFER, shadow_buffer, sizeof(uint16_t),
                                      sizeof(replacement), &replacement);
    expect(mg_pz_buffer_cache_identity(shadow_buffer, &lifetime, &second_version, &cache_size),
           "subdata update advances the shadow identity");
    shadow = mg_pz_buffer_cache_source(shadow_buffer, lifetime, second_version, cache_size);
    const uint16_t expected_indices[] = {1, 9, 3, 4};
    expect(shadow != nullptr && std::memcmp(shadow, expected_indices, sizeof(expected_indices)) == 0,
           "partial element updates patch the CPU shadow");
    expect(mg_pz_buffer_cache_source(shadow_buffer, lifetime, first_version, cache_size) == nullptr,
           "an old content version cannot read a newer CPU shadow");
    mg_pz_quad_index_cache_active = false;

    bool handled = false;
    const GLbitfield write_discard =
        GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT | GL_MAP_UNSYNCHRONIZED_BIT;
    void* pointer = mg_test_try_staging_map(buffer, 0, 4096, write_discard, &handled);
    expect(handled && pointer != nullptr, "complete write-discard map uses CPU staging");
    expect((reinterpret_cast<uintptr_t>(pointer) & 63U) == 0, "staging pointer is 64-byte aligned");
    expect(!mg_pz_buffer_cache_identity(buffer, &lifetime, &first_version, &cache_size),
           "mapped buffers cannot supply reusable cached data");

    handled = false;
    pointer = mg_test_try_staging_map(buffer, 0, 4096, write_discard, &handled);
    expect(handled && pointer == nullptr, "a second map of the same buffer is rejected");
    expect(last_error == GL_INVALID_OPERATION, "double map reports GL_INVALID_OPERATION");
    mg_test_cancel_staging_map(buffer);
    expect(mg_pz_buffer_cache_identity(buffer, &lifetime, &first_version, &cache_size),
           "cancelling a staged map restores cache eligibility");

    const GLbitfield write_full_range =
        GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_UNSYNCHRONIZED_BIT;
    handled = false;
    pointer = mg_test_try_staging_map(buffer, 0, 4096, write_full_range, &handled);
    expect(handled && pointer != nullptr, "a full-buffer invalidated range uses CPU staging");
    mg_test_complete_staging_upload(buffer);

    expect(mg_test_try_elide_buffer_discard(GL_ARRAY_BUFFER, buffer, 4096, GL_STREAM_DRAW),
           "learned same-size array-buffer discard is coalesced");
    expect(mg_test_buffer_discard_is_elided(buffer), "coalesced discard remains paired with the staged upload");
    expect(!mg_test_try_elide_buffer_discard(GL_UNIFORM_BUFFER, buffer, 4096, GL_STREAM_DRAW),
           "non-vertex buffer targets stay on the direct path");
    expect(!mg_test_try_elide_buffer_discard(GL_ARRAY_BUFFER, buffer, 2048, GL_STREAM_DRAW),
           "size changes stay on the direct path");
    expect(!mg_test_try_elide_buffer_discard(GL_ARRAY_BUFFER, buffer, 4096, GL_DYNAMIC_DRAW),
           "usage changes stay on the direct path");

    bool retired[4] = {false, false, false, false};
    expect(mg_test_choose_gpu_ring_slot(retired, 1, 0) == 1,
           "a busy persistent backing allocates the next ring slot");
    retired[0] = true;
    expect(mg_test_choose_gpu_ring_slot(retired, 4, 3) == 0,
           "a retired persistent backing is reused before waiting");
    retired[0] = false;
    expect(mg_test_choose_gpu_ring_slot(retired, 4, 3) == -1,
           "a full busy ring requests the correctness fallback");

    handled = true;
    pointer = mg_test_try_staging_map(buffer, 16, 4080, write_discard, &handled);
    expect(!handled && pointer == nullptr, "partial maps stay on the driver path");

    handled = true;
    pointer = mg_test_try_staging_map(buffer, 0, 4096, GL_MAP_READ_BIT, &handled);
    expect(!handled && pointer == nullptr, "read maps stay on the driver path");

    mg_test_record_buffer_storage(buffer, 4096, GL_STATIC_DRAW, true);
    expect(!mg_pz_buffer_cache_identity(buffer, &lifetime, &first_version, &cache_size),
           "immutable storage is excluded because coherent writes have no observable boundary");
    handled = true;
    pointer = mg_test_try_staging_map(buffer, 0, 4096, write_discard, &handled);
    expect(!handled && pointer == nullptr, "immutable storage stays on the driver path");

    mg_pz_buffer_streaming_active = false;
    mg_test_record_buffer_storage(buffer, 4096, GL_STREAM_DRAW, false);
    handled = true;
    pointer = mg_test_try_staging_map(buffer, 0, 4096, write_discard, &handled);
    expect(!handled && pointer == nullptr, "disabled streaming keeps the driver path");

    std::printf("%s (%d failures)\n", failures ? "FAILED" : "buffer streaming checks passed", failures);
    return failures != 0;
}
