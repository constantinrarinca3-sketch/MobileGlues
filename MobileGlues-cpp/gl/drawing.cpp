// MobileGlues - gl/drawing.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#include "drawing.h"
#include "restart.h"
#include "buffer.h"
#include "enable.h"
#include "framebuffer.h"
#include "mg.h"
#include "program.h"
#include "pz_census.h"
#include "quad_indices.h"
#include "texture.h"
#include "../egl/context.h"

#include <limits>

#define DEBUG 0

GLuint bufSampelerProg;
GLuint bufSampelerLoc;
std::string bufSampelerName;

extern UnorderedMap<GLuint, bool> program_map_is_sampler_buffer_emulated;

UnorderedMap<GLuint, SamplerInfo> g_samplerCacheForSamplerBuffer;

namespace {

// The unit gl/texture.cpp parks the emulated buffer texture on. Kept in step with
// MG_TEXTURE_BUFFER_EMULATION_UNIT there and with gl/buffer.cpp's glTexBuffer,
// which borrows the same one.
const GLint kBufferTextureUnit = 15;

#if defined(ZOMDROID_GL_BREADCRUMBS)
constexpr unsigned int k_texture_trace_draws_per_program = 12;
constexpr unsigned int k_texture_trace_line_limit = 256;
thread_local UnorderedMap<GLuint, unsigned int> g_texture_trace_program_draws;
thread_local UnorderedMap<std::string, bool> g_texture_trace_seen;
thread_local unsigned int g_texture_trace_lines = 0;

bool sampler_binding_for_type(GLenum type, GLenum& target, GLenum& binding) {
    switch (type) {
    case GL_SAMPLER_2D:
    case GL_SAMPLER_2D_SHADOW:
    case GL_INT_SAMPLER_2D:
    case GL_UNSIGNED_INT_SAMPLER_2D:
        target = GL_TEXTURE_2D;
        binding = GL_TEXTURE_BINDING_2D;
        return true;
    case GL_SAMPLER_3D:
    case GL_INT_SAMPLER_3D:
    case GL_UNSIGNED_INT_SAMPLER_3D:
        target = GL_TEXTURE_3D;
        binding = GL_TEXTURE_BINDING_3D;
        return true;
    case GL_SAMPLER_CUBE:
    case GL_SAMPLER_CUBE_SHADOW:
    case GL_INT_SAMPLER_CUBE:
    case GL_UNSIGNED_INT_SAMPLER_CUBE:
        target = GL_TEXTURE_CUBE_MAP;
        binding = GL_TEXTURE_BINDING_CUBE_MAP;
        return true;
    case GL_SAMPLER_2D_ARRAY:
    case GL_SAMPLER_2D_ARRAY_SHADOW:
    case GL_INT_SAMPLER_2D_ARRAY:
    case GL_UNSIGNED_INT_SAMPLER_2D_ARRAY:
        target = GL_TEXTURE_2D_ARRAY;
        binding = GL_TEXTURE_BINDING_2D_ARRAY;
        return true;
    default:
        return false;
    }
}

void trace_texture_state_before_draw(GLuint program) {
    if (program == 0 || g_texture_trace_lines >= k_texture_trace_line_limit) return;
    unsigned int& sampled_draws = g_texture_trace_program_draws[program];
    if (sampled_draws >= k_texture_trace_draws_per_program) return;
    ++sampled_draws;

    GLint active_uniforms = 0;
    GLES.glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &active_uniforms);
    const GLint use_texture_location = GLES.glGetUniformLocation(program, "useTexture");
    GLint use_texture_value = -1;
    if (use_texture_location >= 0) GLES.glGetUniformiv(program, use_texture_location, &use_texture_value);

    // Query and restore the driver's real active unit. Restoring the shadow here
    // would make a diagnostic build alter rendering precisely when that shadow is
    // the state that has diverged and needs to be exposed.
    GLint driver_active_texture = GL_TEXTURE0;
    GLES.glGetIntegerv(GL_ACTIVE_TEXTURE, &driver_active_texture);
    const int driver_active_unit = driver_active_texture - GL_TEXTURE0;
    const int shadow_active_unit = mg_driver_active_texture_unit();

    for (GLint index = 0; index < active_uniforms && g_texture_trace_lines < k_texture_trace_line_limit; ++index) {
        GLchar name[192] = {};
        GLsizei name_length = 0;
        GLint array_size = 0;
        GLenum type = 0;
        GLES.glGetActiveUniform(program, static_cast<GLuint>(index), sizeof(name), &name_length, &array_size, &type,
                                name);

        GLenum target = 0;
        GLenum binding = 0;
        if (!sampler_binding_for_type(type, target, binding)) continue;

        const GLint location = GLES.glGetUniformLocation(program, name);
        GLint unit = -1;
        if (location >= 0) GLES.glGetUniformiv(program, location, &unit);

        GLuint shadow_texture = 0;
        const bool shadow_valid = unit >= 0 && unit < mg_max_texture_units() &&
                                  mg_driver_texture_binding_at_unit(unit, target, &shadow_texture);

        GLint driver_texture = -1;
        if (unit >= 0 && unit < mg_max_texture_units()) {
            GLES.glActiveTexture(GL_TEXTURE0 + unit);
            GLES.glGetIntegerv(binding, &driver_texture);
            GLES.glActiveTexture(driver_active_texture);
        }

        const TextureObject* texture =
            driver_texture > 0 ? mgGetTexObjectByID(static_cast<GLuint>(driver_texture)) : nullptr;
        const bool mismatch = shadow_valid && driver_texture >= 0 && shadow_texture != static_cast<GLuint>(driver_texture);
        const bool dimensions_missing = texture && (texture->width <= 0 || texture->height <= 0);
        const std::string state_key = std::to_string(program) + ":" + name + ":" + std::to_string(unit) + ":" +
                                      std::to_string(driver_texture == 0) + ":" + std::to_string(shadow_valid) + ":" +
                                      std::to_string(mismatch) + ":" + std::to_string(dimensions_missing);
        if (g_texture_trace_seen.find(state_key) != g_texture_trace_seen.end()) continue;
        g_texture_trace_seen[state_key] = true;

        ++g_texture_trace_lines;
        write_log("ZOMDROID_TEXTURE_STATE seq=%u program=%u sampler=%s loc=%d unit=%d useTexture=%d "
                  "target=0x%x app_active=%u shadow_active=%d driver_active=%d shadow_valid=%d shadow_tex=%u "
                  "driver_tex=%d tracked=%d size=%dx%d format=0x%x",
                  g_texture_trace_lines, program, name, location, unit, use_texture_value, target,
                  gl_state ? gl_state->current_tex_unit : 0, shadow_active_unit, driver_active_unit,
                  shadow_valid ? 1 : 0, shadow_texture, driver_texture, texture ? 1 : 0,
                  texture ? texture->width : 0, texture ? texture->height : 0,
                  texture ? texture->internal_format : 0);
    }
}
#else
void trace_texture_state_before_draw(GLuint) {}
#endif

// Everything the two program maps have to say about one program, held so the
// sampler list is not copied out of the map on every draw.
//
// It is not a cache of the lookups themselves. Both maps are re-probed on every
// call, because both carry this layer's invalidation points and neither can reach
// a file-local cache: glCreateProgram clears
// program_map_is_sampler_buffer_emulated[program] and erases the sampler entry --
// it exists for that, because GL hands the name of a deleted program straight back
// out -- and glAttachShader sets the flag. Remembering either answer let a recycled
// name keep the previous program's uniform locations, and glUseProgram cannot be
// the invalidation point instead: it is filtered when the name repeats, and a
// recycled name does repeat.
//
// What is kept is the copy below, valid only while the sampler entry it came from
// is still the entry the map holds for this program. Copied rather than pointed at
// across calls because g_samplerCacheForSamplerBuffer is process-wide with no lock,
// so a pointer into it would not survive a rehash or an erase performed on another
// thread.
struct resolved_program_t {
    GLuint program = 0;
    // The entry the copy below was taken from, and the only thing that says the
    // copy still describes this program. A mismatch costs one copy, never a wrong
    // answer; the map's entries are inserted by this function alone, always from
    // the live program, so a hit at the same address is the entry that was copied.
    const SamplerInfo* source = nullptr;
    bool emulated = false;
    GLint locWidth = -1;
    GLint locHeight = -1;
    std::vector<GLint> samplers;
};

// thread_local because gl_state is: two threads with different current contexts
// have different current programs and must not share this.
thread_local resolved_program_t g_resolved_program;

const resolved_program_t& resolve_program(GLuint program) {
    // find() rather than operator[]: this used to insert a default-constructed
    // entry for every program the application ever drew with, on the draw path,
    // and could rehash the map while doing it.
    const auto emu = program_map_is_sampler_buffer_emulated.find(program);
    if (emu == program_map_is_sampler_buffer_emulated.end() || !emu->second) {
        g_resolved_program.program = program;
        g_resolved_program.source = nullptr;
        g_resolved_program.emulated = false;
        g_resolved_program.locWidth = -1;
        g_resolved_program.locHeight = -1;
        g_resolved_program.samplers.clear();
        return g_resolved_program;
    }

    auto it = g_samplerCacheForSamplerBuffer.find(program);
    if (it != g_samplerCacheForSamplerBuffer.end() && g_resolved_program.program == program &&
        g_resolved_program.source == &it->second) {
        return g_resolved_program;
    }

    const SamplerInfo* info = nullptr;
    if (it != g_samplerCacheForSamplerBuffer.end()) {
        info = &it->second;
    } else {
        // Value-initialised: SamplerInfo has no default member initialisers, and
        // the entry is stored even when the program turns out not to carry the
        // emulation uniforms. The old code inserted the entry *before* that check
        // and returned without filling it in, so every later draw with the same
        // program read whatever the allocation happened to hold -- and reprobing
        // was skipped anyway because the key was present.
        SamplerInfo built{};
        built.locWidth = GLES.glGetUniformLocation(program, "u_BufferTexWidth");
        built.locHeight = GLES.glGetUniformLocation(program, "u_BufferTexHeight");
        if (built.locWidth == -1) {
            LOG_W("u_BufferTexWidth uniform not found in program %d", program);
        } else {
            GLint numUniforms = 0;
            GLES.glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &numUniforms);
            LOG_D("Program %d has %d active uniforms", program, numUniforms);

            for (GLint i = 0; i < numUniforms; ++i) {
                const GLsizei bufSize = 256;
                GLchar name[bufSize];
                GLsizei length = 0;
                GLint size = 0;
                GLenum type = 0;
                GLES.glGetActiveUniform(program, i, bufSize, &length, &size, &type, name);

                if (type == GL_SAMPLER_2D || type == GL_INT_SAMPLER_2D) {
                    built.samplers.push_back(GLES.glGetUniformLocation(program, name));
                }
            }
        }
        info = &(g_samplerCacheForSamplerBuffer[program] = std::move(built));
    }

    g_resolved_program.program = program;
    g_resolved_program.source = info;
    g_resolved_program.samplers.clear();

    // A program with no u_BufferTexWidth has nothing to receive, which is what the
    // early return used to say.
    if (info->locWidth == -1) {
        g_resolved_program.emulated = false;
        g_resolved_program.locWidth = -1;
        g_resolved_program.locHeight = -1;
        return g_resolved_program;
    }

    g_resolved_program.emulated = true;
    g_resolved_program.locWidth = info->locWidth;
    g_resolved_program.locHeight = info->locHeight;
    g_resolved_program.samplers = info->samplers;
    return g_resolved_program;
}

} // namespace

void setupBufferTextureUniforms(GLuint program) {
    LOG_D("setupBufferTextureUniforms, program: %d", program);

    const resolved_program_t& info = resolve_program(program);
    if (!info.emulated || info.samplers.empty()) return;

    // The uniform writes stay on the draw path rather than moving to glUseProgram.
    // The size uniforms describe whatever texture is parked on the emulation unit
    // *now*, and an application is entitled to bind its buffer texture after
    // glUseProgram and before the draw -- resolving them at glUseProgram time would
    // describe the previous binding. Only the map lookups above are hoisted, and
    // those depend on the program alone.
    //
    // Every sampler in the program reads that one binding, so it is resolved once
    // for the whole loop instead of once per sampler, as are the size uniforms.
    GLuint texId = 0;
    if (!mg_driver_texture_binding_at_unit(kBufferTextureUnit, GL_TEXTURE_2D, &texId)) {
        // The tracked driver-side binding is not trustworthy right now (FSR1 leaves
        // a texture bound on a unit nothing records), so pay for the round trip.
        // Borrowing the unit has to hand it back: gl/buffer.cpp's glTexBuffer and
        // gl/texture.cpp's glBindTexture both assume the emulation unit is only
        // ever active inside a window that restores it.
        const int prev_unit = mg_driver_active_texture_unit();
        GLES.glActiveTexture(GL_TEXTURE0 + kBufferTextureUnit);
        GLint queried = 0;
        GLES.glGetIntegerv(GL_TEXTURE_BINDING_2D, &queried);
        GLES.glActiveTexture(GL_TEXTURE0 + prev_unit);
        texId = static_cast<GLuint>(queried);
    }
    if (texId == 0) return;

    const TextureObject* texObject = mgGetTexObjectByID(texId);
    // mgGetTexObjectByID answers null for a name this layer has no record of. The
    // dimensions are the whole point of these uniforms, so there is nothing useful
    // to write without it.
    if (!texObject) return;

    bool wrote_sampler = false;
    for (const GLint locSampler : info.samplers) {
        if (locSampler < 0) continue;
        GLES.glUniform1i(locSampler, kBufferTextureUnit);
        MG_PZ_UNIFORM_STATE(mg_pz_uniform_driver_write(program, locSampler, 0x101U, 1, &kBufferTextureUnit,
                                                        sizeof(kBufferTextureUnit)));
        wrote_sampler = true;
    }
    if (!wrote_sampler) return;

    GLES.glUniform1i(info.locWidth, texObject->width);
    MG_PZ_UNIFORM_STATE(
        mg_pz_uniform_driver_write(program, info.locWidth, 0x101U, 1, &texObject->width, sizeof(texObject->width)));
    GLES.glUniform1i(info.locHeight, texObject->height);
    MG_PZ_UNIFORM_STATE(
        mg_pz_uniform_driver_write(program, info.locHeight, 0x101U, 1, &texObject->height, sizeof(texObject->height)));
}

void prepareForDraw() {
    LOG_D("prepareForDraw...")
#if defined(ZOMDROID_EXPERIMENTAL)
    mg_pz_gpu_buffer_ring_note_draw();
    mg_prepare_pz_alpha_test(gl_state->current_program);
#endif
    if (hardware->emulate_texture_buffer) {
        setupBufferTextureUniforms(gl_state->current_program);
    }
    trace_texture_state_before_draw(gl_state->current_program);
}

namespace {

// GLES removed GL_QUADS. Keep two context-owned scratch index buffers: the
// array path reuses one monotonically-grown 0-based sequence, while the indexed
// path uploads the application's expanded stream. They are separate so an
// indexed draw cannot evict the hot array sequence used by the sprite batcher.
thread_local GLuint g_quad_array_ibo = 0;
thread_local GLuint g_quad_elements_ibo = 0;
thread_local unsigned long long g_quad_owner_ctx_id = 0;
thread_local std::size_t g_quad_array_uploaded = 0;
thread_local std::vector<GLuint> g_quad_array_indices;
thread_local std::vector<GLuint> g_quad_element_indices;

void quad_check_context() {
    const unsigned long long current = g_current_ctx ? g_current_ctx->id : 0;
    if (current == g_quad_owner_ctx_id) return;
    g_quad_array_ibo = 0;
    g_quad_elements_ibo = 0;
    g_quad_array_uploaded = 0;
    g_quad_owner_ctx_id = current;
}

GLsizei quad_index_size(GLenum type) {
    switch (type) {
    case GL_UNSIGNED_BYTE:
        return 1;
    case GL_UNSIGNED_SHORT:
        return 2;
    case GL_UNSIGNED_INT:
        return 4;
    default:
        return 0;
    }
}

bool quad_output_count(GLsizei source_count, GLsizei* output_count) {
    if (source_count < 0) {
        mg_set_gl_error(GL_INVALID_VALUE);
        return false;
    }
    const std::size_t count = mg_quad_detail::triangle_index_capacity(static_cast<std::size_t>(source_count));
    if (count > static_cast<std::size_t>(std::numeric_limits<GLsizei>::max())) {
        mg_set_gl_error(GL_OUT_OF_MEMORY);
        return false;
    }
    *output_count = static_cast<GLsizei>(count);
    return true;
}

void trace_quad_rewrite(const char* route, GLsizei source_count, GLsizei output_count, GLenum type,
                        GLuint source_ibo) {
#if defined(ZOMDROID_GL_BREADCRUMBS)
    static thread_local unsigned int arrays_seen = 0;
    static thread_local unsigned int elements_seen = 0;
    unsigned int& seen = type == 0 ? arrays_seen : elements_seen;
    if (seen++ < 8) {
        write_log("ZOMDROID_QUADS_REWRITE route=%s source=%d triangles_indices=%d type=0x%x source_ibo=%u", route,
                  source_count, output_count, type, source_ibo);
    }
#else
    (void)route;
    (void)source_count;
    (void)output_count;
    (void)type;
    (void)source_ibo;
#endif
}

void quad_warn_once(const char* message) {
    static bool warned = false;
    if (warned) return;
    warned = true;
    LOG_W_FORCE("%s", message)
}

bool draw_arrays_as_triangles(GLint first, GLsizei count, GLsizei instancecount) {
    if (first < 0 || instancecount < -1) {
        mg_set_gl_error(GL_INVALID_VALUE);
        return true;
    }

    GLsizei output_count = 0;
    if (!quad_output_count(count, &output_count) || output_count == 0 || instancecount == 0) return true;

    quad_check_context();
    const GLuint previous_ibo = mg_driver_bound_buffer(GL_ELEMENT_ARRAY_BUFFER);
    const std::size_t wanted = static_cast<std::size_t>(output_count);
    if (g_quad_array_indices.size() < wanted) {
        g_quad_array_indices.resize(wanted);
        mg_quad_detail::expand_sequence(g_quad_array_indices.data(), static_cast<std::size_t>(count));
    }
    if (!g_quad_array_ibo) GLES.glGenBuffers(1, &g_quad_array_ibo);
    GLES.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_quad_array_ibo);
    if (g_quad_array_uploaded < wanted) {
        GLES.glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(wanted * sizeof(GLuint)),
                          g_quad_array_indices.data(), GL_STREAM_DRAW);
        g_quad_array_uploaded = wanted;
    }

    bool drew = false;
    if (first == 0) {
        if (instancecount < 0) {
            GLES.glDrawElements(GL_TRIANGLES, output_count, GL_UNSIGNED_INT, nullptr);
        } else {
            GLES.glDrawElementsInstanced(GL_TRIANGLES, output_count, GL_UNSIGNED_INT, nullptr, instancecount);
        }
        drew = true;
    } else if (instancecount < 0 && GLES.glDrawElementsBaseVertex) {
        GLES.glDrawElementsBaseVertex(GL_TRIANGLES, output_count, GL_UNSIGNED_INT, nullptr, first);
        drew = true;
    } else if (instancecount >= 0 && GLES.glDrawElementsInstancedBaseVertex) {
        GLES.glDrawElementsInstancedBaseVertex(GL_TRIANGLES, output_count, GL_UNSIGNED_INT, nullptr, instancecount,
                                               first);
        drew = true;
    }

    if (!drew) {
        // GLES 3.0/3.1 may have no base-vertex entry point. Build an absolute
        // stream only on that fallback; GLES 3.2 stays on the reusable buffer.
        g_quad_element_indices.resize(wanted);
        mg_quad_detail::expand_sequence(g_quad_element_indices.data(), static_cast<std::size_t>(count),
                                        static_cast<GLuint>(first));
        if (!g_quad_elements_ibo) GLES.glGenBuffers(1, &g_quad_elements_ibo);
        GLES.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_quad_elements_ibo);
        GLES.glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(wanted * sizeof(GLuint)),
                          g_quad_element_indices.data(), GL_STREAM_DRAW);
        if (instancecount < 0) {
            GLES.glDrawElements(GL_TRIANGLES, output_count, GL_UNSIGNED_INT, nullptr);
        } else {
            GLES.glDrawElementsInstanced(GL_TRIANGLES, output_count, GL_UNSIGNED_INT, nullptr, instancecount);
        }
    }

    GLES.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, previous_ibo);
    trace_quad_rewrite("arrays", count, output_count, 0, previous_ibo);
    CHECK_GL_ERROR
    return true;
}

void draw_unreadable_quad_elements(GLsizei count, GLenum type, const void* indices, GLint basevertex,
                                   GLsizei instancecount, GLuint source_ibo) {
    if (mg_primitive_restart_enabled()) {
        quad_warn_once("GL_QUADS: unreadable index buffer with primitive restart; draw skipped");
        return;
    }

    const GLsizei size = quad_index_size(type);
    const GLsizei quads = count / 4;
    for (GLsizei q = 0; q < quads; ++q) {
        const uintptr_t offset = reinterpret_cast<uintptr_t>(indices) + static_cast<uintptr_t>(q * 4) * size;
        const void* quad = reinterpret_cast<const void*>(offset);
        if (basevertex == 0) {
            if (instancecount < 0) {
                GLES.glDrawElements(GL_TRIANGLE_FAN, 4, type, quad);
            } else {
                GLES.glDrawElementsInstanced(GL_TRIANGLE_FAN, 4, type, quad, instancecount);
            }
        } else if (instancecount < 0 && GLES.glDrawElementsBaseVertex) {
            GLES.glDrawElementsBaseVertex(GL_TRIANGLE_FAN, 4, type, quad, basevertex);
        } else if (instancecount >= 0 && GLES.glDrawElementsInstancedBaseVertex) {
            GLES.glDrawElementsInstancedBaseVertex(GL_TRIANGLE_FAN, 4, type, quad, instancecount, basevertex);
        } else {
            quad_warn_once("GL_QUADS: unreadable index buffer needs unavailable base-vertex support; draw skipped");
            return;
        }
    }
    trace_quad_rewrite("elements-fan-fallback", count, quads * 6, type, source_ibo);
}

bool draw_elements_as_triangles(GLsizei count, GLenum type, const void* indices, GLint basevertex,
                                GLsizei instancecount) {
    if (instancecount < -1) {
        mg_set_gl_error(GL_INVALID_VALUE);
        return true;
    }
    const GLsizei size = quad_index_size(type);
    if (size == 0) {
        mg_set_gl_error(GL_INVALID_ENUM);
        return true;
    }

    GLsizei capacity = 0;
    if (!quad_output_count(count, &capacity) || capacity == 0 || instancecount == 0) return true;

    quad_check_context();
    const GLuint previous_ibo = mg_driver_bound_buffer(GL_ELEMENT_ARRAY_BUFFER);
    const void* source = indices;
    bool mapped = false;
    if (previous_ibo != 0) {
        GLES.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, previous_ibo);
        if (!GLES.glMapBufferRange) {
            draw_unreadable_quad_elements(count, type, indices, basevertex, instancecount, previous_ibo);
            return true;
        }
        source = GLES.glMapBufferRange(GL_ELEMENT_ARRAY_BUFFER,
                                       static_cast<GLintptr>(reinterpret_cast<uintptr_t>(indices)),
                                       static_cast<GLsizeiptr>(count) * size, GL_MAP_READ_BIT);
        mapped = source != nullptr;
        if (!mapped) {
            draw_unreadable_quad_elements(count, type, indices, basevertex, instancecount, previous_ibo);
            return true;
        }
    } else if (!source) {
        mg_set_gl_error(GL_INVALID_OPERATION);
        return true;
    }

    g_quad_element_indices.resize(static_cast<std::size_t>(capacity));
    const bool restart = mg_primitive_restart_enabled();
    const GLuint restart_index = mg_primitive_restart_index_for(type);
    std::size_t output = 0;
    switch (type) {
    case GL_UNSIGNED_BYTE:
        output = mg_quad_detail::expand_elements(g_quad_element_indices.data(), static_cast<const GLubyte*>(source),
                                                 static_cast<std::size_t>(count), basevertex, restart, restart_index);
        break;
    case GL_UNSIGNED_SHORT:
        output = mg_quad_detail::expand_elements(g_quad_element_indices.data(), static_cast<const GLushort*>(source),
                                                 static_cast<std::size_t>(count), basevertex, restart, restart_index);
        break;
    case GL_UNSIGNED_INT:
        output = mg_quad_detail::expand_elements(g_quad_element_indices.data(), static_cast<const GLuint*>(source),
                                                 static_cast<std::size_t>(count), basevertex, restart, restart_index);
        break;
    }
    if (mapped) GLES.glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER);
    if (output == 0) return true;

    if (!g_quad_elements_ibo) GLES.glGenBuffers(1, &g_quad_elements_ibo);
    GLES.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_quad_elements_ibo);
    GLES.glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(output * sizeof(GLuint)),
                      g_quad_element_indices.data(), GL_STREAM_DRAW);
    if (instancecount < 0) {
        GLES.glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(output), GL_UNSIGNED_INT, nullptr);
    } else {
        GLES.glDrawElementsInstanced(GL_TRIANGLES, static_cast<GLsizei>(output), GL_UNSIGNED_INT, nullptr,
                                     instancecount);
    }
    GLES.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, previous_ibo);
    trace_quad_rewrite("elements", count, static_cast<GLsizei>(output), type, previous_ibo);
    CHECK_GL_ERROR
    return true;
}

} // namespace

void glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    LOG()
    MG_PZ_CENSUS(mg_pz_census_draw(false, mode, count, 1));
    if (mode == GL_QUADS) {
        prepareForDraw();
        if (draw_arrays_as_triangles(first, count, -1)) return;
#if defined(ZOMDROID_EXPERIMENTAL)
    } else {
        mg_pz_gpu_buffer_ring_note_draw();
        // Non-quad array draws did not previously pay the buffer-texture and
        // trace preparation cost. Only the alpha semantic applies to them.
        mg_prepare_pz_alpha_test(gl_state->current_program);
#endif
    }
    GLES.glDrawArrays(mode, first, count);
    CHECK_GL_ERROR
}

void glDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instancecount) {
    LOG()
    MG_PZ_CENSUS(mg_pz_census_draw(false, mode, count, instancecount));
    if (mode == GL_QUADS) {
        prepareForDraw();
        if (draw_arrays_as_triangles(first, count, instancecount)) return;
#if defined(ZOMDROID_EXPERIMENTAL)
    } else {
        mg_pz_gpu_buffer_ring_note_draw();
        mg_prepare_pz_alpha_test(gl_state->current_program);
#endif
    }
    GLES.glDrawArraysInstanced(mode, first, count, instancecount);
    CHECK_GL_ERROR
}

void glDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei primcount) {
    LOG()
    MG_PZ_CENSUS(mg_pz_census_draw(true, mode, count, primcount));
    LOG_D("glDrawElementsInstanced, mode: %d, count: %d, type: %d, indices: %p, primcount: %d", mode, count, type,
          indices, primcount)
    prepareForDraw();
    if (mode == GL_QUADS && draw_elements_as_triangles(count, type, indices, 0, primcount)) return;
    if (mg_restart_needs_rewrite(type) && mg_draw_elements_restart(mode, count, type, indices, 0, primcount)) return;
    const bool restart_fixed = mg_restart_needs_driver_fixed(type);
    if (restart_fixed) GLES.glEnable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
    GLES.glDrawElementsInstanced(mode, count, type, indices, primcount);
    if (restart_fixed) GLES.glDisable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
    CHECK_GL_ERROR
}

void glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    LOG()
    MG_PZ_CENSUS(mg_pz_census_draw(true, mode, count, 1,
                                   mode == GL_TRIANGLES && !mg_restart_needs_rewrite(type) &&
                                       !mg_restart_needs_driver_fixed(type)));
    LOG_D("glDrawElements, mode: %d, count: %d, type: %d, indices: %p", mode, count, type, indices)
    prepareForDraw();
    if (mode == GL_QUADS && draw_elements_as_triangles(count, type, indices, 0, -1)) return;
    if (mg_restart_needs_rewrite(type) && mg_draw_elements_restart(mode, count, type, indices, 0, -1)) return;
    const bool restart_fixed = mg_restart_needs_driver_fixed(type);
    if (restart_fixed) GLES.glEnable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
    MG_PZ_CENSUS(mg_pz_census_batch_draw(gl_state->current_program, mode, type, count,
                                         mg_driver_bound_buffer(GL_ELEMENT_ARRAY_BUFFER)));
    GLES.glDrawElements(mode, count, type, indices);
    if (restart_fixed) GLES.glDisable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
    CHECK_GL_ERROR
}

void glBindImageTexture(GLuint unit, GLuint texture, GLint level, GLboolean layered, GLint layer, GLenum access,
                        GLenum format) {
    LOG()
    LOG_D("glBindImageTexture, unit: %d, texture: %d, level: %d, layered: %d, layer: %d, access: %d, format: %d", unit,
          texture, level, layered, layer, access, format)
    GLES.glBindImageTexture(unit, texture, level, layered, layer, access, format);
    CHECK_GL_ERROR
}

void glUniform1i(GLint location, GLint v0) {
    LOG()
    LOG_D("glUniform1i, location: %d, v0: %d", location, v0)
#if defined(ZOMDROID_EXPERIMENTAL)
    if ((mg_pz_census_active || mg_pz_uniform_fastpath_active) &&
        mg_pz_uniform_call(gl_state->current_program, location, 0x101U, 1, &v0, sizeof(v0)))
        return;
#endif
    GLES.glUniform1i(location, v0);
    CHECK_GL_ERROR
}

void glDispatchCompute(GLuint num_groups_x, GLuint num_groups_y, GLuint num_groups_z) {
    LOG()
    LOG_D("glDispatchCompute, num_groups_x: %d, num_groups_y: %d, num_groups_z: %d", num_groups_x, num_groups_y,
          num_groups_z)
    GLES.glDispatchCompute(num_groups_x, num_groups_y, num_groups_z);
    CHECK_GL_ERROR
}

void glMemoryBarrier(GLbitfield barriers) {
    LOG()
    LOG_D("glMemoryBarrier, barriers: %d", barriers)
    GLES.glMemoryBarrier(barriers);
    CHECK_GL_ERROR
}

namespace {

// Scratch index buffer for the base-vertex emulation below, and the context that
// owns it. Modeled on gl/restart.cpp's g_restart_ibo, including the invalidation:
// thread_local because g_current_ctx is, so two threads with different current
// contexts keep their own name instead of trading one back and forth.
thread_local GLuint g_basevertex_ibo = 0;
thread_local unsigned long long g_basevertex_owner_ctx_id = 0;

// Drop the cached name when the current context is not the one that created it.
//
// Deliberately no glDeleteBuffers: if the owning context is gone the buffer went
// with it, and if it is merely not current then this name refers to a buffer
// belonging to whichever context *is* current -- the glBufferData below would
// overwrite that buffer's contents.
void basevertex_check_context() {
    const unsigned long long cur = g_current_ctx ? g_current_ctx->id : 0;
    if (cur == g_basevertex_owner_ctx_id) return;
    g_basevertex_ibo = 0;
    g_basevertex_owner_ctx_id = cur;
}

// Staging for the rebased index stream. Elements are GLuint so the storage is
// always aligned for the widest index type it has to hold; the length is in whole
// GLuints, rounded up. Grown and never shrunk, so a steady stream of draws
// allocates nothing -- this used to be a malloc and a free per call.
thread_local std::vector<GLuint> g_basevertex_staging;

void* basevertex_staging(size_t bytes) {
    // Grown only. A plain resize() to the exact length shrinks after a small draw
    // and then value-initialises the difference on the next large one -- a memset
    // of the whole tail that the caller's memcpy overwrites immediately. Only the
    // first `bytes` bytes are ever read, so what is past them is out of range in
    // the same way it is in gl/multidraw.cpp and gl/restart.cpp.
    const size_t need = (bytes + sizeof(GLuint) - 1) / sizeof(GLuint);
    if (g_basevertex_staging.size() < need) g_basevertex_staging.resize(need);
    return g_basevertex_staging.data();
}

} // namespace

void glDrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices, GLint basevertex) {
    LOG()
    MG_PZ_CENSUS(mg_pz_census_draw(true, mode, count, 1));
    LOG_D("glDrawElementsBaseVertex, mode: %d, count: %d, type: %d, indices: %p, basevertex: %d", mode, count, type,
          indices, basevertex);
    prepareForDraw();
    if (mode == GL_QUADS && draw_elements_as_triangles(count, type, indices, basevertex, -1)) return;
    // The rewrite applies the base vertex itself, so it covers both the emulated
    // and the driver-supported branch below.
    if (mg_restart_needs_rewrite(type) && mg_draw_elements_restart(mode, count, type, indices, basevertex, -1)) return;
    const bool restart_fixed = mg_restart_needs_driver_fixed(type);
    if (restart_fixed) GLES.glEnable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
    struct RestartGuard {
        bool on;
        ~RestartGuard() {
            if (on) GLES.glDisable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
        }
    } restart_guard{restart_fixed};
    if (hardware->es_version < 320 && !g_gles_caps.GL_EXT_draw_elements_base_vertex &&
        !g_gles_caps.GL_OES_draw_elements_base_vertex) {
        // TODO: use indirect drawing for GLES 3.1
        LOG_D("Emulating glDrawElementsBaseVertex")
        if (basevertex == 0) {
            GLES.glDrawElements(mode, count, type, indices);
            return;
        }
        if (count <= 0) return;

        size_t indexSize;
        switch (type) {
        case GL_UNSIGNED_INT:
            indexSize = sizeof(GLuint);
            break;
        case GL_UNSIGNED_SHORT:
            indexSize = sizeof(GLushort);
            break;
        case GL_UNSIGNED_BYTE:
            indexSize = sizeof(GLubyte);
            break;
        default:
            return;
        }

        const size_t bytes = static_cast<size_t>(count) * indexSize;

        // The tracked binding rather than a driver round trip. It is the driver's
        // name, so it goes straight back to GLES.glBindBuffer, and it is asked for
        // before the temporary bind below, which is the only window in which this
        // function makes the two disagree. gl/gl.cpp's depth-clear triangle is the
        // one path in the layer that leaves the driver on a vertex array the
        // tracked state does not follow, and the element array binding is vertex
        // array state; see the note on the accessor in gl/buffer.cpp.
        const GLuint prevElementBuffer = mg_driver_bound_buffer(GL_ELEMENT_ARRAY_BUFFER);

        void* tempIndices = basevertex_staging(bytes);

        if (prevElementBuffer != 0) {
            // Redundant on paper -- that buffer is already bound -- but it is what
            // guarantees the map below reads the buffer this call resolved, the
            // same way gl/restart.cpp and gl/multidraw.cpp do it.
            GLES.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, prevElementBuffer);
            // Read-only, and it has to stay a map: gl/buffer.cpp tracks a buffer's
            // size but never its contents, so there is no shadow copy of the index
            // data to rebase from.
            void* srcData =
                GLES.glMapBufferRange(GL_ELEMENT_ARRAY_BUFFER,
                                      static_cast<GLintptr>(reinterpret_cast<uintptr_t>(indices)),
                                      static_cast<GLsizeiptr>(bytes), GL_MAP_READ_BIT);
            if (!srcData) {
                // An immutable or persistently mapped index buffer cannot be read
                // back, and there is no driver base vertex on this path to fall
                // back to. Drop the draw rather than place the geometry at the
                // wrong vertices.
                return;
            }
            memcpy(tempIndices, srcData, bytes);
            GLES.glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER);
        } else {
            if (!indices) return;
            memcpy(tempIndices, indices, bytes);
        }

        switch (type) {
        case GL_UNSIGNED_INT:
            for (GLsizei j = 0; j < count; ++j) {
                ((GLuint*)tempIndices)[j] += basevertex;
            }
            break;
        case GL_UNSIGNED_SHORT:
            for (GLsizei j = 0; j < count; ++j) {
                ((GLushort*)tempIndices)[j] += basevertex;
            }
            break;
        case GL_UNSIGNED_BYTE:
            for (GLsizei j = 0; j < count; ++j) {
                ((GLubyte*)tempIndices)[j] += basevertex;
            }
            break;
        }

        // One persistent scratch buffer instead of a glGenBuffers/glDeleteBuffers
        // pair per draw call.
        basevertex_check_context();
        if (!g_basevertex_ibo) GLES.glGenBuffers(1, &g_basevertex_ibo);
        GLES.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_basevertex_ibo);
        GLES.glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(bytes), tempIndices, GL_STREAM_DRAW);

        GLES.glDrawElements(mode, count, type, nullptr);

        GLES.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, prevElementBuffer);

        CHECK_GL_ERROR
    } else {
        GLES.glDrawElementsBaseVertex(mode, count, type, indices, basevertex);
    }
    CHECK_GL_ERROR
}

#define DR_WARN_ONCE(...)                                                                                              \
    do {                                                                                                               \
        static bool mg_dr_warned = false;                                                                              \
        if (!mg_dr_warned) {                                                                                           \
            mg_dr_warned = true;                                                                                       \
            LOG_W_FORCE(__VA_ARGS__)                                                                                   \
        }                                                                                                              \
    } while (0)

// ---------------------------------------------------------------------------
// The rest of the indexed draw family
//
// These were pass-throughs in gl/gl_native.cpp, so GL_PRIMITIVE_RESTART with a
// custom index went straight to a driver that has no such feature and every
// restart in the batch was drawn as ordinary geometry -- strips joined end to
// end. They are indexed draws like the three above and owe the same treatment:
// rewrite the stream when the chosen value is not the fixed one, and otherwise
// switch the driver's fixed-index restart on for the duration.
// ---------------------------------------------------------------------------

// Brackets a draw with GLES' fixed-index restart. Scoped so an early return
// cannot leave it enabled behind the application's back.
namespace {
struct restart_guard_t {
    bool on;
    explicit restart_guard_t(GLenum type) : on(mg_restart_needs_driver_fixed(type)) {
        if (on) GLES.glEnable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
    }
    ~restart_guard_t() {
        if (on) GLES.glDisable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
    }
    restart_guard_t(const restart_guard_t&) = delete;
    restart_guard_t& operator=(const restart_guard_t&) = delete;
};
} // namespace

void glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void* indices) {
    LOG()
    MG_PZ_CENSUS(mg_pz_census_draw(true, mode, count, 1));
    LOG_D("glDrawRangeElements, mode: %d, start: %u, end: %u, count: %d, type: %d", mode, start, end, count, type)
    prepareForDraw();
    if (mode == GL_QUADS && draw_elements_as_triangles(count, type, indices, 0, -1)) return;
    // The rewritten stream is 32-bit with 0xFFFFFFFF sentinels, so start/end no
    // longer describe it. They are only a promise about the index range, and
    // dropping the promise is allowed; drawing the wrong primitives is not.
    if (mg_restart_needs_rewrite(type) && mg_draw_elements_restart(mode, count, type, indices, 0, -1)) return;
    restart_guard_t guard(type);
    GLES.glDrawRangeElements(mode, start, end, count, type, indices);
    CHECK_GL_ERROR
}

void glDrawRangeElementsBaseVertex(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                                   const void* indices, GLint basevertex) {
    LOG()
    MG_PZ_CENSUS(mg_pz_census_draw(true, mode, count, 1));
    LOG_D("glDrawRangeElementsBaseVertex, mode: %d, count: %d, type: %d, basevertex: %d", mode, count, type, basevertex)
    prepareForDraw();
    if (mode == GL_QUADS && draw_elements_as_triangles(count, type, indices, basevertex, -1)) return;
    if (mg_restart_needs_rewrite(type) && mg_draw_elements_restart(mode, count, type, indices, basevertex, -1)) return;
    restart_guard_t guard(type);
    if (GLES.glDrawRangeElementsBaseVertex) {
        GLES.glDrawRangeElementsBaseVertex(mode, start, end, count, type, indices, basevertex);
    } else {
        // glDrawElementsBaseVertex above already emulates the base vertex when
        // the driver cannot; the range is the only thing lost.
        glDrawElementsBaseVertex(mode, count, type, indices, basevertex);
    }
    CHECK_GL_ERROR
}

void glDrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                       GLsizei instancecount, GLint basevertex) {
    LOG()
    MG_PZ_CENSUS(mg_pz_census_draw(true, mode, count, instancecount));
    LOG_D("glDrawElementsInstancedBaseVertex, mode: %d, count: %d, type: %d, instancecount: %d, basevertex: %d", mode,
          count, type, instancecount, basevertex)
    prepareForDraw();
    if (mode == GL_QUADS && draw_elements_as_triangles(count, type, indices, basevertex, instancecount)) return;
    if (mg_restart_needs_rewrite(type) &&
        mg_draw_elements_restart(mode, count, type, indices, basevertex, instancecount))
        return;
    restart_guard_t guard(type);
    if (GLES.glDrawElementsInstancedBaseVertex) {
        GLES.glDrawElementsInstancedBaseVertex(mode, count, type, indices, instancecount, basevertex);
    } else if (basevertex == 0) {
        GLES.glDrawElementsInstanced(mode, count, type, indices, instancecount);
    } else {
        DR_WARN_ONCE("glDrawElementsInstancedBaseVertex: no base vertex support on this context, drawing without it");
        GLES.glDrawElementsInstanced(mode, count, type, indices, instancecount);
    }
    CHECK_GL_ERROR
}

// ---------------------------------------------------------------------------
// The base instance family (GL 4.2 / ARB_base_instance)
//
// GLES has no base instance in core, and no extension for it on the drivers
// this layer targets, so these three were stubs in gl/gl_stub.cpp: called, they
// drew nothing at all. That is the worst of the available options -- a mesh that
// silently never appears is harder to diagnose than one in the wrong place, and
// baseinstance is 0 in the overwhelming majority of calls, where these commands
// are exactly the ones GLES already implements.
//
// So they forward, and a non-zero base instance is reported once and then
// ignored. The instanced attribute fetch then starts at element 0 instead of
// baseinstance, which is wrong for that case only, and stays visible.
// ---------------------------------------------------------------------------

void glDrawArraysInstancedBaseInstance(GLenum mode, GLint first, GLsizei count, GLsizei instancecount,
                                       GLuint baseinstance) {
    LOG()
    MG_PZ_CENSUS(mg_pz_census_draw(false, mode, count, instancecount));
    LOG_D("glDrawArraysInstancedBaseInstance, mode: %d, first: %d, count: %d, instancecount: %d, baseinstance: %u",
          mode, first, count, instancecount, baseinstance)
    if (baseinstance != 0) {
        DR_WARN_ONCE("glDrawArraysInstancedBaseInstance: baseinstance %u ignored, GLES has no base instance",
                     baseinstance);
    }
    prepareForDraw();
    if (mode == GL_QUADS && draw_arrays_as_triangles(first, count, instancecount)) return;
    GLES.glDrawArraysInstanced(mode, first, count, instancecount);
    CHECK_GL_ERROR
}

void glDrawElementsInstancedBaseInstance(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                         GLsizei instancecount, GLuint baseinstance) {
    LOG()
    LOG_D("glDrawElementsInstancedBaseInstance, mode: %d, count: %d, type: %d, instancecount: %d, baseinstance: %u",
          mode, count, type, instancecount, baseinstance)
    if (baseinstance != 0) {
        DR_WARN_ONCE("glDrawElementsInstancedBaseInstance: baseinstance %u ignored, GLES has no base instance",
                     baseinstance);
    }
    glDrawElementsInstanced(mode, count, type, indices, instancecount);
}

void glDrawElementsInstancedBaseVertexBaseInstance(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                                   GLsizei instancecount, GLint basevertex, GLuint baseinstance) {
    LOG()
    LOG_D("glDrawElementsInstancedBaseVertexBaseInstance, mode: %d, count: %d, basevertex: %d, baseinstance: %u", mode,
          count, basevertex, baseinstance)
    if (baseinstance != 0) {
        DR_WARN_ONCE(
            "glDrawElementsInstancedBaseVertexBaseInstance: baseinstance %u ignored, GLES has no base instance",
            baseinstance);
    }
    glDrawElementsInstancedBaseVertex(mode, count, type, indices, instancecount, basevertex);
}
