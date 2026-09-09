// Exercises the opt-in CPU staging decision and map state without a GLES driver.
#include "gl/buffer.h"
#include "gl/pz_census.h"

#include <cstdint>
#include <cstdio>

bool mg_pz_census_active = false;
bool mg_pz_vao_fastpath_active = false;
bool mg_pz_attrib_fastpath_active = false;
bool mg_pz_uniform_fastpath_active = false;
bool mg_pz_buffer_streaming_active = true;

static GLenum last_error = GL_NO_ERROR;
static int failures = 0;

extern "C" void mg_set_gl_error(GLenum error) {
    if (last_error == GL_NO_ERROR) last_error = error;
}

void mg_pz_census_buffer_map(GLsizeiptr) {}

void mg_test_record_buffer_storage(GLuint buffer, GLsizeiptr size, GLenum usage, bool immutable);
void* mg_test_try_staging_map(GLuint buffer, GLintptr offset, GLsizeiptr length, GLbitfield access, bool* handled);
void mg_test_cancel_staging_map(GLuint buffer);

static void expect(bool condition, const char* message) {
    if (condition) return;
    std::printf("FAIL %s\n", message);
    ++failures;
}

int main() {
    InitBufferMap(8);
    const GLuint buffer = gen_buffer();
    mg_test_record_buffer_storage(buffer, 4096, GL_STREAM_DRAW, false);

    bool handled = false;
    const GLbitfield write_discard =
        GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT | GL_MAP_UNSYNCHRONIZED_BIT;
    void* pointer = mg_test_try_staging_map(buffer, 0, 4096, write_discard, &handled);
    expect(handled && pointer != nullptr, "complete write-discard map uses CPU staging");
    expect((reinterpret_cast<uintptr_t>(pointer) & 63U) == 0, "staging pointer is 64-byte aligned");

    handled = false;
    pointer = mg_test_try_staging_map(buffer, 0, 4096, write_discard, &handled);
    expect(handled && pointer == nullptr, "a second map of the same buffer is rejected");
    expect(last_error == GL_INVALID_OPERATION, "double map reports GL_INVALID_OPERATION");
    mg_test_cancel_staging_map(buffer);

    handled = true;
    pointer = mg_test_try_staging_map(buffer, 16, 4080, write_discard, &handled);
    expect(!handled && pointer == nullptr, "partial maps stay on the driver path");

    handled = true;
    pointer = mg_test_try_staging_map(buffer, 0, 4096, GL_MAP_READ_BIT, &handled);
    expect(!handled && pointer == nullptr, "read maps stay on the driver path");

    mg_test_record_buffer_storage(buffer, 4096, GL_STATIC_DRAW, true);
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
