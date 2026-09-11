// MobileGlues - gl/pz_geometry_arena.cpp
// Isolated ZomDroid experiment: collapse the learned 256 KiB streaming VBO pool
// into one persistently mapped backend buffer and canonicalize legacy vertex
// pointers so buffer changes can be represented as base-vertex changes.

#include "pz_geometry_arena.h"

#if defined(ZOMDROID_EXPERIMENTAL)

#include "../gles/loader.h"
#include "log.h"
#include "pz_census.h"
#include <EGL/egl.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <unordered_map>

namespace {

constexpr GLsizeiptr kPzVertexBufferBytes = 262144;
constexpr GLsizeiptr kVertexSlotPitch = 263040; // 274 * 960: divisible by common PZ strides.
constexpr uint32_t kDefaultSlots = 256;
constexpr uint32_t kMinSlots = 64;
constexpr uint32_t kMaxSlots = 512;
constexpr size_t kTrackedAttribs = 32;
constexpr GLuint kUnknownBinding = std::numeric_limits<GLuint>::max();

using egl_get_current_context_fn = EGLContext (*)();

struct originals_t {
    glBindBuffer_PTR bind_buffer = nullptr;
    glBufferStorageEXT_PTR buffer_storage = nullptr;
    glMapBufferRange_PTR map_buffer_range = nullptr;
    glGenBuffers_PTR gen_buffers = nullptr;
    glDeleteBuffers_PTR delete_buffers = nullptr;
    glGetIntegerv_PTR get_integerv = nullptr;
    glGetStringi_PTR get_string_i = nullptr;
    glGetBufferParameteriv_PTR get_buffer_parameteriv = nullptr;
    glGetBufferPointerv_PTR get_buffer_pointerv = nullptr;
    glGetVertexAttribPointerv_PTR get_vertex_attrib_pointerv = nullptr;
    glBindVertexArray_PTR bind_vertex_array = nullptr;
    glDeleteVertexArrays_PTR delete_vertex_arrays = nullptr;
    glEnableVertexAttribArray_PTR enable_vertex_attrib_array = nullptr;
    glDisableVertexAttribArray_PTR disable_vertex_attrib_array = nullptr;
    glVertexAttribPointer_PTR vertex_attrib_pointer = nullptr;
    glVertexAttribIPointer_PTR vertex_attrib_ipointer = nullptr;
    glVertexAttribDivisor_PTR vertex_attrib_divisor = nullptr;
    glBindVertexBuffer_PTR bind_vertex_buffer = nullptr;
    glVertexAttribFormat_PTR vertex_attrib_format = nullptr;
    glVertexAttribIFormat_PTR vertex_attrib_iformat = nullptr;
    glVertexAttribBinding_PTR vertex_attrib_binding = nullptr;
    glVertexBindingDivisor_PTR vertex_binding_divisor = nullptr;
    glDrawArrays_PTR draw_arrays = nullptr;
    glDrawArraysInstanced_PTR draw_arrays_instanced = nullptr;
    glDrawArraysIndirect_PTR draw_arrays_indirect = nullptr;
    glDrawElements_PTR draw_elements = nullptr;
    glDrawElementsInstanced_PTR draw_elements_instanced = nullptr;
    glDrawElementsIndirect_PTR draw_elements_indirect = nullptr;
    glDrawElementsBaseVertex_PTR draw_elements_base_vertex = nullptr;
    glDrawElementsInstancedBaseVertex_PTR draw_elements_instanced_base_vertex = nullptr;
    glDrawRangeElements_PTR draw_range_elements = nullptr;
    glDrawRangeElementsBaseVertex_PTR draw_range_elements_base_vertex = nullptr;
    glMultiDrawArraysIndirectEXT_PTR multi_draw_arrays_indirect = nullptr;
    glMultiDrawElementsIndirectEXT_PTR multi_draw_elements_indirect = nullptr;
    glMultiDrawElementsBaseVertexEXT_PTR multi_draw_elements_base_vertex = nullptr;
    egl_get_current_context_fn get_current_context = nullptr;
};

struct region_t {
    GLintptr offset = 0;
    GLsizeiptr size = 0;
};

struct attrib_t {
    bool enabled = false;
    bool configured = false;
    bool integer = false;
    bool arena_source = false;
    bool driver_known = false;
    GLuint source_buffer = 0;
    GLintptr arena_offset = 0;
    GLint size = 4;
    GLenum type = GL_FLOAT;
    GLboolean normalized = GL_FALSE;
    GLsizei stride = 0;
    uintptr_t pointer = 0;
    GLuint divisor = 0;

    GLuint driver_buffer = 0;
    uintptr_t driver_pointer = 0;
    GLint driver_size = 4;
    GLenum driver_type = GL_FLOAT;
    GLboolean driver_normalized = GL_FALSE;
    GLsizei driver_stride = 0;
    bool driver_integer = false;
};

struct vao_t {
    std::array<attrib_t, kTrackedAttribs> attribs{};
    GLuint element_buffer = 0;
    bool binding_model = false;
    bool prefer_canonical = false;
};

struct stats_t {
    unsigned long long aliases = 0;
    unsigned long long alias_bytes = 0;
    unsigned long long maps = 0;
    unsigned long long bind_calls = 0;
    unsigned long long bind_skips = 0;
    unsigned long long attrib_calls = 0;
    unsigned long long attrib_skips = 0;
    unsigned long long canonical_draws = 0;
    unsigned long long materialized_draws = 0;
    unsigned long long canonical_misses = 0;
    unsigned long long capacity_fallbacks = 0;
    unsigned long long context_resets = 0;
};

struct thread_state_t {
    EGLContext context = EGL_NO_CONTEXT;
    GLuint arena = 0;
    unsigned char* mapped = nullptr;
    GLsizeiptr capacity = 0;
    uint32_t slots = 0;
    uint32_t next_slot = 0;
    bool arena_failed = false;

    GLuint requested_array = 0;
    GLuint actual_array = kUnknownBinding;
    GLuint current_vao = 0;

    std::unordered_map<GLuint, region_t> regions;
    std::unordered_map<GLuint, vao_t> vaos;
    stats_t stats;
};

originals_t g_orig;
bool g_enabled = false;
uint32_t g_requested_slots = kDefaultSlots;
thread_local thread_state_t g_state;

uint32_t parse_slots() {
    const char* value = std::getenv("MOBILEGLUES_PZ_GEOMETRY_ARENA_SLOTS");
    if (value == nullptr || *value == '\0') return kDefaultSlots;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value || *end != '\0') return kDefaultSlots;
    return static_cast<uint32_t>(std::clamp<unsigned long>(parsed, kMinSlots, kMaxSlots));
}

EGLContext current_context() {
    return g_orig.get_current_context ? g_orig.get_current_context() : EGL_NO_CONTEXT;
}

void abandon_for_context(EGLContext context) {
    const unsigned long long resets = g_state.stats.context_resets + (g_state.context != EGL_NO_CONTEXT ? 1ULL : 0ULL);
    // GL objects from the old context deliberately are not deleted here: once a
    // different context is current we cannot safely address them. The owning
    // context/driver will retire them with that context.
    g_state = {};
    g_state.context = context;
    g_state.stats.context_resets = resets;
}

void ensure_context_for_alias(GLuint buffer) {
    if (!g_enabled) return;
    if (g_state.regions.find(buffer) == g_state.regions.end()) return;
    const EGLContext now = current_context();
    if (now != g_state.context) abandon_for_context(now);
}

void ensure_context_for_vao() {
    if (!g_enabled || (g_state.arena == 0 && g_state.vaos.empty())) return;
    const EGLContext now = current_context();
    if (g_state.context != EGL_NO_CONTEXT && now != g_state.context) abandon_for_context(now);
}

bool backend_has_extension(const char* name) {
    if (name == nullptr || g_orig.get_integerv == nullptr || g_orig.get_string_i == nullptr) return false;
    GLint count = 0;
    g_orig.get_integerv(GL_NUM_EXTENSIONS, &count);
    for (GLint i = 0; i < count; ++i) {
        const GLubyte* value = g_orig.get_string_i(GL_EXTENSIONS, static_cast<GLuint>(i));
        if (value != nullptr && std::strcmp(reinterpret_cast<const char*>(value), name) == 0) return true;
    }
    return false;
}

bool candidate_storage(GLenum target, GLsizeiptr size, const void* data, GLbitfield flags) {
    if (target != GL_ARRAY_BUFFER || size != kPzVertexBufferBytes || data != nullptr) return false;
    constexpr GLbitfield required =
        GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT | GL_DYNAMIC_STORAGE_BIT;
    return (flags & required) == required && (flags & ~required) == 0;
}

bool create_arena() {
    if (g_state.arena != 0 && g_state.mapped != nullptr) return true;
    if (g_state.arena_failed || g_orig.gen_buffers == nullptr || g_orig.bind_buffer == nullptr ||
        g_orig.buffer_storage == nullptr || g_orig.map_buffer_range == nullptr || g_orig.get_integerv == nullptr)
        return false;

    const EGLContext now = current_context();
    if (now == EGL_NO_CONTEXT) return false;
    if (g_state.context != EGL_NO_CONTEXT && g_state.context != now) abandon_for_context(now);
    if (g_state.context == EGL_NO_CONTEXT) g_state.context = now;

    const GLsizeiptr capacity = kVertexSlotPitch * static_cast<GLsizeiptr>(g_requested_slots);
    GLint saved_copy_write = 0;
    g_orig.get_integerv(GL_COPY_WRITE_BUFFER_BINDING, &saved_copy_write);

    GLuint arena = 0;
    g_orig.gen_buffers(1, &arena);
    if (arena == 0) {
        g_state.arena_failed = true;
        return false;
    }

    g_orig.bind_buffer(GL_COPY_WRITE_BUFFER, arena);
    constexpr GLbitfield storage_flags =
        GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT | GL_DYNAMIC_STORAGE_BIT;
    g_orig.buffer_storage(GL_COPY_WRITE_BUFFER, capacity, nullptr, storage_flags);
    void* mapped = g_orig.map_buffer_range(
        GL_COPY_WRITE_BUFFER, 0, capacity, GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
    g_orig.bind_buffer(GL_COPY_WRITE_BUFFER, static_cast<GLuint>(saved_copy_write));

    if (mapped == nullptr) {
        g_orig.delete_buffers(1, &arena);
        g_state.arena_failed = true;
        LOG_W_FORCE("ZOMDROID_PZ_GEOMETRY_ARENA disabled reason=arena_allocation_failed bytes=%lld",
                    static_cast<long long>(capacity))
        return false;
    }

    g_state.arena = arena;
    g_state.mapped = static_cast<unsigned char*>(mapped);
    g_state.capacity = capacity;
    g_state.slots = g_requested_slots;
    LOG_I("ZOMDROID_PZ_GEOMETRY_ARENA arena=1 buffer=%u bytes=%lld slots=%u logical_bytes=%lld pitch=%lld",
          arena, static_cast<long long>(capacity), g_requested_slots,
          static_cast<long long>(kPzVertexBufferBytes), static_cast<long long>(kVertexSlotPitch))
    return true;
}

const region_t* region_for(GLuint buffer) {
    const auto it = g_state.regions.find(buffer);
    return it == g_state.regions.end() ? nullptr : &it->second;
}

vao_t& current_vao_state() {
    return g_state.vaos[g_state.current_vao];
}

bool driver_attrib_matches(const attrib_t& a, GLuint buffer, uintptr_t pointer, bool integer, GLint size, GLenum type,
                           GLboolean normalized, GLsizei stride) {
    return a.driver_known && a.driver_buffer == buffer && a.driver_pointer == pointer && a.driver_integer == integer &&
           a.driver_size == size && a.driver_type == type && a.driver_normalized == normalized &&
           a.driver_stride == stride;
}

void remember_driver_attrib(attrib_t& a, GLuint buffer, uintptr_t pointer, bool integer, GLint size, GLenum type,
                            GLboolean normalized, GLsizei stride) {
    a.driver_known = true;
    a.driver_buffer = buffer;
    a.driver_pointer = pointer;
    a.driver_integer = integer;
    a.driver_size = size;
    a.driver_type = type;
    a.driver_normalized = normalized;
    a.driver_stride = stride;
}

bool canonical_base(const vao_t& vao, GLint* base_out) {
    if (vao.binding_model || g_state.arena == 0) return false;
    bool any = false;
    uint64_t common = 0;
    for (const attrib_t& a : vao.attribs) {
        if (!a.enabled) continue;
        if (!a.configured || !a.arena_source || a.divisor != 0 || a.stride <= 0) return false;
        const uint64_t stride = static_cast<uint64_t>(a.stride);
        const uint64_t offset = static_cast<uint64_t>(a.arena_offset);
        if (offset % stride != 0) return false;
        const uint64_t base = offset / stride;
        if (!any) {
            common = base;
            any = true;
        } else if (common != base) {
            return false;
        }
    }
    if (!any || common > static_cast<uint64_t>(std::numeric_limits<GLint>::max())) return false;
    *base_out = static_cast<GLint>(common);
    return true;
}

void ensure_representation(vao_t& vao, bool canonical) {
    bool needs_arena = false;
    for (const attrib_t& a : vao.attribs) {
        if (a.enabled && a.configured && a.arena_source) {
            needs_arena = true;
            break;
        }
    }
    if (!needs_arena || g_state.arena == 0) return;

    const GLuint restore = g_state.actual_array;
    const bool restore_known = restore != kUnknownBinding;
    if (!restore_known || restore != g_state.arena) g_orig.bind_buffer(GL_ARRAY_BUFFER, g_state.arena);

    for (size_t i = 0; i < vao.attribs.size(); ++i) {
        attrib_t& a = vao.attribs[i];
        if (!a.enabled || !a.configured || !a.arena_source) continue;
        const uintptr_t pointer = a.pointer + (canonical ? 0U : static_cast<uintptr_t>(a.arena_offset));
        if (driver_attrib_matches(a, g_state.arena, pointer, a.integer, a.size, a.type, a.normalized, a.stride))
            continue;
        if (a.integer) {
            if (g_orig.vertex_attrib_ipointer)
                g_orig.vertex_attrib_ipointer(static_cast<GLuint>(i), a.size, a.type, a.stride,
                                              reinterpret_cast<const void*>(pointer));
        } else if (g_orig.vertex_attrib_pointer) {
            g_orig.vertex_attrib_pointer(static_cast<GLuint>(i), a.size, a.type, a.normalized, a.stride,
                                         reinterpret_cast<const void*>(pointer));
        }
        remember_driver_attrib(a, g_state.arena, pointer, a.integer, a.size, a.type, a.normalized, a.stride);
    }

    if (restore_known && restore != g_state.arena) g_orig.bind_buffer(GL_ARRAY_BUFFER, restore);
}

void maybe_report() {
    const unsigned long long draws = g_state.stats.canonical_draws + g_state.stats.materialized_draws;
    if (draws != 1 && draws != 1024 && draws != 65536 && (draws == 0 || draws % 1000000ULL != 0)) return;
    LOG_I("ZOMDROID_PZ_GEOMETRY_ARENA draws=%llu aliases=%llu/%lluB maps=%llu bind_skip=%llu/%llu "
          "attrib_skip=%llu/%llu basevertex=%llu materialized=%llu canonical_miss=%llu capacity_fallback=%llu "
          "context_resets=%llu",
          draws, g_state.stats.aliases, g_state.stats.alias_bytes, g_state.stats.maps, g_state.stats.bind_skips,
          g_state.stats.bind_calls, g_state.stats.attrib_skips, g_state.stats.attrib_calls,
          g_state.stats.canonical_draws, g_state.stats.materialized_draws, g_state.stats.canonical_misses,
          g_state.stats.capacity_fallbacks, g_state.stats.context_resets)
}

void materialized_draw(vao_t& vao) {
    ensure_representation(vao, false);
    vao.prefer_canonical = false;
    ++g_state.stats.materialized_draws;
    maybe_report();
}

bool prepare_canonical_draw(vao_t& vao, GLint* base) {
    if (!canonical_base(vao, base)) {
        bool has_arena = false;
        for (const attrib_t& a : vao.attribs) has_arena = has_arena || (a.enabled && a.arena_source);
        if (has_arena) ++g_state.stats.canonical_misses;
        materialized_draw(vao);
        return false;
    }
    ensure_representation(vao, true);
    vao.prefer_canonical = true;
    ++g_state.stats.canonical_draws;
    maybe_report();
    return true;
}

void arena_glBindBuffer(GLenum target, GLuint buffer) {
    if (!g_enabled) {
        g_orig.bind_buffer(target, buffer);
        return;
    }
    if (target == GL_ELEMENT_ARRAY_BUFFER) {
        current_vao_state().element_buffer = buffer;
        g_orig.bind_buffer(target, buffer);
        return;
    }
    if (target != GL_ARRAY_BUFFER) {
        // This experiment aliases only the learned PZ vertex stream. The exact
        // 256 KiB objects are never expected on other targets; leave unrelated
        // targets byte-for-byte on the stable backend path.
        g_orig.bind_buffer(target, buffer);
        return;
    }

    ensure_context_for_alias(buffer);
    ++g_state.stats.bind_calls;
    g_state.requested_array = buffer;
    const region_t* region = region_for(buffer);
    const GLuint actual = region ? g_state.arena : buffer;
    if (g_state.actual_array == actual) {
        ++g_state.stats.bind_skips;
        return;
    }
    g_orig.bind_buffer(target, actual);
    g_state.actual_array = actual;
}

void arena_glBufferStorageEXT(GLenum target, GLsizeiptr size, const void* data, GLbitfield flags) {
    if (!g_enabled || !candidate_storage(target, size, data, flags) || g_state.requested_array == 0) {
        g_orig.buffer_storage(target, size, data, flags);
        return;
    }

    const EGLContext now = current_context();
    if (g_state.context != EGL_NO_CONTEXT && now != g_state.context) abandon_for_context(now);
    if (g_state.context == EGL_NO_CONTEXT) g_state.context = now;

    if (!create_arena() || g_state.next_slot >= g_state.slots) {
        ++g_state.stats.capacity_fallbacks;
        if (g_state.next_slot >= g_state.slots && g_state.stats.capacity_fallbacks == 1)
            LOG_W_FORCE("ZOMDROID_PZ_GEOMETRY_ARENA capacity_exhausted slots=%u fallback=standalone",
                        g_state.slots)
        g_orig.buffer_storage(target, size, data, flags);
        return;
    }

    const GLuint buffer = g_state.requested_array;
    if (region_for(buffer) != nullptr) {
        // Re-specifying immutable storage is invalid in the standalone path too.
        // Calling the real storage function against the already immutable arena
        // preserves that failure instead of silently accepting a second store.
        g_orig.buffer_storage(target, size, data, flags);
        return;
    }

    const GLintptr offset = static_cast<GLintptr>(g_state.next_slot) * kVertexSlotPitch;
    ++g_state.next_slot;
    g_state.regions.emplace(buffer, region_t{offset, size});
    ++g_state.stats.aliases;
    g_state.stats.alias_bytes += static_cast<unsigned long long>(size);

    // The caller bound the just-created standalone name immediately before this
    // storage call. Replace that physical binding with the shared arena; the
    // frontend continues to remember its private name and sees no semantic change.
    if (g_state.actual_array != g_state.arena) {
        g_orig.bind_buffer(GL_ARRAY_BUFFER, g_state.arena);
        g_state.actual_array = g_state.arena;
    }

    if (g_state.stats.aliases == 1 || g_state.stats.aliases == 64 || g_state.stats.aliases == 256) {
        LOG_I("ZOMDROID_PZ_GEOMETRY_ARENA alias=%llu backend=%u offset=%lld size=%lld",
              g_state.stats.aliases, buffer, static_cast<long long>(offset), static_cast<long long>(size))
    }
}

void* arena_glMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access) {
    if (!g_enabled || target != GL_ARRAY_BUFFER) return g_orig.map_buffer_range(target, offset, length, access);

    const region_t* region = region_for(g_state.requested_array);
    if (region == nullptr || g_state.mapped == nullptr) return g_orig.map_buffer_range(target, offset, length, access);

    const bool range_valid = offset >= 0 && length >= 0 && offset <= region->size && length <= region->size - offset;
    constexpr GLbitfield required = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    const bool access_valid = (access & required) == required && (access & GL_MAP_READ_BIT) == 0;
    if (!range_valid || !access_valid) {
        // The shared store is already persistently mapped. A second incompatible
        // map is invalid just as it would be for the standalone persistent store.
        return g_orig.map_buffer_range(target, region->offset + offset, length, access);
    }

    ++g_state.stats.maps;
    return g_state.mapped + region->offset + offset;
}

void arena_glDeleteBuffers(GLsizei n, const GLuint* buffers) {
    if (g_enabled && buffers != nullptr && n > 0) {
        for (GLsizei i = 0; i < n; ++i) {
            const GLuint buffer = buffers[i];
            ensure_context_for_alias(buffer);
            const auto it = g_state.regions.find(buffer);
            if (it != g_state.regions.end()) {
                g_state.regions.erase(it);
                if (g_state.requested_array == buffer) g_state.requested_array = 0;
            }
            for (auto& entry : g_state.vaos)
                if (entry.second.element_buffer == buffer) entry.second.element_buffer = 0;
        }
    }
    g_orig.delete_buffers(n, buffers);
}

void arena_glGetBufferParameteriv(GLenum target, GLenum pname, GLint* params) {
    if (g_enabled && target == GL_ARRAY_BUFFER && params != nullptr) {
        const region_t* region = region_for(g_state.requested_array);
        if (region != nullptr) {
            switch (pname) {
            case GL_BUFFER_SIZE:
                *params = region->size > std::numeric_limits<GLint>::max()
                              ? std::numeric_limits<GLint>::max()
                              : static_cast<GLint>(region->size);
                return;
            case GL_BUFFER_MAPPED:
                *params = GL_TRUE;
                return;
            case GL_BUFFER_MAP_OFFSET:
                *params = 0;
                return;
            case GL_BUFFER_MAP_LENGTH:
                *params = static_cast<GLint>(region->size);
                return;
            case GL_BUFFER_ACCESS:
                *params = GL_WRITE_ONLY;
                return;
#ifdef GL_BUFFER_ACCESS_FLAGS
            case GL_BUFFER_ACCESS_FLAGS:
                *params = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
                return;
#endif
#ifdef GL_BUFFER_IMMUTABLE_STORAGE
            case GL_BUFFER_IMMUTABLE_STORAGE:
                *params = GL_TRUE;
                return;
#endif
#ifdef GL_BUFFER_STORAGE_FLAGS
            case GL_BUFFER_STORAGE_FLAGS:
                *params = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT | GL_DYNAMIC_STORAGE_BIT;
                return;
#endif
            default:
                break;
            }
        }
    }
    g_orig.get_buffer_parameteriv(target, pname, params);
}

void arena_glGetBufferPointerv(GLenum target, GLenum pname, void** params) {
    if (g_enabled && target == GL_ARRAY_BUFFER && pname == GL_BUFFER_MAP_POINTER && params != nullptr) {
        const region_t* region = region_for(g_state.requested_array);
        if (region != nullptr && g_state.mapped != nullptr) {
            *params = g_state.mapped + region->offset;
            return;
        }
    }
    g_orig.get_buffer_pointerv(target, pname, params);
}

void arena_glGetVertexAttribPointerv(GLuint index, GLenum pname, void** pointer) {
    if (g_enabled && pointer != nullptr && pname == GL_VERTEX_ATTRIB_ARRAY_POINTER && index < kTrackedAttribs) {
        const auto found = g_state.vaos.find(g_state.current_vao);
        if (found != g_state.vaos.end() && found->second.attribs[index].configured) {
            *pointer = reinterpret_cast<void*>(found->second.attribs[index].pointer);
            return;
        }
    }
    g_orig.get_vertex_attrib_pointerv(index, pname, pointer);
}

void arena_glBindVertexArray(GLuint array) {
    if (g_enabled) {
        ensure_context_for_vao();
        g_state.current_vao = array;
    }
    g_orig.bind_vertex_array(array);
}

void arena_glDeleteVertexArrays(GLsizei n, const GLuint* arrays) {
    if (g_enabled && arrays != nullptr && n > 0) {
        for (GLsizei i = 0; i < n; ++i) g_state.vaos.erase(arrays[i]);
    }
    g_orig.delete_vertex_arrays(n, arrays);
}

void arena_glEnableVertexAttribArray(GLuint index) {
    if (g_enabled && index < kTrackedAttribs) current_vao_state().attribs[index].enabled = true;
    g_orig.enable_vertex_attrib_array(index);
}

void arena_glDisableVertexAttribArray(GLuint index) {
    if (g_enabled && index < kTrackedAttribs) current_vao_state().attribs[index].enabled = false;
    g_orig.disable_vertex_attrib_array(index);
}

void record_pointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void* pointer,
                    bool integer) {
    if (!g_enabled || index >= kTrackedAttribs) {
        if (integer)
            g_orig.vertex_attrib_ipointer(index, size, type, stride, pointer);
        else
            g_orig.vertex_attrib_pointer(index, size, type, normalized, stride, pointer);
        return;
    }

    ++g_state.stats.attrib_calls;
    vao_t& vao = current_vao_state();
    attrib_t& a = vao.attribs[index];
    a.configured = true;
    a.integer = integer;
    a.source_buffer = g_state.requested_array;
    a.size = size;
    a.type = type;
    a.normalized = normalized;
    a.stride = stride;
    a.pointer = reinterpret_cast<uintptr_t>(pointer);

    const region_t* region = region_for(a.source_buffer);
    a.arena_source = region != nullptr;
    a.arena_offset = region ? region->offset : 0;

    const bool canonical = vao.prefer_canonical && a.arena_source && stride > 0 &&
                           (static_cast<uint64_t>(a.arena_offset) % static_cast<uint64_t>(stride) == 0);
    const GLuint desired_buffer = a.arena_source ? g_state.arena : a.source_buffer;
    const uintptr_t desired_pointer =
        a.pointer + (a.arena_source && !canonical ? static_cast<uintptr_t>(a.arena_offset) : 0U);

    if (driver_attrib_matches(a, desired_buffer, desired_pointer, integer, size, type, normalized, stride)) {
        ++g_state.stats.attrib_skips;
        return;
    }

    if (integer)
        g_orig.vertex_attrib_ipointer(index, size, type, stride, reinterpret_cast<const void*>(desired_pointer));
    else
        g_orig.vertex_attrib_pointer(index, size, type, normalized, stride,
                                     reinterpret_cast<const void*>(desired_pointer));
    remember_driver_attrib(a, desired_buffer, desired_pointer, integer, size, type, normalized, stride);
}

void arena_glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride,
                                 const void* pointer) {
    record_pointer(index, size, type, normalized, stride, pointer, false);
}

void arena_glVertexAttribIPointer(GLuint index, GLint size, GLenum type, GLsizei stride, const void* pointer) {
    record_pointer(index, size, type, GL_FALSE, stride, pointer, true);
}

void arena_glVertexAttribDivisor(GLuint index, GLuint divisor) {
    if (g_enabled && index < kTrackedAttribs) current_vao_state().attribs[index].divisor = divisor;
    g_orig.vertex_attrib_divisor(index, divisor);
}

void arena_glBindVertexBuffer(GLuint bindingindex, GLuint buffer, GLintptr offset, GLsizei stride) {
    if (g_enabled) {
        vao_t& vao = current_vao_state();
        vao.binding_model = true;
        ensure_context_for_alias(buffer);
        const region_t* region = region_for(buffer);
        if (region != nullptr) {
            g_orig.bind_vertex_buffer(bindingindex, g_state.arena, region->offset + offset, stride);
            return;
        }
    }
    g_orig.bind_vertex_buffer(bindingindex, buffer, offset, stride);
}

void arena_glVertexAttribFormat(GLuint attribindex, GLint size, GLenum type, GLboolean normalized,
                                GLuint relativeoffset) {
    if (g_enabled) current_vao_state().binding_model = true;
    g_orig.vertex_attrib_format(attribindex, size, type, normalized, relativeoffset);
}

void arena_glVertexAttribIFormat(GLuint attribindex, GLint size, GLenum type, GLuint relativeoffset) {
    if (g_enabled) current_vao_state().binding_model = true;
    g_orig.vertex_attrib_iformat(attribindex, size, type, relativeoffset);
}

void arena_glVertexAttribBinding(GLuint attribindex, GLuint bindingindex) {
    if (g_enabled) current_vao_state().binding_model = true;
    g_orig.vertex_attrib_binding(attribindex, bindingindex);
}

void arena_glVertexBindingDivisor(GLuint bindingindex, GLuint divisor) {
    if (g_enabled) current_vao_state().binding_model = true;
    g_orig.vertex_binding_divisor(bindingindex, divisor);
}

void arena_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    if (!g_enabled) {
        g_orig.draw_elements(mode, count, type, indices);
        return;
    }
    vao_t& vao = current_vao_state();
    GLint base = 0;
    if (vao.element_buffer != 0 && g_orig.draw_elements_base_vertex != nullptr) {
        if (prepare_canonical_draw(vao, &base)) {
            g_orig.draw_elements_base_vertex(mode, count, type, indices, base);
            return;
        }
    } else {
        materialized_draw(vao);
    }
    g_orig.draw_elements(mode, count, type, indices);
}

void arena_glDrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices, GLint basevertex) {
    if (!g_enabled) {
        g_orig.draw_elements_base_vertex(mode, count, type, indices, basevertex);
        return;
    }
    vao_t& vao = current_vao_state();
    GLint arena_base = 0;
    if (vao.element_buffer != 0) {
        if (prepare_canonical_draw(vao, &arena_base)) {
            const int64_t combined = static_cast<int64_t>(basevertex) + static_cast<int64_t>(arena_base);
            if (combined >= std::numeric_limits<GLint>::min() && combined <= std::numeric_limits<GLint>::max()) {
                g_orig.draw_elements_base_vertex(mode, count, type, indices, static_cast<GLint>(combined));
                return;
            }
            materialized_draw(vao);
        }
    } else {
        materialized_draw(vao);
    }
    g_orig.draw_elements_base_vertex(mode, count, type, indices, basevertex);
}

void arena_glDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                   GLsizei instancecount) {
    if (!g_enabled || g_orig.draw_elements_instanced_base_vertex == nullptr) {
        if (g_enabled) materialized_draw(current_vao_state());
        g_orig.draw_elements_instanced(mode, count, type, indices, instancecount);
        return;
    }
    vao_t& vao = current_vao_state();
    GLint base = 0;
    if (vao.element_buffer != 0) {
        if (prepare_canonical_draw(vao, &base)) {
            g_orig.draw_elements_instanced_base_vertex(mode, count, type, indices, instancecount, base);
            return;
        }
    } else {
        materialized_draw(vao);
    }
    g_orig.draw_elements_instanced(mode, count, type, indices, instancecount);
}

void arena_glDrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                             GLsizei instancecount, GLint basevertex) {
    if (!g_enabled) {
        g_orig.draw_elements_instanced_base_vertex(mode, count, type, indices, instancecount, basevertex);
        return;
    }
    vao_t& vao = current_vao_state();
    GLint arena_base = 0;
    if (vao.element_buffer != 0) {
        if (prepare_canonical_draw(vao, &arena_base)) {
            const int64_t combined = static_cast<int64_t>(basevertex) + static_cast<int64_t>(arena_base);
            if (combined >= std::numeric_limits<GLint>::min() && combined <= std::numeric_limits<GLint>::max()) {
                g_orig.draw_elements_instanced_base_vertex(mode, count, type, indices, instancecount,
                                                           static_cast<GLint>(combined));
                return;
            }
            materialized_draw(vao);
        }
    } else {
        materialized_draw(vao);
    }
    g_orig.draw_elements_instanced_base_vertex(mode, count, type, indices, instancecount, basevertex);
}

void arena_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    if (!g_enabled) {
        g_orig.draw_arrays(mode, first, count);
        return;
    }
    vao_t& vao = current_vao_state();
    GLint base = 0;
    if (prepare_canonical_draw(vao, &base)) {
        const int64_t shifted = static_cast<int64_t>(first) + static_cast<int64_t>(base);
        if (shifted >= std::numeric_limits<GLint>::min() && shifted <= std::numeric_limits<GLint>::max()) {
            g_orig.draw_arrays(mode, static_cast<GLint>(shifted), count);
            return;
        }
        materialized_draw(vao);
    }
    g_orig.draw_arrays(mode, first, count);
}

void arena_glDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instancecount) {
    if (!g_enabled) {
        g_orig.draw_arrays_instanced(mode, first, count, instancecount);
        return;
    }
    vao_t& vao = current_vao_state();
    GLint base = 0;
    if (prepare_canonical_draw(vao, &base)) {
        const int64_t shifted = static_cast<int64_t>(first) + static_cast<int64_t>(base);
        if (shifted >= std::numeric_limits<GLint>::min() && shifted <= std::numeric_limits<GLint>::max()) {
            g_orig.draw_arrays_instanced(mode, static_cast<GLint>(shifted), count, instancecount);
            return;
        }
        materialized_draw(vao);
    }
    g_orig.draw_arrays_instanced(mode, first, count, instancecount);
}

void arena_glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                               const void* indices) {
    if (g_enabled) materialized_draw(current_vao_state());
    g_orig.draw_range_elements(mode, start, end, count, type, indices);
}

void arena_glDrawRangeElementsBaseVertex(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                                         const void* indices, GLint basevertex) {
    if (g_enabled) materialized_draw(current_vao_state());
    g_orig.draw_range_elements_base_vertex(mode, start, end, count, type, indices, basevertex);
}

void arena_glDrawElementsIndirect(GLenum mode, GLenum type, const void* indirect) {
    if (g_enabled) materialized_draw(current_vao_state());
    g_orig.draw_elements_indirect(mode, type, indirect);
}

void arena_glDrawArraysIndirect(GLenum mode, const void* indirect) {
    if (g_enabled) materialized_draw(current_vao_state());
    g_orig.draw_arrays_indirect(mode, indirect);
}

void arena_glMultiDrawArraysIndirectEXT(GLenum mode, const void* indirect, GLsizei drawcount, GLsizei stride) {
    if (g_enabled) materialized_draw(current_vao_state());
    g_orig.multi_draw_arrays_indirect(mode, indirect, drawcount, stride);
}

void arena_glMultiDrawElementsIndirectEXT(GLenum mode, GLenum type, const void* indirect, GLsizei drawcount,
                                          GLsizei stride) {
    if (g_enabled) materialized_draw(current_vao_state());
    g_orig.multi_draw_elements_indirect(mode, type, indirect, drawcount, stride);
}

void arena_glMultiDrawElementsBaseVertexEXT(GLenum mode, const GLsizei* count, GLenum type,
                                            const void* const* indices, GLsizei drawcount,
                                            const GLint* basevertex) {
    if (g_enabled) materialized_draw(current_vao_state());
    g_orig.multi_draw_elements_base_vertex(mode, count, type, indices, drawcount, basevertex);
}

template <typename Slot, typename Pointer>
Pointer capture(Slot& slot) {
    return static_cast<Pointer>(slot);
}

} // namespace

void mg_pz_geometry_arena_install(void) {
    const char* value = std::getenv("MOBILEGLUES_PZ_GEOMETRY_ARENA");
    if (value == nullptr || std::strcmp(value, "1") != 0) return;

    if (!mg_pz_buffer_streaming_active) {
        LOG_W_FORCE("ZOMDROID_PZ_GEOMETRY_ARENA disabled reason=requires_buffer_streaming")
        return;
    }
    if (!mg_pz_threaded_submission_active) {
        LOG_W_FORCE("ZOMDROID_PZ_GEOMETRY_ARENA disabled reason=requires_threaded_submission")
        return;
    }

    g_orig.bind_buffer = capture<decltype(GLES.glBindBuffer), glBindBuffer_PTR>(GLES.glBindBuffer);
    g_orig.buffer_storage =
        capture<decltype(GLES.glBufferStorageEXT), glBufferStorageEXT_PTR>(GLES.glBufferStorageEXT);
    g_orig.map_buffer_range =
        capture<decltype(GLES.glMapBufferRange), glMapBufferRange_PTR>(GLES.glMapBufferRange);
    g_orig.gen_buffers = capture<decltype(GLES.glGenBuffers), glGenBuffers_PTR>(GLES.glGenBuffers);
    g_orig.delete_buffers = capture<decltype(GLES.glDeleteBuffers), glDeleteBuffers_PTR>(GLES.glDeleteBuffers);
    g_orig.get_integerv = capture<decltype(GLES.glGetIntegerv), glGetIntegerv_PTR>(GLES.glGetIntegerv);
    g_orig.get_string_i = capture<decltype(GLES.glGetStringi), glGetStringi_PTR>(GLES.glGetStringi);
    g_orig.get_buffer_parameteriv =
        capture<decltype(GLES.glGetBufferParameteriv), glGetBufferParameteriv_PTR>(GLES.glGetBufferParameteriv);
    g_orig.get_buffer_pointerv =
        capture<decltype(GLES.glGetBufferPointerv), glGetBufferPointerv_PTR>(GLES.glGetBufferPointerv);
    g_orig.get_vertex_attrib_pointerv =
        capture<decltype(GLES.glGetVertexAttribPointerv), glGetVertexAttribPointerv_PTR>(GLES.glGetVertexAttribPointerv);
    g_orig.bind_vertex_array =
        capture<decltype(GLES.glBindVertexArray), glBindVertexArray_PTR>(GLES.glBindVertexArray);
    g_orig.delete_vertex_arrays =
        capture<decltype(GLES.glDeleteVertexArrays), glDeleteVertexArrays_PTR>(GLES.glDeleteVertexArrays);
    g_orig.enable_vertex_attrib_array = capture<decltype(GLES.glEnableVertexAttribArray), glEnableVertexAttribArray_PTR>(
        GLES.glEnableVertexAttribArray);
    g_orig.disable_vertex_attrib_array =
        capture<decltype(GLES.glDisableVertexAttribArray), glDisableVertexAttribArray_PTR>(
            GLES.glDisableVertexAttribArray);
    g_orig.vertex_attrib_pointer =
        capture<decltype(GLES.glVertexAttribPointer), glVertexAttribPointer_PTR>(GLES.glVertexAttribPointer);
    g_orig.vertex_attrib_ipointer =
        capture<decltype(GLES.glVertexAttribIPointer), glVertexAttribIPointer_PTR>(GLES.glVertexAttribIPointer);
    g_orig.vertex_attrib_divisor =
        capture<decltype(GLES.glVertexAttribDivisor), glVertexAttribDivisor_PTR>(GLES.glVertexAttribDivisor);
    g_orig.bind_vertex_buffer =
        capture<decltype(GLES.glBindVertexBuffer), glBindVertexBuffer_PTR>(GLES.glBindVertexBuffer);
    g_orig.vertex_attrib_format =
        capture<decltype(GLES.glVertexAttribFormat), glVertexAttribFormat_PTR>(GLES.glVertexAttribFormat);
    g_orig.vertex_attrib_iformat =
        capture<decltype(GLES.glVertexAttribIFormat), glVertexAttribIFormat_PTR>(GLES.glVertexAttribIFormat);
    g_orig.vertex_attrib_binding =
        capture<decltype(GLES.glVertexAttribBinding), glVertexAttribBinding_PTR>(GLES.glVertexAttribBinding);
    g_orig.vertex_binding_divisor =
        capture<decltype(GLES.glVertexBindingDivisor), glVertexBindingDivisor_PTR>(GLES.glVertexBindingDivisor);
    g_orig.draw_arrays = capture<decltype(GLES.glDrawArrays), glDrawArrays_PTR>(GLES.glDrawArrays);
    g_orig.draw_arrays_instanced =
        capture<decltype(GLES.glDrawArraysInstanced), glDrawArraysInstanced_PTR>(GLES.glDrawArraysInstanced);
    g_orig.draw_arrays_indirect =
        capture<decltype(GLES.glDrawArraysIndirect), glDrawArraysIndirect_PTR>(GLES.glDrawArraysIndirect);
    g_orig.draw_elements = capture<decltype(GLES.glDrawElements), glDrawElements_PTR>(GLES.glDrawElements);
    g_orig.draw_elements_instanced =
        capture<decltype(GLES.glDrawElementsInstanced), glDrawElementsInstanced_PTR>(GLES.glDrawElementsInstanced);
    g_orig.draw_elements_indirect =
        capture<decltype(GLES.glDrawElementsIndirect), glDrawElementsIndirect_PTR>(GLES.glDrawElementsIndirect);
    g_orig.draw_elements_base_vertex =
        capture<decltype(GLES.glDrawElementsBaseVertex), glDrawElementsBaseVertex_PTR>(GLES.glDrawElementsBaseVertex);
    g_orig.draw_elements_instanced_base_vertex =
        capture<decltype(GLES.glDrawElementsInstancedBaseVertex), glDrawElementsInstancedBaseVertex_PTR>(
            GLES.glDrawElementsInstancedBaseVertex);
    g_orig.draw_range_elements =
        capture<decltype(GLES.glDrawRangeElements), glDrawRangeElements_PTR>(GLES.glDrawRangeElements);
    g_orig.draw_range_elements_base_vertex =
        capture<decltype(GLES.glDrawRangeElementsBaseVertex), glDrawRangeElementsBaseVertex_PTR>(
            GLES.glDrawRangeElementsBaseVertex);
    g_orig.multi_draw_arrays_indirect =
        capture<decltype(GLES.glMultiDrawArraysIndirectEXT), glMultiDrawArraysIndirectEXT_PTR>(
            GLES.glMultiDrawArraysIndirectEXT);
    g_orig.multi_draw_elements_indirect =
        capture<decltype(GLES.glMultiDrawElementsIndirectEXT), glMultiDrawElementsIndirectEXT_PTR>(
            GLES.glMultiDrawElementsIndirectEXT);
    g_orig.multi_draw_elements_base_vertex =
        capture<decltype(GLES.glMultiDrawElementsBaseVertexEXT), glMultiDrawElementsBaseVertexEXT_PTR>(
            GLES.glMultiDrawElementsBaseVertexEXT);
    g_orig.get_current_context =
        egl != nullptr ? reinterpret_cast<egl_get_current_context_fn>(proc_address(egl, "eglGetCurrentContext"))
                       : nullptr;

    if (g_orig.bind_buffer == nullptr || g_orig.buffer_storage == nullptr || g_orig.map_buffer_range == nullptr ||
        g_orig.gen_buffers == nullptr || g_orig.delete_buffers == nullptr || g_orig.get_integerv == nullptr ||
        g_orig.vertex_attrib_pointer == nullptr || g_orig.draw_elements == nullptr ||
        g_orig.draw_elements_base_vertex == nullptr || g_orig.get_current_context == nullptr) {
        LOG_W_FORCE("ZOMDROID_PZ_GEOMETRY_ARENA disabled reason=backend_capability_missing")
        return;
    }

    if (backend_has_extension("GL_EXT_multi_draw_arrays") || backend_has_extension("GL_ANGLE_multi_draw")) {
        // multidraw.cpp resolves those two entry points directly from the driver,
        // bypassing GLES.*. Keeping canonical base-vertex pointers resident would
        // therefore be invisible to that direct call. The target Adreno path does
        // not expose either extension; refuse the experiment on a backend that does.
        LOG_W_FORCE("ZOMDROID_PZ_GEOMETRY_ARENA disabled reason=direct_multidraw_backend")
        return;
    }

    g_requested_slots = parse_slots();
    g_enabled = true;

    GLES.glBindBuffer = arena_glBindBuffer;
    GLES.glBufferStorageEXT = arena_glBufferStorageEXT;
    GLES.glMapBufferRange = arena_glMapBufferRange;
    GLES.glDeleteBuffers = arena_glDeleteBuffers;
    if (g_orig.get_buffer_parameteriv) GLES.glGetBufferParameteriv = arena_glGetBufferParameteriv;
    if (g_orig.get_buffer_pointerv) GLES.glGetBufferPointerv = arena_glGetBufferPointerv;
    if (g_orig.get_vertex_attrib_pointerv) GLES.glGetVertexAttribPointerv = arena_glGetVertexAttribPointerv;
    if (g_orig.bind_vertex_array) GLES.glBindVertexArray = arena_glBindVertexArray;
    if (g_orig.delete_vertex_arrays) GLES.glDeleteVertexArrays = arena_glDeleteVertexArrays;
    if (g_orig.enable_vertex_attrib_array) GLES.glEnableVertexAttribArray = arena_glEnableVertexAttribArray;
    if (g_orig.disable_vertex_attrib_array) GLES.glDisableVertexAttribArray = arena_glDisableVertexAttribArray;
    if (g_orig.vertex_attrib_pointer) GLES.glVertexAttribPointer = arena_glVertexAttribPointer;
    if (g_orig.vertex_attrib_ipointer) GLES.glVertexAttribIPointer = arena_glVertexAttribIPointer;
    if (g_orig.vertex_attrib_divisor) GLES.glVertexAttribDivisor = arena_glVertexAttribDivisor;
    if (g_orig.bind_vertex_buffer) GLES.glBindVertexBuffer = arena_glBindVertexBuffer;
    if (g_orig.vertex_attrib_format) GLES.glVertexAttribFormat = arena_glVertexAttribFormat;
    if (g_orig.vertex_attrib_iformat) GLES.glVertexAttribIFormat = arena_glVertexAttribIFormat;
    if (g_orig.vertex_attrib_binding) GLES.glVertexAttribBinding = arena_glVertexAttribBinding;
    if (g_orig.vertex_binding_divisor) GLES.glVertexBindingDivisor = arena_glVertexBindingDivisor;
    if (g_orig.draw_arrays) GLES.glDrawArrays = arena_glDrawArrays;
    if (g_orig.draw_arrays_instanced) GLES.glDrawArraysInstanced = arena_glDrawArraysInstanced;
    if (g_orig.draw_arrays_indirect) GLES.glDrawArraysIndirect = arena_glDrawArraysIndirect;
    if (g_orig.draw_elements) GLES.glDrawElements = arena_glDrawElements;
    if (g_orig.draw_elements_instanced) GLES.glDrawElementsInstanced = arena_glDrawElementsInstanced;
    if (g_orig.draw_elements_indirect) GLES.glDrawElementsIndirect = arena_glDrawElementsIndirect;
    if (g_orig.draw_elements_base_vertex) GLES.glDrawElementsBaseVertex = arena_glDrawElementsBaseVertex;
    if (g_orig.draw_elements_instanced_base_vertex)
        GLES.glDrawElementsInstancedBaseVertex = arena_glDrawElementsInstancedBaseVertex;
    if (g_orig.draw_range_elements) GLES.glDrawRangeElements = arena_glDrawRangeElements;
    if (g_orig.draw_range_elements_base_vertex)
        GLES.glDrawRangeElementsBaseVertex = arena_glDrawRangeElementsBaseVertex;
    if (g_orig.multi_draw_arrays_indirect) GLES.glMultiDrawArraysIndirectEXT = arena_glMultiDrawArraysIndirectEXT;
    if (g_orig.multi_draw_elements_indirect)
        GLES.glMultiDrawElementsIndirectEXT = arena_glMultiDrawElementsIndirectEXT;
    if (g_orig.multi_draw_elements_base_vertex)
        GLES.glMultiDrawElementsBaseVertexEXT = arena_glMultiDrawElementsBaseVertexEXT;

    LOG_I("ZOMDROID_PZ_GEOMETRY_ARENA enabled=1 mode=shared_persistent_vbo+canonical_basevertex "
          "logical_bytes=%lld pitch=%lld slots=%u",
          static_cast<long long>(kPzVertexBufferBytes), static_cast<long long>(kVertexSlotPitch), g_requested_slots)
}

#else

void mg_pz_geometry_arena_install(void) {}

#endif
