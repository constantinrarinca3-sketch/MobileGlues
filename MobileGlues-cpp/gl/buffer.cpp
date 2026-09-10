// MobileGlues - gl/buffer.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#include "buffer.h"
#include "../egl/context.h"
#include <atomic>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <memory>
#include <cstdint>
#include <limits>
#include <new>
#include <ska/flat_hash_map.hpp>
#include <array>
#include "texture.h"
#include "pz_census.h"

#define DEBUG 0

static GLint maxBufferId = 0;
static GLint maxArrayId = 0;

// ---------------------------------------------------------------------------
// Per-share-group and per-context storage
//
// GL scopes buffer names to the share group -- two contexts created against each
// other see one set of names -- while vertex array objects and the current
// bindings are container state and belong to the context alone, even inside a
// share group. All of it used to be one process-wide set, so a second context
// inherited the first one's names, sizes and bindings.
//
// The tables stay private to this file and are selected by a thread_local
// pointer that eglMakeCurrent swaps, which is why the ~90 access sites only
// changed shape rather than routing through an accessor on every use.
// The state is held by pointer: the map moves its elements when it grows, and
// these thread_local pointers have to outlive other contexts being added.
// ---------------------------------------------------------------------------

namespace {

#if defined(ZOMDROID_EXPERIMENTAL)
// P15 compatibility state.  Desktop glPushClientAttrib snapshots the generic
// vertex input owned by the current VAO; GLES has no equivalent entry point.
// Keep a compact frontend mirror so a push/pop pair does not have to issue
// hundreds of glGet* round trips on a render-hot path.
constexpr size_t kTrackedVertexAttribs = 32;
constexpr size_t kClientAttribStackLimit = 16;

struct vertex_binding_state_t {
    GLuint buffer = 0; // MobileGlues/frontend name, never the renamed GLES id
    uint64_t buffer_lifetime = 0;
    GLuint driver_buffer = 0;
    GLintptr offset = 0;
    GLsizei stride = 16;
    GLuint divisor = 0;
    bool configured = false;
};

struct vertex_attrib_state_t {
    GLboolean enabled = GL_FALSE;
    GLint size = 4;
    GLenum type = GL_FLOAT;
    GLboolean normalized = GL_FALSE;
    GLsizei stride = 0;
    uintptr_t pointer = 0;
    GLuint buffer = 0; // MobileGlues/frontend name
    uint64_t buffer_lifetime = 0;
    GLuint driver_buffer = 0;
    GLuint divisor = 0;
    GLuint binding = 0;
    GLuint relative_offset = 0;
    bool integer = false;
    bool configured = false;
    bool uses_binding_model = false;
};

struct vertex_array_state_t {
    std::array<vertex_attrib_state_t, kTrackedVertexAttribs> attribs{};
    std::array<vertex_binding_state_t, kTrackedVertexAttribs> bindings{};
    GLuint driver_element_buffer = 0;
};

struct client_attrib_snapshot_t {
    GLbitfield mask = 0;
    GLuint vertex_array = 0;
    GLuint array_buffer = 0;
    GLuint element_array_buffer = 0;
    vertex_array_state_t vertex_state{};
};
#endif

// An element-array binding belongs to a VAO, but the frontend buffer name can
// be recycled after glDeleteBuffers.  Remembering only that name makes an old
// VAO appear to reference the new object that later inherited it.  The GLES VAO
// correctly detached the deleted object, while our tracker would then restore
// the unrelated replacement after one of the internal temporary IBO binds.
struct buffer_identity_t {
    GLuint name = 0;
    uint64_t lifetime = 0;
};

#if defined(ZOMDROID_EXPERIMENTAL)
enum class buffer_storage_kind_t : uint8_t {
    none,
    mutable_store,
    immutable_store,
    persistent_stream,
};

struct buffer_staging_map_t {
    std::vector<unsigned char> storage;
    void* pointer = nullptr;
    GLsizeiptr size = 0;
    GLbitfield access = 0;
    bool mapped = false;
    bool persistent_direct = false;
    bool completed_upload = false;
    bool discard_elided = false;
    GLsizeiptr discard_size = 0;
    GLenum discard_usage = GL_STATIC_DRAW;
};

struct buffer_streaming_stats_t {
    unsigned long long attempts = 0;
    unsigned long long hits = 0;
    unsigned long long miss_access = 0;
    unsigned long long miss_size = 0;
    unsigned long long miss_storage = 0;
    unsigned long long miss_busy = 0;
    unsigned long long allocation_failures = 0;
};

struct buffer_discard_coalesce_stats_t {
    unsigned long long elided = 0;
    unsigned long long paired = 0;
    unsigned long long fallbacks = 0;
    unsigned long long superseded = 0;
    unsigned long long bytes = 0;
};

constexpr size_t kGpuBufferRingDepth = 4;

struct gpu_buffer_ring_slot_t {
    GLuint driver_buffer = 0;
    void* pointer = nullptr;
    GLsizeiptr size = 0;
    uint64_t last_use_generation = 0;
};

struct gpu_buffer_ring_t {
    std::array<gpu_buffer_ring_slot_t, kGpuBufferRingDepth> slots{};
    uint8_t count = 0;
    uint8_t current = 0;
    GLuint legacy_buffer = 0;
};

struct gpu_buffer_fence_t {
    uint64_t generation = 0;
    GLsync sync = nullptr;
    uint64_t last_poll_epoch = 0;
};

struct gpu_buffer_ring_stats_t {
    unsigned long long attempts = 0;
    unsigned long long promotions = 0;
    unsigned long long direct_maps = 0;
    unsigned long long bytes = 0;
    unsigned long long rotations = 0;
    unsigned long long allocations = 0;
    unsigned long long reuses = 0;
    unsigned long long cool_updates = 0;
    unsigned long long busy_fallbacks = 0;
    unsigned long long unsafe = 0;
    unsigned long long shared_context = 0;
    unsigned long long fences = 0;
    unsigned long long signaled = 0;
    unsigned long long pending = 0;
    unsigned long long fence_failures = 0;
};
#endif

#if defined(ZOMDROID_GL_BREADCRUMBS)
// First-use breadcrumbs go quiet after an entry point has been seen. Map/unmap
// can fail on a later invocation, so retain a small ordered enter/exit window
// without turning every draw into file I/O.
std::atomic<unsigned int> g_zomdroid_buffer_trace_seq{0};
constexpr unsigned int k_zomdroid_buffer_trace_limit = 256;

void trace_zomdroid_buffer_call(const char* phase, GLenum target, GLintptr offset, GLsizeiptr length,
                                GLbitfield access, const void* result) {
    const unsigned int seq = g_zomdroid_buffer_trace_seq.fetch_add(1, std::memory_order_relaxed) + 1;
    if (seq > k_zomdroid_buffer_trace_limit) return;
    write_log("ZOMDROID_GL_BUFFER %u %s target=0x%x offset=%lld length=%lld access=0x%x result=%p", seq, phase,
              target, static_cast<long long>(offset), static_cast<long long>(length), access, result);
}
#else
void trace_zomdroid_buffer_call(const char*, GLenum, GLintptr, GLsizeiptr, GLbitfield, const void*) {}
#endif

struct buffer_group_state_t { // shared across a share group
    std::vector<GLuint> gen_buffers;
    std::vector<char> gen_buffer_exists;
    std::vector<GLuint> free_buffer_ids;
    std::vector<size_t> buffer_datasize;
    std::vector<uint64_t> buffer_lifetimes;
#if defined(ZOMDROID_EXPERIMENTAL)
    std::vector<GLenum> buffer_usage;
    std::vector<buffer_storage_kind_t> buffer_storage_kind;
    std::vector<char> gpu_ring_safe;
    ska::flat_hash_map<GLuint, buffer_staging_map_t> buffer_staging_maps;
    ska::flat_hash_map<GLuint, gpu_buffer_ring_t> gpu_buffer_rings;
    unsigned int context_count = 0;
    bool multiple_contexts_seen = false;
#endif
};

struct buffer_ctx_state_t { // private to one context
    std::vector<GLuint> gen_arrays;
    std::vector<char> gen_array_exists;
    std::vector<GLuint> free_array_ids;
    std::vector<buffer_identity_t> element_array_buffer_per_vao;
    std::array<GLuint, 13> bound_buffers{};
    GLuint bound_array = 0;
    GLuint driver_bound_array = 0;
    bool driver_bound_array_known = false;
#if defined(ZOMDROID_EXPERIMENTAL)
    buffer_group_state_t* group = nullptr;
    unsigned long long context_id = 0;
    uint64_t gpu_use_generation = 1;
    uint64_t gpu_completed_generation = 0;
    uint64_t gpu_poll_epoch = 1;
    bool gpu_generation_has_draw = false;
    bool gpu_sync_failed = false;
    std::vector<gpu_buffer_fence_t> gpu_fences;
    ska::flat_hash_map<GLuint, vertex_array_state_t> vertex_array_states;
    std::vector<client_attrib_snapshot_t> client_attrib_stack;
    buffer_streaming_stats_t buffer_streaming_stats;
    buffer_discard_coalesce_stats_t buffer_discard_coalesce_stats;
    gpu_buffer_ring_stats_t gpu_buffer_ring_stats;
    unsigned long long client_attrib_push_hits = 0;
    unsigned long long client_attrib_pop_hits = 0;
    unsigned long long client_attrib_restore_hits = 0;
#endif
#if defined(ZOMDROID_GL_BREADCRUMBS)
    unsigned long long ebo_lifetime_guard_hits = 0;
    unsigned long long map_size_fastpath_hits = 0;
    unsigned long long buffer_streaming_map_hits = 0;
#endif
};

std::mutex g_buf_mutex;
// The tables hold their state by pointer. A thread_local pointer into an entry is
// the whole point of the design -- the ~90 access sites read through g_bg/g_bc
// rather than looking anything up -- and the map moves its elements when it
// grows, so the entry itself must not be what moves. The unique_ptr stays put
// while the map rehashes around it.
ska::flat_hash_map<unsigned long long, std::unique_ptr<buffer_group_state_t>> g_buf_groups;
ska::flat_hash_map<unsigned long long, std::unique_ptr<buffer_ctx_state_t>> g_buf_ctxs;

buffer_group_state_t g_buf_group_default;
buffer_ctx_state_t g_buf_ctx_default;

thread_local buffer_group_state_t* g_bg = &g_buf_group_default;
thread_local buffer_ctx_state_t* g_bc = &g_buf_ctx_default;

} // namespace

void mg_buffer_bind_context(unsigned long long ctx_id, unsigned long long group_id) {
    if (ctx_id == 0) {
        g_bg = &g_buf_group_default;
        g_bc = &g_buf_ctx_default;
        return;
    }
    std::lock_guard<std::mutex> lock(g_buf_mutex);
    std::unique_ptr<buffer_group_state_t>& group = g_buf_groups[group_id];
    if (!group) group = std::make_unique<buffer_group_state_t>();
    std::unique_ptr<buffer_ctx_state_t>& ctx = g_buf_ctxs[ctx_id];
    if (!ctx) {
        ctx = std::make_unique<buffer_ctx_state_t>();
#if defined(ZOMDROID_EXPERIMENTAL)
        ctx->group = group.get();
        ctx->context_id = ctx_id;
        ++group->context_count;
        if (group->context_count > 1) group->multiple_contexts_seen = true;
#endif
    }
    g_bg = group.get();
    g_bc = ctx.get();
}

void mg_buffer_forget_context(unsigned long long ctx_id) {
    if (ctx_id == 0) return;
    std::lock_guard<std::mutex> lock(g_buf_mutex);
    const auto it = g_buf_ctxs.find(ctx_id);
    if (it == g_buf_ctxs.end()) return;
#if defined(ZOMDROID_EXPERIMENTAL)
    if (g_bc == it->second.get() && GLES.glDeleteSync) {
        for (const gpu_buffer_fence_t& fence : it->second->gpu_fences)
            if (fence.sync) GLES.glDeleteSync(fence.sync);
    }
    if (it->second->group != nullptr && it->second->group->context_count != 0)
        --it->second->group->context_count;
#endif
    if (g_bc == it->second.get()) g_bc = &g_buf_ctx_default;
    g_buf_ctxs.erase(it);
}

void mg_driver_vertex_array_bound(GLuint driver_array) {
    g_bc->driver_bound_array = driver_array;
    g_bc->driver_bound_array_known = true;
}

void mg_driver_vertex_array_unknown() {
    g_bc->driver_bound_array_known = false;
}

#define g_gen_buffers (g_bg->gen_buffers)
#define g_gen_buffer_exists (g_bg->gen_buffer_exists)
#define g_free_buffer_ids (g_bg->free_buffer_ids)
#define g_buffer_datasize (g_bg->buffer_datasize)
#define g_buffer_lifetimes (g_bg->buffer_lifetimes)
#if defined(ZOMDROID_EXPERIMENTAL)
#define g_buffer_usage (g_bg->buffer_usage)
#define g_buffer_storage_kind (g_bg->buffer_storage_kind)
#define g_gpu_ring_safe (g_bg->gpu_ring_safe)
#define g_buffer_staging_maps (g_bg->buffer_staging_maps)
#define g_gpu_buffer_rings (g_bg->gpu_buffer_rings)
#endif
#define g_gen_arrays (g_bc->gen_arrays)
#define g_gen_array_exists (g_bc->gen_array_exists)
#define g_free_array_ids (g_bc->free_array_ids)
#define g_element_array_buffer_per_vao (g_bc->element_array_buffer_per_vao)
#define g_bound_array (g_bc->bound_array)

enum BindingIndex : int {
    BI_ARRAY_BUFFER = 0,
    BI_ATOMIC_COUNTER,
    BI_COPY_READ,
    BI_COPY_WRITE,
    BI_DRAW_INDIRECT,
    BI_DISPATCH_INDIRECT,
    BI_ELEMENT_ARRAY,
    BI_PIXEL_PACK,
    BI_PIXEL_UNPACK,
    BI_SHADER_STORAGE,
    BI_TRANSFORM_FEEDBACK,
    BI_UNIFORM_BUFFER,
    BI_PARAMETER_BUFFER,
    BINDING_COUNT
};
#define g_bound_buffers_arr (g_bc->bound_buffers)
static_assert(BINDING_COUNT == 13, "buffer_ctx_state_t::bound_buffers must match BindingIndex");

static inline int ensure_buffer_capacity(GLuint id);
GLuint get_ibo_by_vao(GLuint vao);

#if defined(ZOMDROID_EXPERIMENTAL)
static vertex_array_state_t& current_vertex_array_state() {
    return g_bc->vertex_array_states[g_bound_array];
}

static bool same_vertex_binding(const vertex_binding_state_t& a, const vertex_binding_state_t& b) {
    return a.buffer == b.buffer && a.buffer_lifetime == b.buffer_lifetime && a.offset == b.offset &&
           a.stride == b.stride && a.divisor == b.divisor && a.configured == b.configured;
}

static bool same_vertex_attrib(const vertex_attrib_state_t& a, const vertex_attrib_state_t& b) {
    return a.enabled == b.enabled && a.size == b.size && a.type == b.type && a.normalized == b.normalized &&
           a.stride == b.stride && a.pointer == b.pointer && a.buffer == b.buffer && a.divisor == b.divisor &&
           a.buffer_lifetime == b.buffer_lifetime && a.binding == b.binding &&
           a.relative_offset == b.relative_offset && a.integer == b.integer && a.configured == b.configured &&
           a.uses_binding_model == b.uses_binding_model;
}

static GLuint driver_buffer_name(GLuint frontend_name) {
    if (frontend_name == 0) return 0;
    if (!has_buffer(frontend_name)) return frontend_name;
    const GLuint real = find_real_buffer(frontend_name);
    return real != 0 ? real : frontend_name;
}

static uint64_t frontend_buffer_lifetime(GLuint buffer) {
    return buffer < g_buffer_lifetimes.size() ? g_buffer_lifetimes[buffer] : 0;
}

static bool frontend_buffer_identity_alive(GLuint buffer, uint64_t lifetime) {
    return buffer != 0 && has_buffer(buffer) && lifetime != 0 && frontend_buffer_lifetime(buffer) == lifetime;
}

static void record_buffer_storage(GLuint buffer, GLsizeiptr size, GLenum usage, buffer_storage_kind_t kind);

static bool ring_driver_vao_matches_frontend() {
    if (!g_bc->driver_bound_array_known) return false;
    if (g_bound_array == 0) return g_bc->driver_bound_array == 0;
    if (!has_array(g_bound_array)) return false;
    const GLuint real_array = find_real_array(g_bound_array);
    return real_array != 0 && g_bc->driver_bound_array == real_array;
}

static void refresh_bound_vao_backings() {
    if (!mg_pz_buffer_streaming_active || !ring_driver_vao_matches_frontend()) return;
    vertex_array_state_t& state = current_vertex_array_state();

    const GLuint element = get_ibo_by_vao(g_bound_array);
    const GLuint wanted_element = driver_buffer_name(element);
    if (state.driver_element_buffer != wanted_element) {
        GLES.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, wanted_element);
        state.driver_element_buffer = wanted_element;
    }

    const GLuint restore_array = driver_buffer_name(find_bound_buffer_by_target(GL_ARRAY_BUFFER));
    GLuint driver_array = restore_array;
    bool array_binding_changed = false;
    for (GLuint i = 0; i < kTrackedVertexAttribs; ++i) {
        vertex_attrib_state_t& attrib = state.attribs[i];
        if (!attrib.configured || attrib.uses_binding_model || attrib.buffer == 0 ||
            !frontend_buffer_identity_alive(attrib.buffer, attrib.buffer_lifetime))
            continue;
        const GLuint wanted = driver_buffer_name(attrib.buffer);
        if (attrib.driver_buffer == wanted) continue;
        if (driver_array != wanted) {
            GLES.glBindBuffer(GL_ARRAY_BUFFER, wanted);
            driver_array = wanted;
            array_binding_changed = true;
        }
        const void* pointer = reinterpret_cast<const void*>(attrib.pointer);
        if (attrib.integer)
            GLES.glVertexAttribIPointer(i, attrib.size, attrib.type, attrib.stride, pointer);
        else
            GLES.glVertexAttribPointer(i, attrib.size, attrib.type, attrib.normalized, attrib.stride, pointer);
        attrib.driver_buffer = wanted;
    }

    if (GLES.glBindVertexBuffer) {
        for (GLuint i = 0; i < kTrackedVertexAttribs; ++i) {
            vertex_binding_state_t& binding = state.bindings[i];
            if (!binding.configured || binding.buffer == 0 ||
                !frontend_buffer_identity_alive(binding.buffer, binding.buffer_lifetime))
                continue;
            const GLuint wanted = driver_buffer_name(binding.buffer);
            if (binding.driver_buffer == wanted) continue;
            GLES.glBindVertexBuffer(i, wanted, binding.offset, binding.stride);
            binding.driver_buffer = wanted;
        }
    }

    if (array_binding_changed && driver_array != restore_array) GLES.glBindBuffer(GL_ARRAY_BUFFER, restore_array);
}

// -2 reuses the current backing, -1 means all three are still busy, otherwise
// the returned index selects an alternate retired or new slot.
static int choose_gpu_ring_slot(const gpu_buffer_ring_t& ring,
                                const std::array<bool, kGpuBufferRingDepth>& retired) {
    if (ring.count == 0 || ring.current >= ring.count || retired[ring.current]) return -2;
    for (uint8_t i = 0; i < ring.count; ++i) {
        if (i != ring.current && retired[i]) return i;
    }
    return ring.count < kGpuBufferRingDepth ? ring.count : -1;
}

static void retire_gpu_fences_through(uint64_t generation) {
    auto& fences = g_bc->gpu_fences;
    for (const gpu_buffer_fence_t& fence : fences) {
        if (fence.generation > generation) break;
        if (fence.sync) GLES.glDeleteSync(fence.sync);
    }
    fences.erase(std::remove_if(fences.begin(), fences.end(), [generation](const gpu_buffer_fence_t& fence) {
                     return fence.generation <= generation;
                 }),
                 fences.end());
    g_bc->gpu_completed_generation = std::max(g_bc->gpu_completed_generation, generation);
}

static bool gpu_generation_retired(uint64_t generation) {
    if (generation == 0 || generation <= g_bc->gpu_completed_generation) return true;
    const auto found = std::find_if(g_bc->gpu_fences.begin(), g_bc->gpu_fences.end(),
                                    [generation](const gpu_buffer_fence_t& fence) {
                                        return fence.generation == generation;
                                    });
    if (found == g_bc->gpu_fences.end() || !found->sync) return false;
    if (found->last_poll_epoch == g_bc->gpu_poll_epoch) return false;
    found->last_poll_epoch = g_bc->gpu_poll_epoch;
    const GLenum status = GLES.glClientWaitSync(found->sync, 0, 0);
    if (status == GL_ALREADY_SIGNALED || status == GL_CONDITION_SATISFIED) {
        ++g_bc->gpu_buffer_ring_stats.signaled;
        retire_gpu_fences_through(generation);
        return true;
    }
    if (status == GL_TIMEOUT_EXPIRED)
        ++g_bc->gpu_buffer_ring_stats.pending;
    else {
        ++g_bc->gpu_buffer_ring_stats.fence_failures;
        g_bc->gpu_sync_failed = true;
    }
    return false;
}

static bool seal_gpu_draw_generation() {
    if (!g_bc->gpu_generation_has_draw) return true;
    if (g_bc->gpu_sync_failed || !GLES.glFenceSync || !GLES.glClientWaitSync || !GLES.glDeleteSync) return false;
    GLsync sync = GLES.glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    if (!sync) {
        ++g_bc->gpu_buffer_ring_stats.fence_failures;
        g_bc->gpu_sync_failed = true;
        return false;
    }
    try {
        g_bc->gpu_fences.push_back({g_bc->gpu_use_generation, sync});
    } catch (const std::bad_alloc&) {
        GLES.glDeleteSync(sync);
        ++g_bc->gpu_buffer_ring_stats.fence_failures;
        g_bc->gpu_sync_failed = true;
        return false;
    }
    ++g_bc->gpu_poll_epoch;
    if (g_bc->gpu_poll_epoch == 0) ++g_bc->gpu_poll_epoch;
    ++g_bc->gpu_buffer_ring_stats.fences;
    ++g_bc->gpu_use_generation;
    if (g_bc->gpu_use_generation == 0) ++g_bc->gpu_use_generation;
    g_bc->gpu_generation_has_draw = false;
    return true;
}

static bool gpu_ring_trace_milestone(unsigned long long attempts) {
    return attempts <= 8 || attempts == 1024 || attempts == 65536;
}

static void trace_gpu_ring(const char* result, GLuint buffer, GLuint driver_buffer) {
#if defined(ZOMDROID_GL_BREADCRUMBS)
    const gpu_buffer_ring_stats_t& stats = g_bc->gpu_buffer_ring_stats;
    if (gpu_ring_trace_milestone(stats.attempts)) {
        write_log("ZOMDROID_PZ_PERSISTENT_BUFFER_STREAM attempt=%llu promoted=%llu direct=%llu bytes=%llu "
                  "rotations=%llu allocations=%llu reuse=%llu retired=%llu waits=%llu unsupported=%llu "
                  "shared=%llu fences=%llu signaled=%llu pending=%llu fence_fail=%llu buffer=%u backing=%u "
                  "generation=%llu result=%s",
                  stats.attempts, stats.promotions, stats.direct_maps, stats.bytes, stats.rotations,
                  stats.allocations, stats.reuses, stats.cool_updates, stats.busy_fallbacks, stats.unsafe,
                  stats.shared_context, stats.fences, stats.signaled, stats.pending, stats.fence_failures, buffer,
                  driver_buffer,
                  static_cast<unsigned long long>(g_bc->gpu_use_generation), result);
    }
#else
    (void)result;
    (void)buffer;
    (void)driver_buffer;
#endif
}

static bool gpu_ring_target(GLenum target) {
    return target == GL_ARRAY_BUFFER || target == GL_ELEMENT_ARRAY_BUFFER;
}

static void mark_gpu_ring_role(GLuint buffer, GLenum target) {
    if (!mg_pz_buffer_streaming_active || buffer == 0 || !has_buffer(buffer)) return;
    ensure_buffer_capacity(buffer);
    if (!gpu_ring_target(target)) g_gpu_ring_safe[buffer] = 0;
}

static bool persistent_stream_capable(GLenum target, GLuint buffer, GLsizeiptr size) {
    return mg_pz_buffer_streaming_active && gpu_ring_target(target) && buffer != 0 && has_buffer(buffer) && size > 0 &&
           buffer < g_gpu_ring_safe.size() && g_gpu_ring_safe[buffer] != 0 && !g_bg->multiple_contexts_seen &&
           g_bc->context_id != 0 && g_gles_caps.GL_EXT_buffer_storage && GLES.glBufferStorageEXT &&
           GLES.glMapBufferRange && GLES.glMemoryBarrier && GLES.glFenceSync && GLES.glClientWaitSync &&
           GLES.glDeleteSync;
}

static bool allocate_persistent_backing(GLenum target, GLsizeiptr size, gpu_buffer_ring_slot_t* slot) {
    GLuint created = 0;
    GLES.glGenBuffers(1, &created);
    if (created == 0) return false;
    GLES.glBindBuffer(target, created);
    const GLbitfield storage_flags =
        GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT | GL_DYNAMIC_STORAGE_BIT;
    GLES.glBufferStorageEXT(target, size, nullptr, storage_flags);
    void* pointer = GLES.glMapBufferRange(
        target, 0, size, GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
    if (pointer == nullptr) {
        GLES.glDeleteBuffers(1, &created);
        return false;
    }
    slot->driver_buffer = created;
    slot->pointer = pointer;
    slot->size = size;
    slot->last_use_generation = 0;
    return true;
}

static void force_complete_gpu_work() {
    GLES.glFinish();
    for (const gpu_buffer_fence_t& fence : g_bc->gpu_fences)
        if (fence.sync) GLES.glDeleteSync(fence.sync);
    g_bc->gpu_fences.clear();
    g_bc->gpu_completed_generation = g_bc->gpu_use_generation;
    g_bc->gpu_generation_has_draw = false;
}

static void* select_persistent_backing(GLenum target, GLuint buffer, GLsizeiptr size) {
    gpu_buffer_ring_stats_t& stats = g_bc->gpu_buffer_ring_stats;
    ++stats.attempts;
    const GLuint current = driver_buffer_name(buffer);
    if (!persistent_stream_capable(target, buffer, size)) {
        if (g_bg->multiple_contexts_seen || g_bc->context_id == 0)
            ++stats.shared_context;
        else
            ++stats.unsafe;
        trace_gpu_ring("unsupported", buffer, current);
        return nullptr;
    }

    auto found = g_gpu_buffer_rings.find(buffer);
    if (found == g_gpu_buffer_rings.end() || found->second.count == 0) {
        ++stats.unsafe;
        trace_gpu_ring("missing_ring", buffer, current);
        return nullptr;
    }
    gpu_buffer_ring_t& ring = found->second;
    if (!seal_gpu_draw_generation()) {
        ++stats.busy_fallbacks;
        force_complete_gpu_work();
    }

    std::array<bool, kGpuBufferRingDepth> retired{};
    for (uint8_t i = 0; i < ring.count; ++i)
        retired[i] = gpu_generation_retired(ring.slots[i].last_use_generation);
    int selected = choose_gpu_ring_slot(ring, retired);
    if (selected < -1) {
        ++stats.cool_updates;
        selected = ring.current;
    } else if (selected < 0) {
        // Four frames still using all four slots means the GPU is genuinely
        // behind. A one-time finish preserves correctness instead of overwriting
        // memory still referenced by queued draws.
        ++stats.busy_fallbacks;
        force_complete_gpu_work();
        selected = ring.current;
    }

    const uint8_t slot_index = static_cast<uint8_t>(selected);
    if (slot_index == ring.count) {
        const GLuint restore = current;
        if (!allocate_persistent_backing(target, size, &ring.slots[slot_index])) {
            GLES.glBindBuffer(target, restore);
            ++stats.unsafe;
            trace_gpu_ring("allocation_failed", buffer, current);
            return nullptr;
        }
        ++ring.count;
        ++stats.allocations;
    } else if (slot_index != ring.current) {
        GLES.glBindBuffer(target, ring.slots[slot_index].driver_buffer);
        ++stats.reuses;
    }

    if (slot_index != ring.current) {
        ring.current = slot_index;
        ++stats.rotations;
        modify_buffer(buffer, ring.slots[slot_index].driver_buffer);
        refresh_bound_vao_backings();
    }
    ++stats.direct_maps;
    stats.bytes += static_cast<unsigned long long>(size);
    trace_gpu_ring("direct_map", buffer, ring.slots[slot_index].driver_buffer);
    return ring.slots[slot_index].pointer;
}

static bool promote_persistent_stream(GLenum target, GLuint buffer, GLsizeiptr size, const void* data) {
    gpu_buffer_ring_stats_t& stats = g_bc->gpu_buffer_ring_stats;
    ++stats.attempts;
    const GLuint previous = driver_buffer_name(buffer);
    if (!persistent_stream_capable(target, buffer, size)) {
        if (g_bg->multiple_contexts_seen || g_bc->context_id == 0)
            ++stats.shared_context;
        else
            ++stats.unsafe;
        trace_gpu_ring("promotion_unsupported", buffer, previous);
        return false;
    }

    gpu_buffer_ring_t ring{};
    ring.legacy_buffer = previous;
    if (!allocate_persistent_backing(target, size, &ring.slots[0])) {
        GLES.glBindBuffer(target, previous);
        ++stats.unsafe;
        trace_gpu_ring("promotion_failed", buffer, previous);
        return false;
    }
    ring.count = 1;
    ring.current = 0;
    std::memcpy(ring.slots[0].pointer, data, static_cast<size_t>(size));
    GLES.glMemoryBarrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT);
    g_gpu_buffer_rings[buffer] = ring;
    modify_buffer(buffer, ring.slots[0].driver_buffer);
    refresh_bound_vao_backings();
    record_buffer_storage(buffer, size, g_buffer_usage[buffer], buffer_storage_kind_t::persistent_stream);
    ++stats.promotions;
    stats.bytes += static_cast<unsigned long long>(size);
    trace_gpu_ring("promoted", buffer, ring.slots[0].driver_buffer);
    return true;
}

static bool delete_gpu_ring_backings(GLuint buffer) {
    const auto found = g_gpu_buffer_rings.find(buffer);
    if (found == g_gpu_buffer_rings.end()) return false;
    std::array<GLuint, kGpuBufferRingDepth + 1> names{};
    GLsizei count = 0;
    for (uint8_t i = 0; i < found->second.count; ++i) {
        const GLuint name = found->second.slots[i].driver_buffer;
        if (name != 0) names[count++] = name;
    }
    if (found->second.legacy_buffer != 0) names[count++] = found->second.legacy_buffer;
    if (count != 0) GLES.glDeleteBuffers(count, names.data());
    g_gpu_buffer_rings.erase(found);
    return count != 0;
}

static void note_gpu_ring_buffer_use(GLuint buffer) {
    if (buffer == 0) return;
    const auto found = g_gpu_buffer_rings.find(buffer);
    if (found == g_gpu_buffer_rings.end()) return;
    gpu_buffer_ring_t& ring = found->second;
    const GLuint current = driver_buffer_name(buffer);
    for (uint8_t i = 0; i < ring.count; ++i) {
        if (ring.slots[i].driver_buffer != current) continue;
        ring.current = i;
        ring.slots[i].last_use_generation = g_bc->gpu_use_generation;
        g_bc->gpu_generation_has_draw = true;
        return;
    }
}

void mg_pz_persistent_buffer_note_draw() {
    if (!mg_pz_buffer_streaming_active || g_bc->context_id == 0) return;
    vertex_array_state_t& state = current_vertex_array_state();
    for (const vertex_attrib_state_t& attrib : state.attribs) {
        if (attrib.enabled != GL_TRUE || !attrib.configured) continue;
        if (attrib.uses_binding_model) {
            if (attrib.binding < kTrackedVertexAttribs) note_gpu_ring_buffer_use(state.bindings[attrib.binding].buffer);
        } else {
            note_gpu_ring_buffer_use(attrib.buffer);
        }
    }
    note_gpu_ring_buffer_use(get_ibo_by_vao(g_bound_array));
}

static bool client_attrib_trace_milestone(unsigned long long hits) {
    return hits == 1 || hits == 1024 || hits == 65536;
}

#if defined(MOBILEGLUES_TESTING)
int mg_test_choose_gpu_ring_slot(const bool* retired, size_t count, size_t current) {
    gpu_buffer_ring_t ring;
    ring.count = static_cast<uint8_t>(std::min(count, kGpuBufferRingDepth));
    ring.current = static_cast<uint8_t>(current);
    std::array<bool, kGpuBufferRingDepth> states{};
    for (size_t i = 0; i < ring.count; ++i) states[i] = retired[i];
    return choose_gpu_ring_slot(ring, states);
}
#endif
#endif

static inline int ensure_buffer_capacity(GLuint id) {
    if ((int)g_gen_buffers.size() <= (int)id) {
        g_gen_buffers.resize(id + 1, 0);
        g_gen_buffer_exists.resize(id + 1, 0);
        if (g_buffer_datasize.size() <= (size_t)id) g_buffer_datasize.resize(id + 1, 0);
        if (g_buffer_lifetimes.size() <= (size_t)id) g_buffer_lifetimes.resize(id + 1, 0);
#if defined(ZOMDROID_EXPERIMENTAL)
        if (g_buffer_usage.size() <= (size_t)id) g_buffer_usage.resize(id + 1, GL_STATIC_DRAW);
        if (g_buffer_storage_kind.size() <= (size_t)id)
            g_buffer_storage_kind.resize(id + 1, buffer_storage_kind_t::none);
        if (g_gpu_ring_safe.size() <= (size_t)id) g_gpu_ring_safe.resize(id + 1, 1);
#endif
    }
    return 0;
}

static inline int ensure_array_capacity(GLuint id) {
    if ((int)g_gen_arrays.size() <= (int)id) {
        g_gen_arrays.resize(id + 1, 0);
        g_gen_array_exists.resize(id + 1, 0);
        if (g_element_array_buffer_per_vao.size() <= (size_t)id)
            g_element_array_buffer_per_vao.resize(id + 1);
    }
    return 0;
}

static uint64_t begin_buffer_lifetime(GLuint id) {
    ensure_buffer_capacity(id);
    uint64_t& lifetime = g_buffer_lifetimes[id];
    ++lifetime;
    // Zero identifies a name that was never allocated by this frontend.  Keep
    // it reserved even after the practically unreachable 64-bit wraparound.
    if (lifetime == 0) ++lifetime;
    return lifetime;
}

GLuint gen_buffer() {
    if (!g_free_buffer_ids.empty()) {
        GLuint id = g_free_buffer_ids.back();
        g_free_buffer_ids.pop_back();
        ensure_buffer_capacity(id);
        g_gen_buffers[id] = 0;
        g_gen_buffer_exists[id] = 1;
        g_buffer_datasize[id] = 0;
#if defined(ZOMDROID_EXPERIMENTAL)
        g_buffer_usage[id] = GL_STATIC_DRAW;
        g_buffer_storage_kind[id] = buffer_storage_kind_t::none;
        g_gpu_ring_safe[id] = 1;
        g_buffer_staging_maps.erase(id);
        g_gpu_buffer_rings.erase(id);
#endif
        begin_buffer_lifetime(id);
        if (id > (GLuint)maxBufferId) maxBufferId = id;
        return id;
    }
    maxBufferId++;
    ensure_buffer_capacity((GLuint)maxBufferId);
    g_gen_buffers[maxBufferId] = 0;
    g_gen_buffer_exists[maxBufferId] = 1;
    g_buffer_datasize[maxBufferId] = 0;
#if defined(ZOMDROID_EXPERIMENTAL)
    g_buffer_usage[maxBufferId] = GL_STATIC_DRAW;
    g_buffer_storage_kind[maxBufferId] = buffer_storage_kind_t::none;
    g_gpu_ring_safe[maxBufferId] = 1;
#endif
    begin_buffer_lifetime((GLuint)maxBufferId);
    return (GLuint)maxBufferId;
}

GLboolean has_buffer(GLuint key) {
    return key < g_gen_buffer_exists.size() ? (g_gen_buffer_exists[key] != 0) : 0;
}

void modify_buffer(GLuint key, GLuint value) {
    if (key >= g_gen_buffers.size()) ensure_buffer_capacity(key);
    g_gen_buffers[key] = value;
    if (key >= g_gen_buffer_exists.size()) g_gen_buffer_exists.resize(key + 1, 0);
    g_gen_buffer_exists[key] = 1;
}

void remove_buffer(GLuint key) {
    if (key < g_gen_buffer_exists.size() && g_gen_buffer_exists[key]) {
        g_gen_buffer_exists[key] = 0;
        g_gen_buffers[key] = 0;
        if (key < g_buffer_datasize.size()) g_buffer_datasize[key] = 0;
#if defined(ZOMDROID_EXPERIMENTAL)
        if (key < g_buffer_storage_kind.size()) g_buffer_storage_kind[key] = buffer_storage_kind_t::none;
        if (key < g_gpu_ring_safe.size()) g_gpu_ring_safe[key] = 1;
        g_buffer_staging_maps.erase(key);
        g_gpu_buffer_rings.erase(key);
#endif
        g_free_buffer_ids.push_back(key);
    }
}

GLuint find_real_buffer(GLuint key) {
    if (key < g_gen_buffers.size() && g_gen_buffer_exists[key]) return g_gen_buffers[key];
    return 0;
}

GLuint get_ibo_by_vao(GLuint vao) {
    if (vao >= g_element_array_buffer_per_vao.size()) return 0;

    const buffer_identity_t& binding = g_element_array_buffer_per_vao[vao];
    if (binding.name == 0) return 0;

    // Names bound without first passing through glGenBuffers have no frontend
    // lifetime.  Preserve that legacy pass-through until a generated object
    // takes the same name; at that point treating it as the old object would be
    // precisely the alias this guard exists to reject.
    if (binding.lifetime == 0) return has_buffer(binding.name) ? 0 : binding.name;

    const bool alive = has_buffer(binding.name);
    const uint64_t current = binding.name < g_buffer_lifetimes.size() ? g_buffer_lifetimes[binding.name] : 0;
    if (alive && current == binding.lifetime) return binding.name;

#if defined(ZOMDROID_GL_BREADCRUMBS)
    ++g_bc->ebo_lifetime_guard_hits;
    const unsigned long long hit = g_bc->ebo_lifetime_guard_hits;
    if (hit == 1 || hit == 1024 || hit == 65536) {
        write_log("ZOMDROID_EBO_LIFETIME_GUARD vao=%u stale_name=%u saved_lifetime=%llu current_lifetime=%llu "
                  "alive=%d semantic_applied=1 hit=%llu",
                  vao, binding.name, static_cast<unsigned long long>(binding.lifetime),
                  static_cast<unsigned long long>(current), alive ? 1 : 0, hit);
    }
#endif
    return 0;
}

GLuint find_bound_array() {
    return g_bound_array;
}

void update_vao_ibo_binding(GLuint vao, GLuint ibo) {
    ensure_array_capacity(vao);
    buffer_identity_t& binding = g_element_array_buffer_per_vao[vao];
    binding.name = ibo;
    binding.lifetime = (ibo != 0 && has_buffer(ibo) && ibo < g_buffer_lifetimes.size())
                           ? g_buffer_lifetimes[ibo]
                           : 0;
}

void set_buffer_data_size(GLuint buffer, size_t size) {
    ensure_buffer_capacity(buffer);
    g_buffer_datasize[buffer] = size;
}

size_t get_buffer_data_size(GLuint buffer) {
    if (buffer < g_buffer_datasize.size()) return g_buffer_datasize[buffer];
    return 0;
}

bool get_known_buffer_data_size(GLuint buffer, GLsizeiptr* size) {
    if (size == nullptr || buffer == 0 || !has_buffer(buffer) || buffer >= g_buffer_datasize.size()) return false;
    const size_t tracked = g_buffer_datasize[buffer];
    if (tracked == 0 || tracked > static_cast<size_t>(std::numeric_limits<GLsizeiptr>::max())) return false;
    *size = static_cast<GLsizeiptr>(tracked);
    return true;
}

#if defined(ZOMDROID_EXPERIMENTAL)
static void record_buffer_storage(GLuint buffer, GLsizeiptr size, GLenum usage, buffer_storage_kind_t kind) {
    if (buffer == 0 || !has_buffer(buffer) || size < 0) return;
    ensure_buffer_capacity(buffer);
    g_buffer_datasize[buffer] = static_cast<size_t>(size);
    g_buffer_usage[buffer] = usage;
    g_buffer_storage_kind[buffer] = kind;
}

static bool staging_map_active(GLuint buffer) {
    const auto found = g_buffer_staging_maps.find(buffer);
    return found != g_buffer_staging_maps.end() && found->second.mapped;
}

static void trace_buffer_streaming_pattern(const buffer_streaming_stats_t& stats, GLuint buffer, GLintptr offset,
                                           GLsizeiptr length, GLsizeiptr tracked_size, GLbitfield access,
                                           const char* result) {
#if defined(ZOMDROID_GL_BREADCRUMBS)
    if (stats.attempts <= 8 || stats.attempts == 1024 || stats.attempts == 65536) {
        write_log("ZOMDROID_PZ_BUFFER_STREAMING_PATTERN attempt=%llu hits=%llu miss_access=%llu miss_size=%llu "
                  "miss_storage=%llu miss_busy=%llu alloc_fail=%llu buffer=%u offset=%lld length=%lld tracked=%lld "
                  "access=0x%x result=%s",
                  stats.attempts, stats.hits, stats.miss_access, stats.miss_size, stats.miss_storage, stats.miss_busy,
                  stats.allocation_failures, buffer, static_cast<long long>(offset), static_cast<long long>(length),
                  static_cast<long long>(tracked_size), access, result);
    }
#else
    (void)stats;
    (void)buffer;
    (void)offset;
    (void)length;
    (void)tracked_size;
    (void)access;
    (void)result;
#endif
}

static void* try_staging_map(GLenum target, GLuint buffer, GLintptr offset, GLsizeiptr length, GLbitfield access,
                             bool* handled) {
    *handled = false;
    if (!mg_pz_buffer_streaming_active) return nullptr;

    buffer_streaming_stats_t& stats = g_bc->buffer_streaming_stats;
    ++stats.attempts;
    GLsizeiptr tracked_size = 0;
    const bool size_known = get_known_buffer_data_size(buffer, &tracked_size);
    const bool invalidated = (access & (GL_MAP_INVALIDATE_BUFFER_BIT | GL_MAP_INVALIDATE_RANGE_BIT)) != 0;

    if (offset != 0 || length <= 0 || (access & GL_MAP_WRITE_BIT) == 0 || (access & GL_MAP_READ_BIT) != 0 ||
        !invalidated || (access & (GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT)) != 0) {
        ++stats.miss_access;
        const char* reason = offset != 0                                      ? "offset"
                             : length <= 0                                    ? "length"
                             : (access & GL_MAP_WRITE_BIT) == 0                ? "not_write"
                             : (access & GL_MAP_READ_BIT) != 0                 ? "read"
                             : !invalidated                                  ? "no_invalidate"
                                                                              : "persistent_or_coherent";
        trace_buffer_streaming_pattern(stats, buffer, offset, length, tracked_size, access, reason);
        return nullptr;
    }

    if (!size_known || tracked_size != length) {
        ++stats.miss_size;
        trace_buffer_streaming_pattern(stats, buffer, offset, length, tracked_size, access,
                                       size_known ? "partial_size" : "unknown_size");
        return nullptr;
    }

    auto existing = g_buffer_staging_maps.find(buffer);
    if (existing != g_buffer_staging_maps.end() && existing->second.mapped) {
        ++stats.miss_busy;
        trace_buffer_streaming_pattern(stats, buffer, offset, length, tracked_size, access, "already_mapped");
        mg_set_gl_error(GL_INVALID_OPERATION);
        *handled = true;
        return nullptr;
    }

    const buffer_storage_kind_t kind = buffer < g_buffer_storage_kind.size()
                                           ? g_buffer_storage_kind[buffer]
                                           : buffer_storage_kind_t::none;
    if (kind == buffer_storage_kind_t::persistent_stream) {
        void* pointer = select_persistent_backing(target, buffer, length);
        *handled = true;
        if (pointer == nullptr) {
            ++stats.allocation_failures;
            mg_set_gl_error(GL_OUT_OF_MEMORY);
            trace_buffer_streaming_pattern(stats, buffer, offset, length, tracked_size, access,
                                           "persistent_unavailable");
            return nullptr;
        }
        buffer_staging_map_t& staging = g_buffer_staging_maps[buffer];
        staging.pointer = pointer;
        staging.size = length;
        staging.access = access;
        staging.mapped = true;
        staging.persistent_direct = true;
        ++stats.hits;
        trace_buffer_streaming_pattern(stats, buffer, offset, length, tracked_size, access, "persistent_direct");
        MG_PZ_CENSUS(mg_pz_census_buffer_map(length));
        return pointer;
    }
    if (kind != buffer_storage_kind_t::mutable_store) {
        ++stats.miss_storage;
        trace_buffer_streaming_pattern(stats, buffer, offset, length, tracked_size, access, "not_mutable");
        return nullptr;
    }

    constexpr size_t kMapAlignment = 64;
    const size_t bytes = static_cast<size_t>(length);
    if (bytes > std::numeric_limits<size_t>::max() - (kMapAlignment - 1)) return nullptr;

    try {
        buffer_staging_map_t& staging = g_buffer_staging_maps[buffer];
        staging.storage.resize(bytes + kMapAlignment - 1);
        const uintptr_t base = reinterpret_cast<uintptr_t>(staging.storage.data());
        const uintptr_t aligned = (base + kMapAlignment - 1) & ~(uintptr_t{kMapAlignment - 1});
        staging.pointer = reinterpret_cast<void*>(aligned);
        staging.size = length;
        staging.access = access;
        staging.mapped = true;
        staging.persistent_direct = false;
        *handled = true;
        ++stats.hits;
        trace_buffer_streaming_pattern(stats, buffer, offset, length, tracked_size, access, "hit");
        MG_PZ_CENSUS(mg_pz_census_buffer_map(length));
#if defined(ZOMDROID_GL_BREADCRUMBS)
        ++g_bc->buffer_streaming_map_hits;
        const unsigned long long hit = g_bc->buffer_streaming_map_hits;
        if (hit == 1 || hit == 1024 || hit == 65536) {
            write_log("ZOMDROID_PZ_BUFFER_STREAMING_MAP buffer=%u bytes=%lld cpu_staging=1 hit=%llu", buffer,
                      static_cast<long long>(length), hit);
        }
#endif
        return staging.pointer;
    } catch (const std::bad_alloc&) {
        // Allocation pressure must retain the normal driver mapping path.
        ++stats.allocation_failures;
        trace_buffer_streaming_pattern(stats, buffer, offset, length, tracked_size, access, "allocation_failure");
        return nullptr;
    }
}

static void trace_discard_coalesce(GLuint buffer, GLsizeiptr size, const char* result,
                                   unsigned long long event_count) {
#if defined(ZOMDROID_GL_BREADCRUMBS)
    const buffer_discard_coalesce_stats_t& stats = g_bc->buffer_discard_coalesce_stats;
    if (event_count == 1 || event_count == 1024 || event_count == 65536) {
        write_log("ZOMDROID_PZ_BUFFER_DISCARD_COALESCE elided=%llu paired=%llu fallback=%llu "
                  "superseded=%llu bytes=%llu buffer=%u size=%lld result=%s",
                  stats.elided, stats.paired, stats.fallbacks, stats.superseded, stats.bytes, buffer,
                  static_cast<long long>(size), result);
    }
#else
    (void)buffer;
    (void)size;
    (void)result;
    (void)event_count;
#endif
}

static bool try_elide_buffer_discard(GLenum target, GLuint buffer, GLsizeiptr size, const void* data, GLenum usage) {
    if (!mg_pz_buffer_discard_coalesce_active || !mg_pz_buffer_streaming_active || data != nullptr || size <= 0 ||
        (target != GL_ARRAY_BUFFER && target != GL_ELEMENT_ARRAY_BUFFER) || buffer == 0 || !has_buffer(buffer) ||
        buffer >= g_buffer_datasize.size() || g_buffer_datasize[buffer] != static_cast<size_t>(size) ||
        buffer >= g_buffer_usage.size() || g_buffer_usage[buffer] != usage || buffer >= g_buffer_storage_kind.size() ||
        (g_buffer_storage_kind[buffer] != buffer_storage_kind_t::mutable_store &&
         g_buffer_storage_kind[buffer] != buffer_storage_kind_t::persistent_stream))
        return false;

    const auto found = g_buffer_staging_maps.find(buffer);
    if (found == g_buffer_staging_maps.end() || found->second.mapped || !found->second.completed_upload) return false;

    buffer_staging_map_t& staging = found->second;
    staging.discard_elided = true;
    staging.discard_size = size;
    staging.discard_usage = usage;
    buffer_discard_coalesce_stats_t& stats = g_bc->buffer_discard_coalesce_stats;
    ++stats.elided;
    stats.bytes += static_cast<unsigned long long>(size);
    trace_discard_coalesce(buffer, size, "deferred_to_staged_upload", stats.elided);
    return true;
}

static void flush_elided_discard(GLenum target, GLuint buffer) {
    const auto found = g_buffer_staging_maps.find(buffer);
    if (found == g_buffer_staging_maps.end() || !found->second.discard_elided) return;
    buffer_staging_map_t& staging = found->second;
    const bool persistent = buffer < g_buffer_storage_kind.size() &&
                            g_buffer_storage_kind[buffer] == buffer_storage_kind_t::persistent_stream;
    if (!persistent) GLES.glBufferData(target, staging.discard_size, nullptr, staging.discard_usage);
    staging.discard_elided = false;
    ++g_bc->buffer_discard_coalesce_stats.fallbacks;
    trace_discard_coalesce(buffer, staging.discard_size, "fallback",
                           g_bc->buffer_discard_coalesce_stats.fallbacks);
}

#if defined(MOBILEGLUES_TESTING)
void mg_test_record_buffer_storage(GLuint buffer, GLsizeiptr size, GLenum usage, bool immutable) {
    record_buffer_storage(buffer, size, usage,
                          immutable ? buffer_storage_kind_t::immutable_store : buffer_storage_kind_t::mutable_store);
}

void* mg_test_try_staging_map(GLuint buffer, GLintptr offset, GLsizeiptr length, GLbitfield access, bool* handled) {
    return try_staging_map(GL_ARRAY_BUFFER, buffer, offset, length, access, handled);
}

void mg_test_cancel_staging_map(GLuint buffer) {
    const auto found = g_buffer_staging_maps.find(buffer);
    if (found == g_buffer_staging_maps.end()) return;
    found->second.mapped = false;
    found->second.pointer = nullptr;
}

void mg_test_complete_staging_upload(GLuint buffer) {
    const auto found = g_buffer_staging_maps.find(buffer);
    if (found == g_buffer_staging_maps.end()) return;
    found->second.mapped = false;
    found->second.pointer = nullptr;
    found->second.completed_upload = true;
}

bool mg_test_try_elide_buffer_discard(GLenum target, GLuint buffer, GLsizeiptr size, GLenum usage) {
    return try_elide_buffer_discard(target, buffer, size, nullptr, usage);
}

bool mg_test_buffer_discard_is_elided(GLuint buffer) {
    const auto found = g_buffer_staging_maps.find(buffer);
    return found != g_buffer_staging_maps.end() && found->second.discard_elided;
}
#endif
#endif

static inline int binding_target_to_index(GLenum target) {
    switch (target) {
    case GL_ARRAY_BUFFER:
        return BI_ARRAY_BUFFER;
    case GL_ATOMIC_COUNTER_BUFFER:
        return BI_ATOMIC_COUNTER;
    case GL_COPY_READ_BUFFER:
        return BI_COPY_READ;
    case GL_COPY_WRITE_BUFFER:
        return BI_COPY_WRITE;
    case GL_DRAW_INDIRECT_BUFFER:
        return BI_DRAW_INDIRECT;
    case GL_DISPATCH_INDIRECT_BUFFER:
        return BI_DISPATCH_INDIRECT;
    case GL_ELEMENT_ARRAY_BUFFER:
        return BI_ELEMENT_ARRAY;
    case GL_PIXEL_PACK_BUFFER:
        return BI_PIXEL_PACK;
    case GL_PIXEL_UNPACK_BUFFER:
        return BI_PIXEL_UNPACK;
    case GL_SHADER_STORAGE_BUFFER:
        return BI_SHADER_STORAGE;
    case GL_TRANSFORM_FEEDBACK_BUFFER:
        return BI_TRANSFORM_FEEDBACK;
    case GL_UNIFORM_BUFFER:
        return BI_UNIFORM_BUFFER;
    case GL_PARAMETER_BUFFER:
        return BI_PARAMETER_BUFFER;
    default:
        return -1;
    }
}

void set_bound_buffer_by_target(GLenum target, GLuint buffer) {
    int idx = binding_target_to_index(target);
    if (idx >= 0) g_bound_buffers_arr[idx] = buffer;
}

// find_bound_buffer below answers the *_BINDING query enums, which is what
// glGetIntegerv passes it. Callers holding a bind target need this one instead:
// handing a target to find_bound_buffer falls through to its default and comes
// back 0, which is a valid buffer name and so goes unnoticed.
GLuint find_bound_buffer_by_target(GLenum target) {
    if (target == GL_ELEMENT_ARRAY_BUFFER) return get_ibo_by_vao(find_bound_array());
    const int idx = binding_target_to_index(target);
    return idx >= 0 ? g_bound_buffers_arr[idx] : 0;
}

// The name the *driver* has bound to `target`, i.e. what
// GLES.glGetIntegerv(<target>_BINDING) would answer, worked out from the tracked
// bindings rather than by asking the driver.
//
// Buffer names are renamed across this boundary: the application sees names
// gen_buffer() hands out and the driver sees the ones glGenBuffers gave back, so
// find_bound_buffer_by_target's answer must not be passed to GLES.glBindBuffer
// unmapped. This one may. A name the application bound without ever generating it
// is forwarded verbatim by glBindBuffer -- GLES creates the object on first bind
// -- so it is its own driver name.
//
// Two limits, both shared with this layer's own glGetIntegerv:
//   - It reports the last name bound to the target, and glDeleteBuffers does not
//     clear the binding slots (only GL_PARAMETER_BUFFER, which has no driver-side
//     binding to fall back on). Deleting a still-bound buffer resets the driver's
//     binding to 0 while this keeps reporting the dead name.
//   - It is the tracked state, so it is only the driver's state where the two
//     agree. Every internal path that binds GL_ELEMENT_ARRAY_BUFFER or
//     GL_DRAW_INDIRECT_BUFFER through GLES.* directly (gl/multidraw.cpp,
//     gl/drawing.cpp, gl/restart.cpp) saves and restores around its own work, so
//     they disagree only inside those windows -- ask before the temporary bind,
//     never during it. gl/gl.cpp's depth-clear triangle is the one path that does
//     not: it leaves the driver on vertex array 0 and GL_ARRAY_BUFFER 0 without
//     putting the application's back, which desynchronises the element array
//     binding too, since that is vertex array state.
//
// GL_PARAMETER_BUFFER has no GLES binding at all; the mapped name is returned for
// it anyway, because gl/multidraw.cpp is the only thing that asks and it needs the
// real object to bind somewhere else.
GLuint mg_driver_bound_buffer(GLenum target) {
    const GLuint name = find_bound_buffer_by_target(target);
    const GLuint real = (name == 0 || !has_buffer(name)) ? name : find_real_buffer(name);
#if GLOBAL_DEBUG
    // The divergence this answer is vulnerable to -- driver state mutated
    // behind the frontend's back -- is undetectable at runtime: the tracked
    // state always has an answer and cannot know it is stale. So debug builds
    // pay the round-trip this function exists to avoid, and scream on a
    // mismatch instead of letting a wrong binding surface three calls later as
    // a skipped draw or a corrupted restore. Release builds trust the tracking.
    if (GLES.glGetIntegerv) {
        GLenum pname = 0;
        switch (target) {
        case GL_ARRAY_BUFFER:          pname = GL_ARRAY_BUFFER_BINDING; break;
        case GL_ELEMENT_ARRAY_BUFFER:  pname = GL_ELEMENT_ARRAY_BUFFER_BINDING; break;
        case GL_DRAW_INDIRECT_BUFFER:  pname = GL_DRAW_INDIRECT_BUFFER_BINDING; break;
        case GL_PIXEL_UNPACK_BUFFER:   pname = GL_PIXEL_UNPACK_BUFFER_BINDING; break;
        case GL_PIXEL_PACK_BUFFER:     pname = GL_PIXEL_PACK_BUFFER_BINDING; break;
        case GL_COPY_READ_BUFFER:      pname = GL_COPY_READ_BUFFER_BINDING; break;
        case GL_COPY_WRITE_BUFFER:     pname = GL_COPY_WRITE_BUFFER_BINDING; break;
        default: break;
        }
        if (pname != 0) {
            GLint driver = 0;
            GLES.glGetIntegerv(pname, &driver);
            if (static_cast<GLuint>(driver) != real) {
                LOG_E("mg_driver_bound_buffer(0x%X): tracked %u (real %u) but the driver holds %u -- "
                      "something mutated this binding without going through the frontend",
                      target, name, real, static_cast<GLuint>(driver))
            }
        }
    }
#endif
    return real;
}

GLuint find_bound_buffer(GLenum key) {
    GLenum target = 0;
    switch (key) {
    case GL_ARRAY_BUFFER_BINDING:
        target = GL_ARRAY_BUFFER;
        break;
    case GL_ATOMIC_COUNTER_BUFFER_BINDING:
        target = GL_ATOMIC_COUNTER_BUFFER;
        break;
    case GL_COPY_READ_BUFFER_BINDING:
        target = GL_COPY_READ_BUFFER;
        break;
    case GL_COPY_WRITE_BUFFER_BINDING:
        target = GL_COPY_WRITE_BUFFER;
        break;
    case GL_DRAW_INDIRECT_BUFFER_BINDING:
        target = GL_DRAW_INDIRECT_BUFFER;
        break;
    case GL_DISPATCH_INDIRECT_BUFFER_BINDING:
        target = GL_DISPATCH_INDIRECT_BUFFER;
        break;
    case GL_ELEMENT_ARRAY_BUFFER_BINDING:
        target = GL_ELEMENT_ARRAY_BUFFER;
        break;
    case GL_PIXEL_PACK_BUFFER_BINDING:
        target = GL_PIXEL_PACK_BUFFER;
        break;
    case GL_PIXEL_UNPACK_BUFFER_BINDING:
        target = GL_PIXEL_UNPACK_BUFFER;
        break;
    case GL_SHADER_STORAGE_BUFFER_BINDING:
        target = GL_SHADER_STORAGE_BUFFER;
        break;
    case GL_TRANSFORM_FEEDBACK_BUFFER_BINDING:
        target = GL_TRANSFORM_FEEDBACK_BUFFER;
        break;
    case GL_UNIFORM_BUFFER_BINDING:
        target = GL_UNIFORM_BUFFER;
        break;
    case GL_PARAMETER_BUFFER_BINDING:
        target = GL_PARAMETER_BUFFER;
        break;
    default:
        target = 0;
        break;
    }
    if (target == GL_ELEMENT_ARRAY_BUFFER) {
        return get_ibo_by_vao(find_bound_array());
    }
    int idx = binding_target_to_index(target);
    if (idx >= 0) return g_bound_buffers_arr[idx];
    return 0;
}

GLuint gen_array() {
    if (!g_free_array_ids.empty()) {
        GLuint id = g_free_array_ids.back();
        g_free_array_ids.pop_back();
        ensure_array_capacity(id);
        g_gen_arrays[id] = 0;
        g_gen_array_exists[id] = 1;
        g_element_array_buffer_per_vao[id] = {};
        if (id > (GLuint)maxArrayId) maxArrayId = id;
        return id;
    }
    maxArrayId++;
    ensure_array_capacity((GLuint)maxArrayId);
    g_gen_arrays[maxArrayId] = 0;
    g_gen_array_exists[maxArrayId] = 1;
    g_element_array_buffer_per_vao[maxArrayId] = {};
    return (GLuint)maxArrayId;
}

GLboolean has_array(GLuint key) {
    return key < g_gen_array_exists.size() ? (g_gen_array_exists[key] != 0) : 0;
}

void modify_array(GLuint key, GLuint value) {
    if (key >= g_gen_arrays.size()) ensure_array_capacity(key);
    g_gen_arrays[key] = value;
    if (key >= g_gen_array_exists.size()) g_gen_array_exists.resize(key + 1, 0);
    g_gen_array_exists[key] = 1;
}

void remove_array(GLuint key) {
    if (key < g_gen_array_exists.size() && g_gen_array_exists[key]) {
        g_gen_array_exists[key] = 0;
        g_gen_arrays[key] = 0;
        if (key < g_element_array_buffer_per_vao.size()) g_element_array_buffer_per_vao[key] = {};
        g_free_array_ids.push_back(key);
    }
#if defined(ZOMDROID_EXPERIMENTAL)
    g_bc->vertex_array_states.erase(key);
#endif
}

GLuint find_real_array(GLuint key) {
    if (key < g_gen_arrays.size() && g_gen_array_exists[key]) return g_gen_arrays[key];
    return 0;
}

static GLenum get_binding_query(GLenum target) {
    switch (target) {
    case GL_ARRAY_BUFFER:
        return GL_ARRAY_BUFFER_BINDING;
    case GL_ELEMENT_ARRAY_BUFFER:
        return GL_ELEMENT_ARRAY_BUFFER_BINDING;
    case GL_PIXEL_PACK_BUFFER:
        return GL_PIXEL_PACK_BUFFER_BINDING;
    case GL_PIXEL_UNPACK_BUFFER:
        return GL_PIXEL_UNPACK_BUFFER_BINDING;
    case GL_COPY_WRITE_BUFFER:
        return GL_COPY_WRITE_BUFFER_BINDING;
    case GL_COPY_READ_BUFFER:
        return GL_COPY_READ_BUFFER_BINDING;
    case GL_UNIFORM_BUFFER:
        return GL_UNIFORM_BUFFER_BINDING;
    case GL_SHADER_STORAGE_BUFFER:
        return GL_SHADER_STORAGE_BUFFER_BINDING;
    case GL_TRANSFORM_FEEDBACK_BUFFER:
        return GL_TRANSFORM_FEEDBACK_BUFFER_BINDING;
    case GL_ATOMIC_COUNTER_BUFFER:
        return GL_ATOMIC_COUNTER_BUFFER_BINDING;
    case GL_DRAW_INDIRECT_BUFFER:
        return GL_DRAW_INDIRECT_BUFFER_BINDING;
    case GL_DISPATCH_INDIRECT_BUFFER:
        return GL_DISPATCH_INDIRECT_BUFFER_BINDING;
    default:
        return 0;
    }
}

void InitBufferMap(size_t expectedSize) {
    g_gen_buffers.reserve(expectedSize + 2);
    g_gen_buffer_exists.reserve(expectedSize + 2);
    g_buffer_datasize.reserve(expectedSize + 2);
    g_buffer_lifetimes.reserve(expectedSize + 2);
#if defined(ZOMDROID_EXPERIMENTAL)
    g_buffer_usage.reserve(expectedSize + 2);
    g_buffer_storage_kind.reserve(expectedSize + 2);
    g_gpu_ring_safe.reserve(expectedSize + 2);
    g_buffer_staging_maps.reserve(expectedSize + 2);
    g_gpu_buffer_rings.reserve(expectedSize + 2);
#endif
    g_gen_buffers.resize(1, 0);
    g_gen_buffer_exists.resize(1, 0);
    g_buffer_datasize.resize(1, 0);
    g_buffer_lifetimes.resize(1, 0);
#if defined(ZOMDROID_EXPERIMENTAL)
    g_buffer_usage.resize(1, GL_STATIC_DRAW);
    g_buffer_storage_kind.resize(1, buffer_storage_kind_t::none);
    g_gpu_ring_safe.resize(1, 1);
#endif
}

void InitVertexArrayMap(size_t expectedSize) {
    g_gen_arrays.reserve(expectedSize + 2);
    g_gen_array_exists.reserve(expectedSize + 2);
    g_element_array_buffer_per_vao.reserve(expectedSize + 2);
    g_gen_arrays.resize(1, 0);
    g_gen_array_exists.resize(1, 0);
    g_element_array_buffer_per_vao.resize(1);
}

void glGenBuffers(GLsizei n, GLuint* buffers) {
    LOG()
    LOG_D("glGenBuffers(%i, %p)", n, buffers)
    for (int i = 0; i < n; ++i) {
        buffers[i] = gen_buffer();
    }
}

void glDeleteBuffers(GLsizei n, const GLuint* buffers) {
    LOG()
    LOG_D("glDeleteBuffers(%i, %p)", n, buffers)
    for (int i = 0; i < n; ++i) {
        // GL resets a binding to 0 when the bound buffer is deleted. The
        // parameter buffer slot is the only source of truth gl/multidraw.cpp has
        // for where the draw count lives -- there is no driver-side binding to
        // cross-check it against -- and deleted ids are recycled by gen_buffer(),
        // so a stale slot would silently point at somebody else's buffer.
        if (buffers[i] != 0 && find_bound_buffer(GL_PARAMETER_BUFFER_BINDING) == buffers[i]) {
            set_bound_buffer_by_target(GL_PARAMETER_BUFFER, 0);
        }
        bool ring_deleted = false;
#if defined(ZOMDROID_EXPERIMENTAL)
        ring_deleted = delete_gpu_ring_backings(buffers[i]);
#endif
        if (!ring_deleted && find_real_buffer(buffers[i])) {
            GLuint real_buff = find_real_buffer(buffers[i]);
            GLES.glDeleteBuffers(1, &real_buff);
            CHECK_GL_ERROR
        }
        remove_buffer(buffers[i]);
    }
}

GLboolean glIsBuffer(GLuint buffer) {
    LOG()
    LOG_D("glIsBuffer, buffer = %d", buffer)
    return has_buffer(buffer);
}

void glBindBuffer(GLenum target, GLuint buffer) {
    LOG()
    LOG_D("glBindBuffer, target = %s, buffer = %d", glEnumToString(target), buffer)
    MG_PZ_CENSUS(mg_pz_census_bind_buffer(find_bound_buffer_by_target(target) == buffer));
    set_bound_buffer_by_target(target, buffer);
#if defined(ZOMDROID_EXPERIMENTAL)
    mark_gpu_ring_role(buffer, target);
#endif

    if (target == GL_PARAMETER_BUFFER) {
        // GLES has no GL_PARAMETER_BUFFER. The binding is tracked here and read
        // back by gl/multidraw.cpp for glMultiDraw*IndirectCount; forwarding the
        // target to the driver would only raise GL_INVALID_ENUM. The backing
        // object still has to exist, because nothing else will create it.
        if (buffer != 0 && has_buffer(buffer) && !find_real_buffer(buffer)) {
            GLuint real_buffer = 0;
            GLES.glGenBuffers(1, &real_buffer);
            modify_buffer(buffer, real_buffer);
            CHECK_GL_ERROR
        }
        return;
    }

    // save ibo binding to vao
    if (target == GL_ELEMENT_ARRAY_BUFFER) {
        update_vao_ibo_binding(find_bound_array(), buffer);
    }

    if (!has_buffer(buffer) || buffer == 0) {
        GLES.glBindBuffer(target, buffer);
#if defined(ZOMDROID_EXPERIMENTAL)
        if (target == GL_ELEMENT_ARRAY_BUFFER) current_vertex_array_state().driver_element_buffer = buffer;
#endif
        CHECK_GL_ERROR
        return;
    }
    GLuint real_buffer = find_real_buffer(buffer);
    if (!real_buffer) {
        GLES.glGenBuffers(1, &real_buffer);
        modify_buffer(buffer, real_buffer);
        CHECK_GL_ERROR
    }
    LOG_D("glBindBuffer: %d -> %d", buffer, real_buffer)
    GLES.glBindBuffer(target, real_buffer);
#if defined(ZOMDROID_EXPERIMENTAL)
    if (target == GL_ELEMENT_ARRAY_BUFFER) current_vertex_array_state().driver_element_buffer = real_buffer;
#endif
    CHECK_GL_ERROR
}

static std::vector<GLuint> g_buffer_map_ssbo_id;

void glBindBufferRange(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size) {
    LOG()
    LOG_D("glBindBufferRange, target = %s, index = %d, buffer = %d, offset = %p, size = %zi", glEnumToString(target),
          index, buffer, (void*)offset, size)
#if defined(ZOMDROID_EXPERIMENTAL)
    mark_gpu_ring_role(buffer, target);
#endif

    if (!has_buffer(buffer) || buffer == 0) {
        GLES.glBindBufferRange(target, index, buffer, offset, size);
        CHECK_GL_ERROR
        return;
    }
    GLuint real_buffer = find_real_buffer(buffer);
    if (!real_buffer) {
        GLES.glGenBuffers(1, &real_buffer);
        modify_buffer(buffer, real_buffer);
        CHECK_GL_ERROR
    }
    GLES.glBindBufferRange(target, index, real_buffer, offset, size);
    CHECK_GL_ERROR
}

void glBindBufferBase(GLenum target, GLuint index, GLuint buffer) {
    LOG()
    LOG_D("glBindBufferBase, target = %s, index = %d, buffer = %d", glEnumToString(target), index, buffer)
#if defined(ZOMDROID_EXPERIMENTAL)
    mark_gpu_ring_role(buffer, target);
#endif

    if (!has_buffer(buffer) || buffer == 0) {
        GLES.glBindBufferBase(target, index, buffer);
        CHECK_GL_ERROR
        return;
    }
    GLuint real_buffer = find_real_buffer(buffer);
    if (!real_buffer) {
        GLES.glGenBuffers(1, &real_buffer);
        modify_buffer(buffer, real_buffer);
        CHECK_GL_ERROR
    }
    GLES.glBindBufferBase(target, index, real_buffer);
    if (target == GL_SHADER_STORAGE_BUFFER) {
        if (g_buffer_map_ssbo_id.empty()) {
            g_buffer_map_ssbo_id.resize(GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, 0);
        }
        g_buffer_map_ssbo_id[index] = buffer;
    }
    CHECK_GL_ERROR
}

void glBindVertexBuffer(GLuint bindingindex, GLuint buffer, GLintptr offset, GLsizei stride) {
    LOG()
    LOG_D("glBindVertexBuffer, bindingindex = %d, buffer = %d, offset = %p, stride = %i", bindingindex, buffer, offset,
          stride)
#if defined(ZOMDROID_EXPERIMENTAL)
    const uint64_t lifetime = frontend_buffer_lifetime(buffer);
    if (mg_pz_census_active) {
        const bool tracked = bindingindex < kTrackedVertexAttribs;
        const bool exact = tracked && current_vertex_array_state().bindings[bindingindex].configured &&
                           current_vertex_array_state().bindings[bindingindex].buffer == buffer &&
                           current_vertex_array_state().bindings[bindingindex].buffer_lifetime == lifetime &&
                           current_vertex_array_state().bindings[bindingindex].offset == offset &&
                           current_vertex_array_state().bindings[bindingindex].stride == stride;
        mg_pz_census_attrib(mg_pz_attrib_kind::vertex_buffer, tracked, exact);
    }
    if (bindingindex < kTrackedVertexAttribs) {
        auto& binding = current_vertex_array_state().bindings[bindingindex];
        binding.buffer = buffer;
        binding.buffer_lifetime = lifetime;
        binding.driver_buffer = driver_buffer_name(buffer);
        binding.offset = offset;
        binding.stride = stride;
        binding.configured = true;
    }
#endif
    if (!has_buffer(buffer) || buffer == 0) {
        GLES.glBindVertexBuffer(bindingindex, buffer, offset, stride);
#if defined(ZOMDROID_EXPERIMENTAL)
        if (bindingindex < kTrackedVertexAttribs)
            current_vertex_array_state().bindings[bindingindex].driver_buffer = buffer;
#endif
        CHECK_GL_ERROR
        return;
    }
    GLuint real_buffer = find_real_buffer(buffer);
    if (!real_buffer) {
        GLES.glGenBuffers(1, &real_buffer);
        modify_buffer(buffer, real_buffer);
        CHECK_GL_ERROR
    }
    GLES.glBindVertexBuffer(bindingindex, real_buffer, offset, stride);
#if defined(ZOMDROID_EXPERIMENTAL)
    if (bindingindex < kTrackedVertexAttribs) current_vertex_array_state().bindings[bindingindex].driver_buffer = real_buffer;
#endif
    CHECK_GL_ERROR
}

// The transfer pair GLES accepts for a given sized internalformat.
//
// The texture-buffer emulation used to allocate and upload with a hardcoded
// GL_RED_INTEGER + GL_BYTE whatever the internalformat was. ES validates
// internalformat/format/type as a triple, and that one is legal for exactly one
// format -- GL_R8I. Everything else (GL_R32I, GL_RGBA32F, even GL_R8UI, which
// wants GL_UNSIGNED_BYTE) failed the glTexImage2D with GL_INVALID_OPERATION,
// left level 0 undefined, and the emulated texelFetch read zeros from an
// incomplete texture. Sized here from the same table as the texel size, so the
// two cannot drift apart.
//
// Returns false for a format with no ES-legal pair -- the normalised 16-bit ones
// need EXT_texture_norm16, and depth formats are not texture-buffer formats at
// all. The caller drops the call instead of guessing.
//
// Deliberately wider than GL 4.6 table 8.16, which lists only the 32-bit
// three-component forms among the RGB ones: the extra entries here (GL_RGB8,
// GL_RGB8I/UI, GL_RGB16I/UI/F) are all valid ES triples, so emulating them costs
// nothing, while refusing them would only break an application that already works
// against the permissive desktop drivers. Being stricter than the hardware buys
// no correctness.
bool get_internal_format_transfer(GLenum internalformat, GLenum* format, GLenum* type) {
    switch (internalformat) {
    // clang-format off
    case GL_R8:        *format = GL_RED;           *type = GL_UNSIGNED_BYTE;  return true;
    case GL_R8I:       *format = GL_RED_INTEGER;   *type = GL_BYTE;           return true;
    case GL_R8UI:      *format = GL_RED_INTEGER;   *type = GL_UNSIGNED_BYTE;  return true;
    case GL_R16I:      *format = GL_RED_INTEGER;   *type = GL_SHORT;          return true;
    case GL_R16UI:     *format = GL_RED_INTEGER;   *type = GL_UNSIGNED_SHORT; return true;
    case GL_R16F:      *format = GL_RED;           *type = GL_HALF_FLOAT;     return true;
    case GL_R32I:      *format = GL_RED_INTEGER;   *type = GL_INT;            return true;
    case GL_R32UI:     *format = GL_RED_INTEGER;   *type = GL_UNSIGNED_INT;   return true;
    case GL_R32F:      *format = GL_RED;           *type = GL_FLOAT;          return true;

    case GL_RG8:       *format = GL_RG;            *type = GL_UNSIGNED_BYTE;  return true;
    case GL_RG8I:      *format = GL_RG_INTEGER;    *type = GL_BYTE;           return true;
    case GL_RG8UI:     *format = GL_RG_INTEGER;    *type = GL_UNSIGNED_BYTE;  return true;
    case GL_RG16I:     *format = GL_RG_INTEGER;    *type = GL_SHORT;          return true;
    case GL_RG16UI:    *format = GL_RG_INTEGER;    *type = GL_UNSIGNED_SHORT; return true;
    case GL_RG16F:     *format = GL_RG;            *type = GL_HALF_FLOAT;     return true;
    case GL_RG32I:     *format = GL_RG_INTEGER;    *type = GL_INT;            return true;
    case GL_RG32UI:    *format = GL_RG_INTEGER;    *type = GL_UNSIGNED_INT;   return true;
    case GL_RG32F:     *format = GL_RG;            *type = GL_FLOAT;          return true;

    case GL_RGB8:      *format = GL_RGB;           *type = GL_UNSIGNED_BYTE;  return true;
    case GL_RGB8I:     *format = GL_RGB_INTEGER;   *type = GL_BYTE;           return true;
    case GL_RGB8UI:    *format = GL_RGB_INTEGER;   *type = GL_UNSIGNED_BYTE;  return true;
    case GL_RGB16I:    *format = GL_RGB_INTEGER;   *type = GL_SHORT;          return true;
    case GL_RGB16UI:   *format = GL_RGB_INTEGER;   *type = GL_UNSIGNED_SHORT; return true;
    case GL_RGB16F:    *format = GL_RGB;           *type = GL_HALF_FLOAT;     return true;
    case GL_RGB32I:    *format = GL_RGB_INTEGER;   *type = GL_INT;            return true;
    case GL_RGB32UI:   *format = GL_RGB_INTEGER;   *type = GL_UNSIGNED_INT;   return true;
    case GL_RGB32F:    *format = GL_RGB;           *type = GL_FLOAT;          return true;

    case GL_RGBA8:     *format = GL_RGBA;          *type = GL_UNSIGNED_BYTE;  return true;
    case GL_RGBA8I:    *format = GL_RGBA_INTEGER;  *type = GL_BYTE;           return true;
    case GL_RGBA8UI:   *format = GL_RGBA_INTEGER;  *type = GL_UNSIGNED_BYTE;  return true;
    case GL_RGBA16I:   *format = GL_RGBA_INTEGER;  *type = GL_SHORT;          return true;
    case GL_RGBA16UI:  *format = GL_RGBA_INTEGER;  *type = GL_UNSIGNED_SHORT; return true;
    case GL_RGBA16F:   *format = GL_RGBA;          *type = GL_HALF_FLOAT;     return true;
    case GL_RGBA32I:   *format = GL_RGBA_INTEGER;  *type = GL_INT;            return true;
    case GL_RGBA32UI:  *format = GL_RGBA_INTEGER;  *type = GL_UNSIGNED_INT;   return true;
    case GL_RGBA32F:   *format = GL_RGBA;          *type = GL_FLOAT;          return true;
    // clang-format on
    default:
        return false;
    }
}

size_t get_internal_format_size(GLenum internalformat) {
    switch (internalformat) {
    case GL_R8:
        return 1;
    case GL_R8I:
    case GL_R8UI:
        return 1;
    case GL_R16:
        return 2;
    case GL_R16I:
    case GL_R16UI:
    case GL_R16F:
        return 2;
    case GL_R32I:
    case GL_R32UI:
    case GL_R32F:
        return 4;

    case GL_RG8:
        return 2;
    case GL_RG8I:
    case GL_RG8UI:
        return 2;
    case GL_RG16:
        return 4;
    case GL_RG16I:
    case GL_RG16UI:
    case GL_RG16F:
        return 4;
    case GL_RG32I:
    case GL_RG32UI:
    case GL_RG32F:
        return 8;

    case GL_RGB8:
        return 3;
    case GL_RGB8I:
    case GL_RGB8UI:
        return 3;
    case GL_RGB16:
        return 6;
    case GL_RGB16I:
    case GL_RGB16UI:
    case GL_RGB16F:
        return 6;
    case GL_RGB32I:
    case GL_RGB32UI:
    case GL_RGB32F:
        return 12;

    case GL_RGBA8:
        return 4;
    case GL_RGBA8I:
    case GL_RGBA8UI:
        return 4;
    case GL_RGBA16:
        return 8;
    case GL_RGBA16I:
    case GL_RGBA16UI:
    case GL_RGBA16F:
        return 8;
    case GL_RGBA32I:
    case GL_RGBA32UI:
    case GL_RGBA32F:
        return 16;

    case GL_DEPTH_COMPONENT16:
        return 2;
    case GL_DEPTH_COMPONENT24:
        return 3;
    case GL_DEPTH_COMPONENT32:
        return 4;
    case GL_DEPTH_COMPONENT32F:
        return 4;
    case GL_DEPTH24_STENCIL8:
        return 4;
    case GL_DEPTH32F_STENCIL8:
        return 5;

    case GL_STENCIL_INDEX8:
        return 1;

    case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
    case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
        return 8;
    case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
    case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
        return 16;

    default:
        LOG_E("Unknown internal format size for %s", glEnumToString(internalformat));
        return 0;
    }
}

extern std::string bufSampelerName;

// Report a rejected argument once per site. This layer cannot raise a GL error
// -- glGetError always answers GL_NO_ERROR by design -- so an unusable argument
// means "do nothing" plus one line a user can paste into a bug report. LOG_W and
// LOG_E compile to nothing in release builds, hence LOG_W_FORCE.
#define BU_WARN_ONCE(...)                                                                                              \
    do {                                                                                                               \
        static bool mg_bu_warned = false;                                                                              \
        if (!mg_bu_warned) {                                                                                           \
            mg_bu_warned = true;                                                                                       \
            LOG_W_FORCE(__VA_ARGS__)                                                                                   \
        }                                                                                                              \
    } while (0)

// Todo: any glGet* related to this function?
void glTexBuffer(GLenum target, GLenum internalformat, GLuint buffer) {
    LOG()
    LOG_D("glTexBuffer, target = %s, internalformat = %s, buffer = %d", glEnumToString(target),
          glEnumToString(internalformat), buffer)
    if (target != GL_TEXTURE_BUFFER) return;
#if defined(ZOMDROID_EXPERIMENTAL)
    mark_gpu_ring_role(buffer, GL_TEXTURE_BUFFER);
#endif

    if (!has_buffer(buffer) || buffer == 0) {
        GLES.glTexBuffer(target, internalformat, buffer);
        CHECK_GL_ERROR
        return;
    }
    GLuint real_buffer = find_real_buffer(buffer);
    if (!real_buffer) {
        GLES.glGenBuffers(1, &real_buffer);
        modify_buffer(buffer, real_buffer);
        CHECK_GL_ERROR
    }

    if (hardware->emulate_texture_buffer) {
        LOG_D("Emulating glTexBuffer");

        // internalformat arrives unvalidated -- a format outside GL 4.6 table 8.16
        // is GL_INVALID_ENUM in real GL and this layer raises nothing -- so
        // get_internal_format_size answers 0 for it, as its own default case says.
        // That 0 used to reach "bufferSize / pixelSize" below: undefined, and on
        // arm64 it divides to zero, giving a 0 x 1 texture that the emulated
        // texelFetch then indexes modulo zero. Size the texel first and drop the
        // call if we cannot, before any binding is disturbed.
        GLuint pixelSize = get_internal_format_size(internalformat);
        if (pixelSize == 0) {
            BU_WARN_ONCE("glTexBuffer: no texel size known for internalformat %s, texture buffer left untouched",
                         glEnumToString(internalformat));
            mg_set_gl_error(GL_INVALID_ENUM);
            return;
        }

        // The transfer pair this internalformat actually accepts. Hardcoding one
        // pair here is what made every format but GL_R8I fail to allocate.
        GLenum tb_format = GL_RED_INTEGER, tb_type = GL_BYTE;
        if (!get_internal_format_transfer(internalformat, &tb_format, &tb_type)) {
            BU_WARN_ONCE("glTexBuffer: no GLES transfer pair for internalformat %s, texture buffer left untouched",
                         glEnumToString(internalformat));
            mg_set_gl_error(GL_INVALID_ENUM);
            return;
        }

        GLint boundTexture = 0;
        GLint prev_pixel_buffer_binding = 0;

        GLES.glActiveTexture(GL_TEXTURE0 + 15);

        GLES.glGetIntegerv(GL_TEXTURE_BINDING_2D, &boundTexture);
        LOG_D("Current GL_TEXTURE_BINDING_BUFFER = %d", boundTexture);
        GLES.glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &prev_pixel_buffer_binding);
        LOG_D("Previous GL_PIXEL_UNPACK_BUFFER_BINDING = %d", prev_pixel_buffer_binding);

        if (!boundTexture) {
            LOG_D("No texture bound to GL_TEXTURE_BUFFER, skipping emulation.");
            // Unit 15 is only ever borrowed for the emulated buffer texture; every
            // other borrower hands it back. Returning from here without doing so
            // left the app's next glBindTexture landing on unit 15.
            GLES.glActiveTexture(GL_TEXTURE0 + gl_state->current_tex_unit);
            return;
        }

        GLES.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, real_buffer);
        LOG_D("Bound GL_PIXEL_UNPACK_BUFFER to buffer %u", real_buffer);

        GLint bufferSize;
        GLES.glGetBufferParameteriv(GL_PIXEL_UNPACK_BUFFER, GL_BUFFER_SIZE, &bufferSize);
        LOG_D("Buffer size = %d bytes", bufferSize);

        GLES.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

        GLES.glBindTexture(GL_TEXTURE_2D, boundTexture);
        LOG_D("Binding texture %u to GL_TEXTURE_2D", boundTexture);

        const GLuint MAX_WIDTH = 8192;
        GLuint numElements = bufferSize / pixelSize;
        if (numElements == 0) {
            // A buffer too small to hold one texel. The pixelSize == 0 guard above
            // exists because a zero-sized texture makes the emulated texelFetch
            // index modulo zero; this reaches the same place by the other road,
            // through a 0 x 1 glTexImage2D and a u_BufferTexWidth of 0.
            BU_WARN_ONCE("glTexBuffer: buffer of %d bytes holds no %u-byte texel, texture buffer left untouched",
                         bufferSize, pixelSize);
            mg_set_gl_error(GL_INVALID_VALUE);
            GLES.glActiveTexture(GL_TEXTURE0 + gl_state->current_tex_unit);
            return;
        }

        GLuint width = numElements;
        GLuint height = 1;

        if (width > MAX_WIDTH) {
            width = MAX_WIDTH;
            height = (numElements + MAX_WIDTH - 1) / MAX_WIDTH;
        }

        GLint prev_alignment, prev_row_length, prev_skip_pixels, prev_skip_rows;
        GLES.glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_alignment);
        GLES.glGetIntegerv(GL_UNPACK_ROW_LENGTH, &prev_row_length);
        GLES.glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &prev_skip_pixels);
        GLES.glGetIntegerv(GL_UNPACK_SKIP_ROWS, &prev_skip_rows);

        // why do these 2 params not work
        // GLES.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        // GLES.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0)
        GLES.glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        GLES.glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);

        // TODO: Optimize the glTexImage2D call
        GLES.glTexImage2D(GL_TEXTURE_2D, 0, internalformat, width, height, 0, tb_format, tb_type, nullptr);

        GLES.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, real_buffer);

        for (GLuint row = 0; row < height; ++row) {
            // The last row is short whenever the element count is not a multiple
            // of the row width. Asking for a full row anyway made the driver read
            // past the end of the unpack buffer, which GLES answers with
            // GL_INVALID_OPERATION and a no-op -- so the tail of the buffer was
            // never uploaded and texelFetch read it back as whatever the
            // allocation left there.
            const GLuint row_texels = (row + 1 == height) ? (numElements - row * width) : width;
            if (row_texels == 0) break;
            void* offset = (void*)(static_cast<size_t>(row) * width * pixelSize);
            GLES.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, row, row_texels, 1, tb_format, tb_type, offset);
        }

        GLES.glPixelStorei(GL_UNPACK_ALIGNMENT, prev_alignment);
        GLES.glPixelStorei(GL_UNPACK_ROW_LENGTH, prev_row_length);
        GLES.glPixelStorei(GL_UNPACK_SKIP_PIXELS, prev_skip_pixels);
        GLES.glPixelStorei(GL_UNPACK_SKIP_ROWS, prev_skip_rows);

        auto tex = mgGetTexObjectByTarget(target);
        tex->target = ConvertGLEnumToTextureTarget(target);
        tex->internal_format = internalformat;
        tex->width = width;
        tex->height = height;
        tex->depth = 1;
        tex->swizzle_param[0] = GL_RED;
        tex->swizzle_param[1] = GL_GREEN;
        tex->swizzle_param[2] = GL_BLUE;
        tex->swizzle_param[3] = GL_ALPHA;

        LOG_D("Called glTexImage2D with internalformat = 0x%X", internalformat);

        GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        LOG_D("Set texture parameters: MIN_FILTER=NEAREST, MAG_FILTER=NEAREST, WRAP_S/T=CLAMP_TO_EDGE");

        GLES.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, prev_pixel_buffer_binding);

        GLES.glActiveTexture(GL_TEXTURE0 + gl_state->current_tex_unit);

        LOG_D("Restored bindings: GL_PIXEL_UNPACK_BUFFER=%d", prev_pixel_buffer_binding);

        CHECK_GL_ERROR;
        return;
    }

    GLES.glTexBuffer(target, internalformat, real_buffer);
    CHECK_GL_ERROR
}

void glTexBufferRange(GLenum target, GLenum internalformat, GLuint buffer, GLintptr offset, GLsizeiptr size) {
    LOG()
    LOG_D("glTexBufferRange, target = %s, internalformat = %s, buffer = %d, offset = %p, size = %zi",
          glEnumToString(target), glEnumToString(internalformat), buffer, (void*)offset, size)
#if defined(ZOMDROID_EXPERIMENTAL)
    mark_gpu_ring_role(buffer, GL_TEXTURE_BUFFER);
#endif
    if (!has_buffer(buffer) || buffer == 0) {
        GLES.glTexBufferRange(target, internalformat, buffer, offset, size);
        CHECK_GL_ERROR
        return;
    }
    GLuint real_buffer = find_real_buffer(buffer);
    if (!real_buffer) {
        GLES.glGenBuffers(1, &real_buffer);
        modify_buffer(buffer, real_buffer);
        CHECK_GL_ERROR
    }
    GLES.glTexBufferRange(target, internalformat, real_buffer, offset, size);
    CHECK_GL_ERROR
}

// GLES has no GL_PARAMETER_BUFFER, so glBindBuffer above tracks the binding
// without ever handing that target to the driver. GL 4.6 still lets an
// application fill and query the buffer through it, which used to reach GLES
// verbatim and come back GL_INVALID_ENUM -- the buffer stayed empty, and
// glMultiDraw*IndirectCount then found a zero-byte parameter buffer and drew
// nothing.
//
// GL_COPY_WRITE_BUFFER is borrowed for the duration of one call and put back
// afterwards. GLES defines it as a generic target with no meaning of its own, so
// the swap is invisible: nothing observes it, and no draw depends on it.
namespace {
struct borrowed_target_t {
    GLenum target;
    GLint saved = 0;
    bool borrowed = false;

    explicit borrowed_target_t(GLenum requested) : target(requested) {
        if (requested != GL_PARAMETER_BUFFER) return;
        const GLuint real = find_real_buffer(find_bound_buffer(GL_PARAMETER_BUFFER_BINDING));
        GLES.glGetIntegerv(GL_COPY_WRITE_BUFFER_BINDING, &saved);
        GLES.glBindBuffer(GL_COPY_WRITE_BUFFER, real);
        target = GL_COPY_WRITE_BUFFER;
        borrowed = true;
    }
    ~borrowed_target_t() {
        if (borrowed) GLES.glBindBuffer(GL_COPY_WRITE_BUFFER, static_cast<GLuint>(saved));
    }

    borrowed_target_t(const borrowed_target_t&) = delete;
    borrowed_target_t& operator=(const borrowed_target_t&) = delete;
};
} // namespace

#if defined(ZOMDROID_EXPERIMENTAL)
static bool persistent_stream_storage(GLuint buffer) {
    return buffer < g_buffer_storage_kind.size() &&
           g_buffer_storage_kind[buffer] == buffer_storage_kind_t::persistent_stream;
}

static bool replace_persistent_with_mutable(GLenum target, GLuint buffer, GLsizeiptr size, const void* data,
                                            GLenum usage) {
    if (!persistent_stream_storage(buffer) || !gpu_ring_target(target)) return false;
    GLuint replacement = 0;
    GLES.glGenBuffers(1, &replacement);
    if (replacement == 0) {
        mg_set_gl_error(GL_OUT_OF_MEMORY);
        return true;
    }
    GLES.glBindBuffer(target, replacement);
    GLES.glBufferData(target, size, data, usage);
    modify_buffer(buffer, replacement);
    refresh_bound_vao_backings();
    delete_gpu_ring_backings(buffer);
    record_buffer_storage(buffer, size, usage, buffer_storage_kind_t::mutable_store);
    auto staged = g_buffer_staging_maps.find(buffer);
    if (staged != g_buffer_staging_maps.end()) {
        staged->second.mapped = false;
        staged->second.persistent_direct = false;
        staged->second.pointer = nullptr;
        staged->second.discard_elided = false;
    }
    return true;
}

static bool demote_persistent_for_driver_map(GLenum target, GLuint buffer) {
    const auto ring_it = g_gpu_buffer_rings.find(buffer);
    if (!persistent_stream_storage(buffer) || ring_it == g_gpu_buffer_rings.end() ||
        ring_it->second.current >= ring_it->second.count)
        return false;
    const gpu_buffer_ring_slot_t& slot = ring_it->second.slots[ring_it->second.current];
    const GLenum usage = buffer < g_buffer_usage.size() ? g_buffer_usage[buffer] : GL_STREAM_DRAW;
    return replace_persistent_with_mutable(target, buffer, slot.size, slot.pointer, usage);
}
#endif

void glBufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage) {
    LOG()
    MG_PZ_CENSUS(mg_pz_census_buffer_data(size, false));
    LOG_D("glBufferData, target = %s, size = %d, data = 0x%x, usage = %s", glEnumToString(target), size, data,
          glEnumToString(usage))
    const GLuint frontend_buffer = find_bound_buffer_by_target(target);
#if defined(ZOMDROID_EXPERIMENTAL)
    if (staging_map_active(frontend_buffer)) {
        mg_set_gl_error(GL_INVALID_OPERATION);
        return;
    }
    const buffer_storage_kind_t storage_kind = frontend_buffer < g_buffer_storage_kind.size()
                                                   ? g_buffer_storage_kind[frontend_buffer]
                                                   : buffer_storage_kind_t::none;
    const bool immutable_storage = storage_kind == buffer_storage_kind_t::immutable_store;
    if (!immutable_storage && try_elide_buffer_discard(target, frontend_buffer, size, data, usage)) {
        record_buffer_storage(frontend_buffer, size, usage, storage_kind);
        return;
    }
    const auto staged = g_buffer_staging_maps.find(frontend_buffer);
    if (staged != g_buffer_staging_maps.end() && staged->second.discard_elided) {
        // This new store supersedes the deferred undefined store completely.
        staged->second.discard_elided = false;
        ++g_bc->buffer_discard_coalesce_stats.superseded;
        trace_discard_coalesce(frontend_buffer, size, "superseded",
                               g_bc->buffer_discard_coalesce_stats.superseded);
    }
    if (storage_kind == buffer_storage_kind_t::persistent_stream &&
        replace_persistent_with_mutable(target, frontend_buffer, size, data, usage)) {
        CHECK_GL_ERROR
        return;
    }
#endif
    borrowed_target_t t(target);
    GLES.glBufferData(t.target, size, data, usage);
#if defined(ZOMDROID_EXPERIMENTAL)
    // glBufferData is rejected by GLES for immutable storage, so retain the
    // existing immutable record instead of making a later map eligible.
    if (!immutable_storage)
        record_buffer_storage(frontend_buffer, size, usage, buffer_storage_kind_t::mutable_store);
#else
    set_buffer_data_size(frontend_buffer, size);
#endif
    CHECK_GL_ERROR
}

// Both of these were plain pass-throughs in gl/gl_native.cpp. They live here now
// so that GL_PARAMETER_BUFFER reaches the driver as a target it understands.
void glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data) {
    LOG()
    MG_PZ_CENSUS(mg_pz_census_buffer_data(size, true));
    LOG_D("glBufferSubData, target = %s, offset = %p, size = %zi", glEnumToString(target), (void*)offset, size)
    const GLuint frontend_buffer = find_bound_buffer_by_target(target);
#if defined(ZOMDROID_EXPERIMENTAL)
    if (staging_map_active(frontend_buffer)) {
        mg_set_gl_error(GL_INVALID_OPERATION);
        return;
    }
    // A partial update needs the discard to exist first. This is an uncommon
    // deviation from the learned discard/full-map sequence and keeps its old path.
    flush_elided_discard(target, frontend_buffer);
#endif
    borrowed_target_t t(target);
    GLES.glBufferSubData(t.target, offset, size, data);
    CHECK_GL_ERROR
}

void glGetBufferParameteriv(GLenum target, GLenum pname, GLint* params) {
    LOG()
    LOG_D("glGetBufferParameteriv, target = %s, pname = %s", glEnumToString(target), glEnumToString(pname))
#if defined(ZOMDROID_EXPERIMENTAL)
    const GLuint frontend_buffer = find_bound_buffer_by_target(target);
    const auto staged = g_buffer_staging_maps.find(frontend_buffer);
    const bool persistent = persistent_stream_storage(frontend_buffer);
    if (params != nullptr && staged != g_buffer_staging_maps.end() && (staged->second.mapped || persistent)) {
        switch (pname) {
        case GL_BUFFER_MAPPED:
            *params = staged->second.mapped ? GL_TRUE : GL_FALSE;
            return;
        case GL_BUFFER_ACCESS:
            *params = GL_WRITE_ONLY;
            return;
        case GL_BUFFER_ACCESS_FLAGS:
            *params = staged->second.mapped ? static_cast<GLint>(staged->second.access) : 0;
            return;
        case GL_BUFFER_MAP_OFFSET:
            *params = 0;
            return;
        case GL_BUFFER_MAP_LENGTH:
            *params = staged->second.mapped ? static_cast<GLint>(staged->second.size) : 0;
            return;
        case GL_BUFFER_SIZE:
            *params = staged->second.size > std::numeric_limits<GLint>::max()
                          ? std::numeric_limits<GLint>::max()
                          : static_cast<GLint>(staged->second.size);
            return;
#ifdef GL_BUFFER_IMMUTABLE_STORAGE
        case GL_BUFFER_IMMUTABLE_STORAGE:
            *params = GL_FALSE;
            return;
#endif
#ifdef GL_BUFFER_STORAGE_FLAGS
        case GL_BUFFER_STORAGE_FLAGS:
            *params = 0;
            return;
#endif
        default:
            break;
        }
    }
#endif
    borrowed_target_t t(target);
    GLES.glGetBufferParameteriv(t.target, pname, params);
    CHECK_GL_ERROR
}

void glGetBufferPointerv(GLenum target, GLenum pname, void** params) {
    LOG()
#if defined(ZOMDROID_EXPERIMENTAL)
    const GLuint frontend_buffer = find_bound_buffer_by_target(target);
    const auto staged = g_buffer_staging_maps.find(frontend_buffer);
    if (params != nullptr && pname == GL_BUFFER_MAP_POINTER && staged != g_buffer_staging_maps.end() &&
        (staged->second.mapped || persistent_stream_storage(frontend_buffer))) {
        *params = staged->second.mapped ? staged->second.pointer : nullptr;
        return;
    }
#endif
    borrowed_target_t t(target);
    GLES.glGetBufferPointerv(t.target, pname, params);
    CHECK_GL_ERROR
}

void* glMapBuffer(GLenum target, GLenum access) {
    LOG()
    LOG_D("glMapBuffer, target = %s, access = %s", glEnumToString(target), glEnumToString(access))
    trace_zomdroid_buffer_call("MAP_ENTER", target, 0, 0, access, nullptr);

    // Do not mix the OES map entry point with the core unmap entry point. GLES
    // 3.x uses the core pair; OES is only a fallback for a backend without core
    // range mapping.
    if (!GLES.glMapBufferRange && g_gles_caps.GL_OES_mapbuffer && GLES.glMapBufferOES && GLES.glUnmapBufferOES) {
        borrowed_target_t t(target);
        void* ptr = GLES.glMapBufferOES(t.target, access);
        if (ptr) MG_PZ_CENSUS(mg_pz_census_buffer_map(0));
        trace_zomdroid_buffer_call("MAP_OES_EXIT", target, 0, 0, access, ptr);
        return ptr;
    }
    GLsizeiptr buffer_size = 0;
    const GLuint frontend_buffer = find_bound_buffer_by_target(target);
    const bool size_known = get_known_buffer_data_size(frontend_buffer, &buffer_size);
    if (!size_known) {
        GLint queried_size = 0;
        glGetBufferParameteriv(target, GL_BUFFER_SIZE, &queried_size);
        if (queried_size <= 0 || glGetError() != GL_NO_ERROR) return nullptr;
        buffer_size = queried_size;
#if defined(ZOMDROID_GL_BREADCRUMBS)
    } else {
        ++g_bc->map_size_fastpath_hits;
        const unsigned long long hit = g_bc->map_size_fastpath_hits;
        if (hit == 1 || hit == 1024 || hit == 65536) {
            write_log("ZOMDROID_BUFFER_MAP_SIZE_FASTPATH buffer=%u bytes=%lld driver_query_skipped=1 hit=%llu",
                      frontend_buffer, static_cast<long long>(buffer_size), hit);
        }
#endif
    }
    GLbitfield flags = 0;
    switch (access) {
    case GL_READ_ONLY:
        flags = GL_MAP_READ_BIT;
        break;
    case GL_WRITE_ONLY:
        flags = GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT;
#if defined(ZOMDROID_EXPERIMENTAL)
        if (mg_pz_buffer_streaming_active) flags |= GL_MAP_UNSYNCHRONIZED_BIT;
#endif
        break;
    case GL_READ_WRITE:
        flags = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT;
        break;
    default:
        trace_zomdroid_buffer_call("MAP_BAD_ACCESS", target, 0, buffer_size, access, nullptr);
        mg_set_gl_error(GL_INVALID_ENUM);
        return nullptr;
    }
    void* ptr = glMapBufferRange(target, 0, buffer_size, flags);
    trace_zomdroid_buffer_call("MAP_CORE_EXIT", target, 0, buffer_size, flags, ptr);
    return ptr;
}

#if GLOBAL_DEBUG || DEBUG
#include <fstream>
#define BIN_FILE_PREFIX "/sdcard/MG/buf/"
#endif

#if !defined(__APPLE__)
extern "C"
{
    GLAPI GLAPIENTRY void* glMapBufferARB(GLenum target, GLenum access) __attribute__((alias("glMapBuffer")));
    GLAPI GLAPIENTRY void glBufferDataARB(GLenum target, GLsizeiptr size, const void* data, GLenum usage)
        __attribute__((alias("glBufferData")));
    GLAPI GLAPIENTRY GLboolean glUnmapBufferARB(GLenum target) __attribute__((alias("glUnmapBuffer")));
    GLAPI GLAPIENTRY void glBufferStorageARB(GLenum target, GLsizeiptr size, const void* data, GLbitfield flags)
        __attribute__((alias("glBufferStorage")));
    GLAPI GLAPIENTRY void glBindBufferARB(GLenum target, GLuint buffer) __attribute__((alias("glBindBuffer")));
}
#endif

void* glMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access) {
    LOG()
    if (global_settings.buffer_coherent_as_flush) access &= ~GL_MAP_FLUSH_EXPLICIT_BIT;
    //    access |= GL_MAP_UNSYNCHRONIZED_BIT;
    trace_zomdroid_buffer_call("MAP_RANGE_ENTER", target, offset, length, access, nullptr);
#if defined(ZOMDROID_EXPERIMENTAL)
    const GLuint frontend_buffer = find_bound_buffer_by_target(target);
    if (staging_map_active(frontend_buffer)) {
        mg_set_gl_error(GL_INVALID_OPERATION);
        trace_zomdroid_buffer_call("MAP_STAGING_REJECT", target, offset, length, access, nullptr);
        return nullptr;
    }
    bool staging_handled = false;
    void* staging = try_staging_map(target, frontend_buffer, offset, length, access, &staging_handled);
    if (staging_handled) {
        trace_zomdroid_buffer_call(staging ? "MAP_STAGING_EXIT" : "MAP_STAGING_REJECT", target, offset, length,
                                   access, staging);
        return staging;
    }
    if (persistent_stream_storage(frontend_buffer)) demote_persistent_for_driver_map(target, frontend_buffer);
#endif
    if (!GLES.glMapBufferRange) {
        trace_zomdroid_buffer_call("MAP_RANGE_MISSING", target, offset, length, access, nullptr);
        mg_set_gl_error(GL_INVALID_OPERATION);
        return nullptr;
    }
#if defined(ZOMDROID_EXPERIMENTAL)
    // A map that missed the CPU staging gate still needs the driver's orphaning
    // operation before it can touch the store.
    flush_elided_discard(target, frontend_buffer);
#endif
    borrowed_target_t t(target);
    void* ptr = GLES.glMapBufferRange(t.target, offset, length, access);
    if (ptr) MG_PZ_CENSUS(mg_pz_census_buffer_map(length));
    trace_zomdroid_buffer_call("MAP_RANGE_EXIT", target, offset, length, access, ptr);
    return ptr;
}

GLboolean glUnmapBuffer(GLenum target) {
    LOG()
    LOG_D("%s(%s)", __func__, glEnumToString(target));
    trace_zomdroid_buffer_call("UNMAP_ENTER", target, 0, 0, 0, nullptr);
#if defined(ZOMDROID_EXPERIMENTAL)
    const GLuint frontend_buffer = find_bound_buffer_by_target(target);
    auto staged = g_buffer_staging_maps.find(frontend_buffer);
    if (staged != g_buffer_staging_maps.end() && staged->second.mapped) {
        const GLsizeiptr staged_size = staged->second.size;
        const GLbitfield staged_access = staged->second.access;
        if (staged->second.persistent_direct) {
            GLES.glMemoryBarrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT);
            if (staged->second.discard_elided) {
                staged->second.discard_elided = false;
                ++g_bc->buffer_discard_coalesce_stats.paired;
                trace_discard_coalesce(frontend_buffer, staged_size, "paired_persistent_write",
                                       g_bc->buffer_discard_coalesce_stats.paired);
            }
            staged->second.completed_upload = true;
            staged->second.mapped = false;
            staged->second.persistent_direct = false;
            staged->second.pointer = nullptr;
            trace_zomdroid_buffer_call("UNMAP_PERSISTENT_DIRECT", target, 0, staged_size, staged_access, nullptr);
            return GL_TRUE;
        }

        const bool promote = staged->second.completed_upload && staged->second.discard_elided &&
                             promote_persistent_stream(target, frontend_buffer, staged_size, staged->second.pointer);
        if (!promote) {
            borrowed_target_t t(target);
            const GLenum usage = frontend_buffer < g_buffer_usage.size() ? g_buffer_usage[frontend_buffer]
                                                                         : GL_STREAM_DRAW;
            GLES.glBufferData(t.target, staged_size, staged->second.pointer, usage);
        }
        if (staged->second.discard_elided) {
            staged->second.discard_elided = false;
            ++g_bc->buffer_discard_coalesce_stats.paired;
            trace_discard_coalesce(frontend_buffer, staged_size,
                                   promote ? "paired_persistent_promotion" : "paired_upload",
                                   g_bc->buffer_discard_coalesce_stats.paired);
        }
        staged->second.completed_upload = true;
        staged->second.mapped = false;
        staged->second.persistent_direct = false;
        staged->second.pointer = nullptr;
        trace_zomdroid_buffer_call(promote ? "UNMAP_PERSISTENT_PROMOTE" : "UNMAP_STAGING_UPLOAD", target, 0,
                                   staged_size, staged_access, nullptr);
        CHECK_GL_ERROR
        return GL_TRUE;
    }
#endif
    borrowed_target_t t(target);
    GLboolean result = GL_FALSE;
    if (!GLES.glMapBufferRange && g_gles_caps.GL_OES_mapbuffer && GLES.glUnmapBufferOES) {
        result = GLES.glUnmapBufferOES(t.target);
        trace_zomdroid_buffer_call(result ? "UNMAP_OES_EXIT_TRUE" : "UNMAP_OES_EXIT_FALSE", target, 0, 0, 0,
                                   nullptr);
        return result;
    }
    if (!GLES.glUnmapBuffer) {
        trace_zomdroid_buffer_call("UNMAP_MISSING", target, 0, 0, 0, nullptr);
        mg_set_gl_error(GL_INVALID_OPERATION);
        return GL_FALSE;
    }
    result = GLES.glUnmapBuffer(t.target);
    trace_zomdroid_buffer_call(result ? "UNMAP_CORE_EXIT_TRUE" : "UNMAP_CORE_EXIT_FALSE", target, 0, 0, 0,
                               nullptr);
    CHECK_GL_ERROR
    return result;
}

void glBufferStorage(GLenum target, GLsizeiptr size, const void* data, GLbitfield flags) {
    LOG()
    if (GLES.glBufferStorageEXT) {
        if (global_settings.buffer_coherent_as_flush &&
            ((flags & GL_MAP_PERSISTENT_BIT) != 0 || (flags & GL_DYNAMIC_STORAGE_BIT) != 0))
            flags |= (GL_MAP_WRITE_BIT | GL_MAP_COHERENT_BIT | GL_MAP_PERSISTENT_BIT);
        const GLuint frontend_buffer = find_bound_buffer_by_target(target);
#if defined(ZOMDROID_EXPERIMENTAL)
        if (staging_map_active(frontend_buffer)) {
            mg_set_gl_error(GL_INVALID_OPERATION);
            return;
        }
        const bool already_immutable = frontend_buffer < g_buffer_storage_kind.size() &&
                                       g_buffer_storage_kind[frontend_buffer] ==
                                           buffer_storage_kind_t::immutable_store;
        const auto staged = g_buffer_staging_maps.find(frontend_buffer);
        if (staged != g_buffer_staging_maps.end() && staged->second.discard_elided) {
            staged->second.discard_elided = false;
            ++g_bc->buffer_discard_coalesce_stats.superseded;
            trace_discard_coalesce(frontend_buffer, size, "superseded_by_storage",
                                   g_bc->buffer_discard_coalesce_stats.superseded);
        }
#endif
        borrowed_target_t t(target);
#if defined(ZOMDROID_EXPERIMENTAL)
        if (persistent_stream_storage(frontend_buffer)) {
            GLuint replacement = 0;
            GLES.glGenBuffers(1, &replacement);
            if (replacement == 0) {
                mg_set_gl_error(GL_OUT_OF_MEMORY);
                return;
            }
            GLES.glBindBuffer(t.target, replacement);
            GLES.glBufferStorageEXT(t.target, size, data, flags);
            modify_buffer(frontend_buffer, replacement);
            refresh_bound_vao_backings();
            delete_gpu_ring_backings(frontend_buffer);
            record_buffer_storage(frontend_buffer, size, GL_STATIC_DRAW,
                                  buffer_storage_kind_t::immutable_store);
            MG_PZ_CENSUS(mg_pz_census_buffer_data(size, false));
            CHECK_GL_ERROR
            return;
        }
#endif
        GLES.glBufferStorageEXT(t.target, size, data, flags);
        MG_PZ_CENSUS(mg_pz_census_buffer_data(size, false));
        // Allocates storage just as glBufferData does, so it owes the same record.
#if defined(ZOMDROID_EXPERIMENTAL)
        if (!already_immutable)
            record_buffer_storage(frontend_buffer, size, GL_STATIC_DRAW, buffer_storage_kind_t::immutable_store);
#else
        set_buffer_data_size(frontend_buffer, size);
#endif
    }
    CHECK_GL_ERROR
}

void glFlushMappedBufferRange(GLenum target, GLintptr offset, GLsizeiptr length) {
    LOG()
#if defined(ZOMDROID_EXPERIMENTAL)
    if (staging_map_active(find_bound_buffer_by_target(target))) return;
#endif
    if (!global_settings.buffer_coherent_as_flush) {
        borrowed_target_t t(target);
        GLES.glFlushMappedBufferRange(t.target, offset, length);
    }
}

void glGenVertexArrays(GLsizei n, GLuint* arrays) {
    LOG()
    LOG_D("glGenVertexArrays(%i, %p)", n, arrays)
    for (int i = 0; i < n; ++i) {
        arrays[i] = gen_array();
    }
}

void glDeleteVertexArrays(GLsizei n, const GLuint* arrays) {
    LOG()
    LOG_D("glDeleteVertexArrays(%i, %p)", n, arrays)
    for (int i = 0; i < n; ++i) {
        const GLuint real_array = find_real_array(arrays[i]);
        if (real_array) {
            GLES.glDeleteVertexArrays(1, &real_array);
            CHECK_GL_ERROR
            if (g_bc->driver_bound_array_known && g_bc->driver_bound_array == real_array)
                mg_driver_vertex_array_bound(0);
        }
        if (g_bound_array == arrays[i]) {
            g_bound_array = 0;
            set_bound_buffer_by_target(GL_ELEMENT_ARRAY_BUFFER, get_ibo_by_vao(0));
        }
        remove_array(arrays[i]);
    }
}

GLboolean glIsVertexArray(GLuint array) {
    LOG()
    LOG_D("glIsVertexArray(%d)", array)
    return has_array(array);
}

void glBindVertexArray(GLuint array) {
    LOG()
    LOG_D("glBindVertexArray(%d)", array)
    const bool same_frontend = g_bound_array == array;
    g_bound_array = array;

    // update bound ibo
    set_bound_buffer_by_target(GL_ELEMENT_ARRAY_BUFFER, get_ibo_by_vao(array));

    if (!has_array(array) || array == 0) {
        LOG_D("Does not have va=%d found!", array)
        const bool driver_confirmed = array == 0 && g_bc->driver_bound_array_known && g_bc->driver_bound_array == 0;
#if defined(ZOMDROID_EXPERIMENTAL)
        const bool skip = mg_pz_vao_fastpath_active && same_frontend && driver_confirmed;
#else
        const bool skip = false;
#endif
        MG_PZ_CENSUS(mg_pz_census_bind_vao(same_frontend, driver_confirmed, skip));
        if (skip) {
#if defined(ZOMDROID_EXPERIMENTAL)
            refresh_bound_vao_backings();
#endif
            return;
        }
        GLES.glBindVertexArray(array);
        if (array == 0)
            mg_driver_vertex_array_bound(0);
        else
            mg_driver_vertex_array_unknown();
#if defined(ZOMDROID_EXPERIMENTAL)
        if (array == 0) refresh_bound_vao_backings();
#endif
        CHECK_GL_ERROR
        return;
    }

    GLuint real_array = find_real_array(array);
    if (!real_array) {
        LOG_D("va=%d not initialized, initializing...", array)
        GLES.glGenVertexArrays(1, &real_array);
        modify_array(array, real_array);
        CHECK_GL_ERROR
    }
    LOG_D("glBindVertexArray: %d -> %d", array, real_array)
    const bool driver_confirmed = g_bc->driver_bound_array_known && g_bc->driver_bound_array == real_array;
#if defined(ZOMDROID_EXPERIMENTAL)
    const bool skip = mg_pz_vao_fastpath_active && same_frontend && driver_confirmed;
#else
    const bool skip = false;
#endif
    MG_PZ_CENSUS(mg_pz_census_bind_vao(same_frontend, driver_confirmed, skip));
    if (skip) {
#if defined(ZOMDROID_EXPERIMENTAL)
        refresh_bound_vao_backings();
#endif
        return;
    }
    GLES.glBindVertexArray(real_array);
    mg_driver_vertex_array_bound(real_array);
#if defined(ZOMDROID_EXPERIMENTAL)
    refresh_bound_vao_backings();
#endif
    CHECK_GL_ERROR
}

#if defined(ZOMDROID_EXPERIMENTAL)
namespace {

bool binding_used_by_model(const vertex_array_state_t& state, GLuint binding) {
    for (const auto& attrib : state.attribs) {
        if (attrib.configured && attrib.uses_binding_model && attrib.binding == binding) return true;
    }
    return false;
}

bool driver_vao_matches_frontend() {
    if (!g_bc->driver_bound_array_known) return false;
    if (g_bound_array == 0) return g_bc->driver_bound_array == 0;
    if (!has_array(g_bound_array)) return false;
    const GLuint real_array = find_real_array(g_bound_array);
    return real_array != 0 && g_bc->driver_bound_array == real_array;
}

void restore_client_vertex_array(const client_attrib_snapshot_t& snapshot) {
    // Restore the frontend VAO name first.  The wrapper maps it back to the
    // driver's renamed object and also reselects the VAO-owned index binding.
    glBindVertexArray(snapshot.vertex_array);
    vertex_array_state_t& active = current_vertex_array_state();
    const vertex_array_state_t before = active;

    uint32_t changed_attrib_mask = 0;
    bool changed = false;
    for (size_t i = 0; i < kTrackedVertexAttribs; ++i) {
        if (!same_vertex_attrib(before.attribs[i], snapshot.vertex_state.attribs[i])) {
            changed = true;
            changed_attrib_mask |= uint32_t{1} << i;
        }
        if (!same_vertex_binding(before.bindings[i], snapshot.vertex_state.bindings[i])) changed = true;
    }

    if (changed) {
        // Legacy glVertexAttrib*Pointer owns its buffer/pointer tuple.  Restore
        // those first; the calls may rewrite the corresponding GLES binding
        // slots, which the explicit binding-model pass below intentionally wins
        // for attributes that use that newer model.
        for (GLuint i = 0; i < kTrackedVertexAttribs; ++i) {
            const auto& saved = snapshot.vertex_state.attribs[i];
            const auto& old = before.attribs[i];
            if (!saved.configured || saved.uses_binding_model) continue;

            const bool pointer_changed = !old.configured || old.uses_binding_model || old.size != saved.size ||
                                         old.type != saved.type || old.normalized != saved.normalized ||
                                         old.stride != saved.stride || old.pointer != saved.pointer ||
                                         old.buffer != saved.buffer || old.buffer_lifetime != saved.buffer_lifetime ||
                                         old.integer != saved.integer;
            if (pointer_changed) {
                GLES.glBindBuffer(GL_ARRAY_BUFFER, driver_buffer_name(saved.buffer));
                const void* pointer = reinterpret_cast<const void*>(saved.pointer);
                if (saved.integer) {
                    GLES.glVertexAttribIPointer(i, saved.size, saved.type, saved.stride, pointer);
                } else {
                    GLES.glVertexAttribPointer(i, saved.size, saved.type, saved.normalized, saved.stride, pointer);
                }
            }
            if (old.divisor != saved.divisor) GLES.glVertexAttribDivisor(i, saved.divisor);
        }

        // Restore only binding points that feed an attribute captured through
        // the GL 4.3 binding model.  PZ normally uses the legacy pointer route,
        // but DSA helpers in the same VAO must not be silently discarded.
        if (GLES.glBindVertexBuffer && GLES.glVertexBindingDivisor) {
            for (GLuint i = 0; i < kTrackedVertexAttribs; ++i) {
                const auto& saved = snapshot.vertex_state.bindings[i];
                const auto& old = before.bindings[i];
                if (!saved.configured || !binding_used_by_model(snapshot.vertex_state, i)) continue;
                if (old.buffer != saved.buffer || old.offset != saved.offset || old.stride != saved.stride ||
                    !old.configured) {
                    GLES.glBindVertexBuffer(i, driver_buffer_name(saved.buffer), saved.offset, saved.stride);
                }
                if (old.divisor != saved.divisor || !old.configured) {
                    GLES.glVertexBindingDivisor(i, saved.divisor);
                }
            }
        }

        for (GLuint i = 0; i < kTrackedVertexAttribs; ++i) {
            const auto& saved = snapshot.vertex_state.attribs[i];
            const auto& old = before.attribs[i];
            if (saved.configured && saved.uses_binding_model && GLES.glVertexAttribBinding) {
                const bool format_changed = !old.configured || !old.uses_binding_model || old.size != saved.size ||
                                            old.type != saved.type || old.normalized != saved.normalized ||
                                            old.integer != saved.integer ||
                                            old.relative_offset != saved.relative_offset;
                if (format_changed) {
                    if (saved.integer && GLES.glVertexAttribIFormat) {
                        GLES.glVertexAttribIFormat(i, saved.size, saved.type, saved.relative_offset);
                    } else if (!saved.integer && GLES.glVertexAttribFormat) {
                        GLES.glVertexAttribFormat(i, saved.size, saved.type, saved.normalized, saved.relative_offset);
                    }
                }
                if (!old.uses_binding_model || old.binding != saved.binding) {
                    GLES.glVertexAttribBinding(i, saved.binding);
                }
            }

            if (old.enabled != saved.enabled) {
                if (saved.enabled) {
                    GLES.glEnableVertexAttribArray(i);
                } else {
                    GLES.glDisableVertexAttribArray(i);
                }
            }
        }
    }

    // These are frontend names.  Going through the wrappers keeps MobileGlues'
    // virtual-name bookkeeping aligned with the GLES state changed above.
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, snapshot.element_array_buffer);
    glBindBuffer(GL_ARRAY_BUFFER, snapshot.array_buffer);
    active = snapshot.vertex_state;
    for (auto& attrib : active.attribs) {
        if (attrib.configured && attrib.buffer != 0 &&
            frontend_buffer_identity_alive(attrib.buffer, attrib.buffer_lifetime))
            attrib.driver_buffer = driver_buffer_name(attrib.buffer);
    }
    for (auto& binding : active.bindings) {
        if (binding.configured && binding.buffer != 0 &&
            frontend_buffer_identity_alive(binding.buffer, binding.buffer_lifetime))
            binding.driver_buffer = driver_buffer_name(binding.buffer);
    }
    active.driver_element_buffer = driver_buffer_name(snapshot.element_array_buffer);

    ++g_bc->client_attrib_pop_hits;
    if (changed) {
        ++g_bc->client_attrib_restore_hits;
#if defined(ZOMDROID_GL_BREADCRUMBS)
        if (client_attrib_trace_milestone(g_bc->client_attrib_restore_hits)) {
            write_log("ZOMDROID_P15_CLIENT_ATTRIB_RESTORE vao=%u changed_attr_mask=0x%x semantic_applied=1 hit=%llu",
                      snapshot.vertex_array, changed_attrib_mask, g_bc->client_attrib_restore_hits);
        }
#endif
    }
}

} // namespace

extern "C" GLAPI GLAPIENTRY void glPushClientAttrib(GLbitfield mask) {
    LOG()
    if (g_bc->client_attrib_stack.size() >= kClientAttribStackLimit) {
        mg_set_gl_error(GL_STACK_OVERFLOW);
        return;
    }

    client_attrib_snapshot_t snapshot;
    snapshot.mask = mask;
    if ((mask & GL_CLIENT_VERTEX_ARRAY_BIT) != 0) {
        snapshot.vertex_array = find_bound_array();
        snapshot.array_buffer = find_bound_buffer_by_target(GL_ARRAY_BUFFER);
        snapshot.element_array_buffer = find_bound_buffer_by_target(GL_ELEMENT_ARRAY_BUFFER);
        snapshot.vertex_state = current_vertex_array_state();
        ++g_bc->client_attrib_push_hits;
#if defined(ZOMDROID_GL_BREADCRUMBS)
        if (client_attrib_trace_milestone(g_bc->client_attrib_push_hits)) {
            write_log("ZOMDROID_P15_CLIENT_ATTRIB_CENSUS mask=0x%x vao=%u semantic_applied=0 hit=%llu", mask,
                      snapshot.vertex_array, g_bc->client_attrib_push_hits);
        }
#endif
    }
    g_bc->client_attrib_stack.push_back(std::move(snapshot));
}

extern "C" GLAPI GLAPIENTRY void glPopClientAttrib(void) {
    LOG()
    if (g_bc->client_attrib_stack.empty()) {
        mg_set_gl_error(GL_STACK_UNDERFLOW);
        return;
    }
    client_attrib_snapshot_t snapshot = std::move(g_bc->client_attrib_stack.back());
    g_bc->client_attrib_stack.pop_back();
    if ((snapshot.mask & GL_CLIENT_VERTEX_ARRAY_BIT) != 0) restore_client_vertex_array(snapshot);
}

NATIVE_FUNCTION_HEAD(void, glEnableVertexAttribArray, GLuint index)
    bool skip = false;
    if (mg_pz_census_active || mg_pz_attrib_fastpath_active) {
        const bool tracked = index < kTrackedVertexAttribs;
        const bool exact = tracked && current_vertex_array_state().attribs[index].enabled == GL_TRUE;
        skip = mg_pz_attrib_fastpath_active && exact && driver_vao_matches_frontend();
        MG_PZ_CENSUS(mg_pz_census_attrib(mg_pz_attrib_kind::enable, tracked, exact, skip));
    }
    if (index < kTrackedVertexAttribs) current_vertex_array_state().attribs[index].enabled = GL_TRUE;
    if (skip) return;
    GLES.glEnableVertexAttribArray(index);
}

NATIVE_FUNCTION_HEAD(void, glDisableVertexAttribArray, GLuint index)
    bool skip = false;
    if (mg_pz_census_active || mg_pz_attrib_fastpath_active) {
        const bool tracked = index < kTrackedVertexAttribs;
        const bool exact = tracked && current_vertex_array_state().attribs[index].enabled == GL_FALSE;
        skip = mg_pz_attrib_fastpath_active && exact && driver_vao_matches_frontend();
        MG_PZ_CENSUS(mg_pz_census_attrib(mg_pz_attrib_kind::enable, tracked, exact, skip));
    }
    if (index < kTrackedVertexAttribs) current_vertex_array_state().attribs[index].enabled = GL_FALSE;
    if (skip) return;
    GLES.glDisableVertexAttribArray(index);
}

NATIVE_FUNCTION_HEAD(void, glVertexAttribPointer, GLuint index, GLint size, GLenum type, GLboolean normalized,
                     GLsizei stride, const void* pointer)
    const GLuint frontend_buffer = find_bound_buffer_by_target(GL_ARRAY_BUFFER);
    const uint64_t frontend_lifetime = frontend_buffer_lifetime(frontend_buffer);
    if (mg_pz_census_active) {
        const bool tracked = index < kTrackedVertexAttribs;
        const auto* previous = tracked ? &current_vertex_array_state().attribs[index] : nullptr;
        const bool exact = previous && previous->configured && !previous->uses_binding_model && !previous->integer &&
                           previous->size == size && previous->type == type && previous->normalized == normalized &&
                           previous->stride == stride && previous->pointer == reinterpret_cast<uintptr_t>(pointer) &&
                           previous->buffer == frontend_buffer && previous->buffer_lifetime == frontend_lifetime;
        mg_pz_census_attrib(mg_pz_attrib_kind::pointer, tracked, exact);
    }
    if (index < kTrackedVertexAttribs) {
        auto& attrib = current_vertex_array_state().attribs[index];
        attrib.size = size;
        attrib.type = type;
        attrib.normalized = normalized;
        attrib.stride = stride;
        attrib.pointer = reinterpret_cast<uintptr_t>(pointer);
        attrib.buffer = frontend_buffer;
        attrib.buffer_lifetime = frontend_lifetime;
        attrib.binding = index;
        attrib.relative_offset = 0;
        attrib.integer = false;
        attrib.configured = true;
        attrib.uses_binding_model = false;
    }
    GLES.glVertexAttribPointer(index, size, type, normalized, stride, pointer);
    if (index < kTrackedVertexAttribs)
        current_vertex_array_state().attribs[index].driver_buffer = driver_buffer_name(frontend_buffer);
}

NATIVE_FUNCTION_HEAD(void, glVertexAttribIPointer, GLuint index, GLint size, GLenum type, GLsizei stride,
                     const void* pointer)
    const GLuint frontend_buffer = find_bound_buffer_by_target(GL_ARRAY_BUFFER);
    const uint64_t frontend_lifetime = frontend_buffer_lifetime(frontend_buffer);
    if (mg_pz_census_active) {
        const bool tracked = index < kTrackedVertexAttribs;
        const auto* previous = tracked ? &current_vertex_array_state().attribs[index] : nullptr;
        const bool exact = previous && previous->configured && !previous->uses_binding_model && previous->integer &&
                           previous->size == size && previous->type == type && previous->stride == stride &&
                           previous->pointer == reinterpret_cast<uintptr_t>(pointer) &&
                           previous->buffer == frontend_buffer && previous->buffer_lifetime == frontend_lifetime;
        mg_pz_census_attrib(mg_pz_attrib_kind::pointer, tracked, exact);
    }
    if (index < kTrackedVertexAttribs) {
        auto& attrib = current_vertex_array_state().attribs[index];
        attrib.size = size;
        attrib.type = type;
        attrib.normalized = GL_FALSE;
        attrib.stride = stride;
        attrib.pointer = reinterpret_cast<uintptr_t>(pointer);
        attrib.buffer = frontend_buffer;
        attrib.buffer_lifetime = frontend_lifetime;
        attrib.binding = index;
        attrib.relative_offset = 0;
        attrib.integer = true;
        attrib.configured = true;
        attrib.uses_binding_model = false;
    }
    GLES.glVertexAttribIPointer(index, size, type, stride, pointer);
    if (index < kTrackedVertexAttribs)
        current_vertex_array_state().attribs[index].driver_buffer = driver_buffer_name(frontend_buffer);
}

NATIVE_FUNCTION_HEAD(void, glVertexAttribDivisor, GLuint index, GLuint divisor)
    if (mg_pz_census_active) {
        const bool tracked = index < kTrackedVertexAttribs;
        const bool exact = tracked && current_vertex_array_state().attribs[index].divisor == divisor;
        mg_pz_census_attrib(mg_pz_attrib_kind::divisor, tracked, exact);
    }
    if (index < kTrackedVertexAttribs) {
        auto& state = current_vertex_array_state();
        state.attribs[index].divisor = divisor;
        state.bindings[index].divisor = divisor;
    }
    GLES.glVertexAttribDivisor(index, divisor);
}

NATIVE_FUNCTION_HEAD(void, glVertexAttribFormat, GLuint attribindex, GLint size, GLenum type, GLboolean normalized,
                     GLuint relativeoffset)
    if (mg_pz_census_active) {
        const bool tracked = attribindex < kTrackedVertexAttribs;
        const auto* previous = tracked ? &current_vertex_array_state().attribs[attribindex] : nullptr;
        const bool exact = previous && previous->configured && previous->uses_binding_model && !previous->integer &&
                           previous->size == size && previous->type == type && previous->normalized == normalized &&
                           previous->relative_offset == relativeoffset;
        mg_pz_census_attrib(mg_pz_attrib_kind::format, tracked, exact);
    }
    if (attribindex < kTrackedVertexAttribs) {
        auto& attrib = current_vertex_array_state().attribs[attribindex];
        attrib.size = size;
        attrib.type = type;
        attrib.normalized = normalized;
        attrib.relative_offset = relativeoffset;
        attrib.integer = false;
        attrib.configured = true;
        attrib.uses_binding_model = true;
    }
    GLES.glVertexAttribFormat(attribindex, size, type, normalized, relativeoffset);
}

NATIVE_FUNCTION_HEAD(void, glVertexAttribIFormat, GLuint attribindex, GLint size, GLenum type, GLuint relativeoffset)
    if (mg_pz_census_active) {
        const bool tracked = attribindex < kTrackedVertexAttribs;
        const auto* previous = tracked ? &current_vertex_array_state().attribs[attribindex] : nullptr;
        const bool exact = previous && previous->configured && previous->uses_binding_model && previous->integer &&
                           previous->size == size && previous->type == type &&
                           previous->relative_offset == relativeoffset;
        mg_pz_census_attrib(mg_pz_attrib_kind::format, tracked, exact);
    }
    if (attribindex < kTrackedVertexAttribs) {
        auto& attrib = current_vertex_array_state().attribs[attribindex];
        attrib.size = size;
        attrib.type = type;
        attrib.normalized = GL_FALSE;
        attrib.relative_offset = relativeoffset;
        attrib.integer = true;
        attrib.configured = true;
        attrib.uses_binding_model = true;
    }
    GLES.glVertexAttribIFormat(attribindex, size, type, relativeoffset);
}

NATIVE_FUNCTION_HEAD(void, glVertexAttribBinding, GLuint attribindex, GLuint bindingindex)
    if (mg_pz_census_active) {
        const bool tracked = attribindex < kTrackedVertexAttribs && bindingindex < kTrackedVertexAttribs;
        const auto* previous = tracked ? &current_vertex_array_state().attribs[attribindex] : nullptr;
        const bool exact = previous && previous->configured && previous->uses_binding_model &&
                           previous->binding == bindingindex;
        mg_pz_census_attrib(mg_pz_attrib_kind::binding, tracked, exact);
    }
    if (attribindex < kTrackedVertexAttribs && bindingindex < kTrackedVertexAttribs) {
        auto& attrib = current_vertex_array_state().attribs[attribindex];
        attrib.binding = bindingindex;
        attrib.uses_binding_model = true;
    }
    GLES.glVertexAttribBinding(attribindex, bindingindex);
}

NATIVE_FUNCTION_HEAD(void, glVertexBindingDivisor, GLuint bindingindex, GLuint divisor)
    if (mg_pz_census_active) {
        const bool tracked = bindingindex < kTrackedVertexAttribs;
        const bool exact = tracked && current_vertex_array_state().bindings[bindingindex].configured &&
                           current_vertex_array_state().bindings[bindingindex].divisor == divisor;
        mg_pz_census_attrib(mg_pz_attrib_kind::divisor, tracked, exact);
    }
    if (bindingindex < kTrackedVertexAttribs) {
        auto& binding = current_vertex_array_state().bindings[bindingindex];
        binding.divisor = divisor;
        binding.configured = true;
    }
    GLES.glVertexBindingDivisor(bindingindex, divisor);
}

GLint mg_client_attrib_stack_depth() {
    return static_cast<GLint>(g_bc->client_attrib_stack.size());
}
#endif
