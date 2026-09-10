// Contract checks for learned runtime-mipmap skipping without a GLES driver.
#include "gl/pz_census.h"
#include "gl/texture.h"

#include <cstdio>

bool mg_pz_census_active = false;
bool mg_pz_vao_fastpath_active = false;
bool mg_pz_attrib_fastpath_active = false;
bool mg_pz_uniform_fastpath_active = false;
bool mg_pz_buffer_streaming_active = false;
bool mg_pz_buffer_discard_coalesce_active = false;
bool mg_pz_state_shadow_active = false;
bool mg_pz_runtime_mipmap_skip_active = true;
bool mg_pz_quad_index_cache_active = false;
bool mg_pz_threaded_present_active = false;

static int failures = 0;

extern "C" void write_log(const char*, ...) {}
extern "C" void write_log_n(const char*, ...) {}

bool mg_test_runtime_mipmap_prepare(TextureObject* texture, GLenum target);
GLint mg_test_runtime_mipmap_min_filter(TextureObject* texture, GLenum target, GLenum pname, GLint param);
TextureObject* GetOrCreateTextureObject(GLuint index);
void mg_texture_bind_context(unsigned long long ctx_id, unsigned long long group_id);
void mg_texture_forget_context(unsigned long long ctx_id);

static void expect(bool condition, const char* message) {
    if (condition) return;
    std::printf("FAIL %s\n", message);
    ++failures;
}

int main() {
    TextureObject texture{};
    texture.texture = 7;
    texture.width = 1024;
    texture.height = 1024;

    expect(mg_test_runtime_mipmap_prepare(&texture, GL_TEXTURE_2D),
           "the first generation must reach the driver");
    expect(texture.runtime_mipmap_generated, "the first generation must be recorded");

    const GLint applied = mg_test_runtime_mipmap_min_filter(
        &texture, GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    expect(applied == GL_LINEAR && texture.runtime_mipmap_base_only,
           "the existing fallback must prove level-0-only sampling");
    expect(!mg_test_runtime_mipmap_prepare(&texture, GL_TEXTURE_2D),
           "later generation must be skipped after the level-0 fallback");

    TextureObject untouched{};
    untouched.texture = 8;
    untouched.width = 1024;
    untouched.height = 1024;
    expect(mg_test_runtime_mipmap_prepare(&untouched, GL_TEXTURE_2D),
           "a texture without the fallback must keep the driver path");

    TextureObject other_size = texture;
    other_size.width = 512;
    expect(mg_test_runtime_mipmap_prepare(&other_size, GL_TEXTURE_2D),
           "a texture outside the measured PZ signature must keep the driver path");

    mg_pz_runtime_mipmap_skip_active = false;
    expect(mg_test_runtime_mipmap_prepare(&texture, GL_TEXTURE_2D),
           "disabling the optimization must restore generation");

    mg_pz_runtime_mipmap_skip_active = true;
    expect(mg_test_runtime_mipmap_prepare(&texture, GL_TEXTURE_CUBE_MAP),
           "non-2D texture targets must keep the driver path");

    mg_texture_bind_context(101, 201);
    InitTextureMap(8);
    GetOrCreateTextureObject(7)->width = 777;
    expect(mgGetTexObjectByID(7) != nullptr, "context group owns its live texture object");
    mg_texture_bind_context(102, 201);
    mg_texture_bind_context(0, 0);
    mg_texture_forget_context(101);
    mg_texture_bind_context(102, 201);
    expect(mgGetTexObjectByID(7) != nullptr, "shared texture table survives a sibling context");
    mg_texture_bind_context(0, 0);
    mg_texture_forget_context(102);
    mg_texture_bind_context(103, 201);
    expect(mgGetTexObjectByID(7) == nullptr,
           "last context teardown releases the shared texture object table");
    mg_texture_bind_context(0, 0);
    mg_texture_forget_context(103);

    std::printf("%s (%d failures)\n", failures ? "FAILED" : "runtime mipmap checks passed", failures);
    return failures != 0;
}
