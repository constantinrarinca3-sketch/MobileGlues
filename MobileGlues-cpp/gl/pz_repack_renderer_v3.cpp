// MobileGlues - gl/pz_repack_renderer_v3.cpp
// Project Zomboid experiment: preserve the renderer's shader/program safety
// metadata when threaded submission moves an EGL context to its backend worker.
//
// The v2 renderer intentionally keeps its live GL shadow thread-local. That is
// correct for execution, but programs can be compiled/linked while the context
// is still owned by the producer. context_adopt then starts a fresh worker TLS,
// making those already-linked programs look unknown and forcing miss_program.
//
// Keep the original fail-closed identity guard unchanged. This wrapper only
// hands the already-computed shader/program verdicts across context ownership.

#define mg_pz_repack_renderer_install mg_pz_repack_renderer_install_v2_impl
#define pz_repack_before_backend_command pz_repack_before_backend_command_v2_impl
#include "pz_repack_renderer.cpp"
#undef pz_repack_before_backend_command
#undef mg_pz_repack_renderer_install

#if defined(ZOMDROID_EXPERIMENTAL)

#include <mutex>

namespace {

struct program_handoff_originals_t {
    glUseProgram_PTR use_program = nullptr;
    glShaderSource_PTR shader_source = nullptr;
    glAttachShader_PTR attach_shader = nullptr;
    glDetachShader_PTR detach_shader = nullptr;
    glLinkProgram_PTR link_program = nullptr;
    glDeleteProgram_PTR delete_program = nullptr;
};

struct program_handoff_shadow_t {
    GLuint current_program = 0;
    std::unordered_map<GLuint, shader_t> shaders;
    std::unordered_map<GLuint, program_t> programs;
    unsigned long long producer_updates = 0;
    unsigned long long snapshots = 0;
    unsigned long long restores = 0;
};

program_handoff_originals_t g_handoff_orig;
program_handoff_shadow_t g_handoff_shadow;
std::mutex g_handoff_mutex;
bool g_handoff_enabled = false;

void shadow_current_program(GLuint program) {
    if (g_state.worker_context) return;
    std::lock_guard<std::mutex> lock(g_handoff_mutex);
    g_handoff_shadow.current_program = program;
    ++g_handoff_shadow.producer_updates;
}

void shadow_shader(GLuint shader) {
    if (g_state.worker_context) return;
    const auto found = g_state.shaders.find(shader);
    if (found == g_state.shaders.end()) return;
    std::lock_guard<std::mutex> lock(g_handoff_mutex);
    g_handoff_shadow.shaders[shader] = found->second;
    ++g_handoff_shadow.producer_updates;
}

void shadow_program(GLuint program) {
    if (g_state.worker_context) return;
    const auto found = g_state.programs.find(program);
    if (found == g_state.programs.end()) return;
    std::lock_guard<std::mutex> lock(g_handoff_mutex);
    g_handoff_shadow.programs[program] = found->second;
    ++g_handoff_shadow.producer_updates;
}

void snapshot_worker_program_state() {
    if (!g_state.worker_context) return;
    std::lock_guard<std::mutex> lock(g_handoff_mutex);
    g_handoff_shadow.current_program = g_state.program;
    g_handoff_shadow.shaders = g_state.shaders;
    g_handoff_shadow.programs = g_state.programs;
    ++g_handoff_shadow.snapshots;
}

void restore_worker_program_state() {
    size_t safe = 0;
    size_t unsafe = 0;
    size_t unknown = 0;
    GLuint restored_program = 0;
    size_t shader_count = 0;
    size_t program_count = 0;
    unsigned long long restores = 0;

    {
        std::lock_guard<std::mutex> lock(g_handoff_mutex);
        g_state.program = g_handoff_shadow.current_program;
        g_state.shaders = g_handoff_shadow.shaders;
        g_state.programs = g_handoff_shadow.programs;
        ++g_handoff_shadow.restores;
        restores = g_handoff_shadow.restores;
        restored_program = g_state.program;
        shader_count = g_state.shaders.size();
        program_count = g_state.programs.size();
    }

    for (const auto& entry : g_state.programs) {
        if (!entry.second.linked_known)
            ++unknown;
        else if (entry.second.linked_safe)
            ++safe;
        else
            ++unsafe;
    }

    LOG_I("ZOMDROID_PZ_REPACK_PROGRAM_HANDOFF restore=%llu current=%u shaders=%llu programs=%llu "
          "safe=%llu unsafe=%llu unknown=%llu guard=preserved",
          restores, restored_program,
          static_cast<unsigned long long>(shader_count),
          static_cast<unsigned long long>(program_count),
          static_cast<unsigned long long>(safe),
          static_cast<unsigned long long>(unsafe),
          static_cast<unsigned long long>(unknown))
}

void handoff_glUseProgram(GLuint program) {
    g_handoff_orig.use_program(program);
    shadow_current_program(program);
}

void handoff_glShaderSource(GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length) {
    g_handoff_orig.shader_source(shader, count, string, length);
    shadow_shader(shader);
}

void handoff_glAttachShader(GLuint program, GLuint shader) {
    g_handoff_orig.attach_shader(program, shader);
    shadow_program(program);
}

void handoff_glDetachShader(GLuint program, GLuint shader) {
    g_handoff_orig.detach_shader(program, shader);
    shadow_program(program);
}

void handoff_glLinkProgram(GLuint program) {
    g_handoff_orig.link_program(program);
    shadow_program(program);
}

void handoff_glDeleteProgram(GLuint program) {
    g_handoff_orig.delete_program(program);
    std::lock_guard<std::mutex> lock(g_handoff_mutex);
    g_handoff_shadow.programs.erase(program);
    if (g_handoff_shadow.current_program == program) g_handoff_shadow.current_program = 0;
    ++g_handoff_shadow.producer_updates;
}

} // namespace

namespace mg_ts {
void pz_repack_before_backend_command(backend_command_class classification) {
    if (g_handoff_enabled && classification == backend_command_class::context_release)
        snapshot_worker_program_state();

    pz_repack_before_backend_command_v2_impl(classification);

    if (g_handoff_enabled && classification == backend_command_class::context_adopt)
        restore_worker_program_state();
}
} // namespace mg_ts

void mg_pz_repack_renderer_install(void) {
    mg_pz_repack_renderer_install_v2_impl();
    if (!g_enabled) return;

    g_handoff_orig.use_program = static_cast<glUseProgram_PTR>(GLES.glUseProgram);
    g_handoff_orig.shader_source = static_cast<glShaderSource_PTR>(GLES.glShaderSource);
    g_handoff_orig.attach_shader = static_cast<glAttachShader_PTR>(GLES.glAttachShader);
    g_handoff_orig.detach_shader = static_cast<glDetachShader_PTR>(GLES.glDetachShader);
    g_handoff_orig.link_program = static_cast<glLinkProgram_PTR>(GLES.glLinkProgram);
    g_handoff_orig.delete_program = static_cast<glDeleteProgram_PTR>(GLES.glDeleteProgram);

    if (!g_handoff_orig.use_program || !g_handoff_orig.shader_source || !g_handoff_orig.attach_shader ||
        !g_handoff_orig.detach_shader || !g_handoff_orig.link_program || !g_handoff_orig.delete_program) {
        LOG_W_FORCE("ZOMDROID_PZ_REPACK_PROGRAM_HANDOFF disabled reason=program_hook_missing")
        return;
    }

    GLES.glUseProgram = handoff_glUseProgram;
    GLES.glShaderSource = handoff_glShaderSource;
    GLES.glAttachShader = handoff_glAttachShader;
    GLES.glDetachShader = handoff_glDetachShader;
    GLES.glLinkProgram = handoff_glLinkProgram;
    GLES.glDeleteProgram = handoff_glDeleteProgram;
    g_handoff_enabled = true;

    LOG_I("ZOMDROID_PZ_REPACK_PROGRAM_HANDOFF enabled=1 revision=3 mode=producer_to_worker "
          "guard=linked_known+linked_safe identity_guard=preserved")
}

#else

void mg_pz_repack_renderer_install(void) {
    mg_pz_repack_renderer_install_v2_impl();
}

#endif
