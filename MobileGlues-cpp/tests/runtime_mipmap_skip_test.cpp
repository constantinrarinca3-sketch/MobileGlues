// Contract checks for learned runtime-mipmap skipping without a GLES driver.
#include "gl/pz_census.h"
#include "gl/texture.h"
#include "gl/mg.h"
#include "gles/loader.h"

#include <array>
#include <cstdio>
#include <limits>

bool mg_pz_census_active = false;
bool mg_pz_vao_fastpath_active = false;
bool mg_pz_attrib_fastpath_active = false;
bool mg_pz_uniform_fastpath_active = false;
bool mg_pz_buffer_streaming_active = false;
bool mg_pz_buffer_discard_coalesce_active = false;
bool mg_pz_state_shadow_active = false;
bool mg_pz_runtime_mipmap_skip_active = true;
bool mg_pz_quad_index_cache_active = false;
bool mg_pz_threaded_submission_active = false;

gles_func_t g_gles_func{};
gles_caps_t g_gles_caps{};
gl_state_s g_default_gl_state{};
thread_local gl_state_t gl_state = &g_default_gl_state;

namespace {
int backend_active_unit = 0;
std::array<GLuint, MG_TEXTURE_ATTRIB_UNIT_LIMIT> backend_bindings{};

void fake_active_texture(GLenum texture) { backend_active_unit = static_cast<int>(texture - GL_TEXTURE0); }
void fake_bind_texture(GLenum target, GLuint texture) {
    if (target == GL_TEXTURE_2D) backend_bindings[backend_active_unit] = texture;
}
} // namespace

static int failures = 0;

extern "C" void write_log(const char*, ...) {}
extern "C" void write_log_n(const char*, ...) {}

bool mg_test_runtime_mipmap_prepare(TextureObject* texture, GLenum target);
GLint mg_test_runtime_mipmap_min_filter(TextureObject* texture, GLenum target, GLenum pname, GLint param);
void mg_test_runtime_mipmap_reset(TextureObject* texture);
TextureObject* GetOrCreateTextureObject(GLuint index);
void mg_texture_bind_context(unsigned long long ctx_id, unsigned long long group_id);
void mg_texture_forget_context(unsigned long long ctx_id);

static void expect(bool condition, const char* message) {
    if (condition) return;
    std::printf("FAIL %s\n", message);
    ++failures;
}

int main() {
    GLES.glActiveTexture = fake_active_texture;
    GLES.glBindTexture = fake_bind_texture;

    expect(GetOrCreateTextureObject(std::numeric_limits<GLuint>::max()) == nullptr,
           "the Java no-texture sentinel must never index the texture object table");

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

    mg_test_runtime_mipmap_reset(&texture);
    expect(!texture.runtime_mipmap_generated && !texture.runtime_mipmap_base_only &&
               !texture.runtime_mipmap_fallback_logged,
           "redefining texture storage must clear learned mipmap state");
    expect(mg_test_runtime_mipmap_prepare(&texture, GL_TEXTURE_2D),
           "generation after storage redefinition must reach the driver");

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

    mg_test_texture_attrib_bind_2d(1, 11);
    mg_test_texture_attrib_bind_2d(2, 22);
    mg_test_texture_attrib_set_active(0);
    mg_texture_attrib_snapshot_t attrib_snapshot;
    mg_texture_attrib_capture(&attrib_snapshot);
    mg_test_texture_attrib_bind_2d(1, 101);
    mg_test_texture_attrib_bind_2d(2, 102);
    mg_test_texture_attrib_bind_2d(3, 103);
    mg_test_texture_attrib_set_active(3);
    expect(mg_texture_attrib_restore(&attrib_snapshot) >= 4,
           "texture attrib restore reports the three bindings and active unit");
    expect(mg_test_texture_attrib_binding_2d(1) == 11 && backend_bindings[1] == 11,
           "texture attrib restore repairs unit 1 frontend and backend state");
    expect(mg_test_texture_attrib_binding_2d(2) == 22 && backend_bindings[2] == 22,
           "texture attrib restore repairs unit 2 frontend and backend state");
    expect(mg_test_texture_attrib_binding_2d(3) == 0 && backend_bindings[3] == 0,
           "texture attrib restore clears the temporary unit 3 binding");
    expect(mg_test_texture_attrib_active() == 0 && backend_active_unit == 0 && gl_state->current_tex_unit == 0,
           "texture attrib restore returns both layers to the saved active unit");

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
