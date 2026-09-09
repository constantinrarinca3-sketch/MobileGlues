// MobileGlues - gl/buffer.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#include "buffer.h"
#include "../egl/context.h"
#include <atomic>
#include <mutex>
#include <memory>
#include <cstdint>
#include <ska/flat_hash_map.hpp>
#include <array>
#include "texture.h"

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
};

struct client_attrib_snapshot_t {
    GLbitfield mask = 0;
    GLuint vertex_array = 0;
    GLuint array_buffer = 0;
    GLuint element_array_buffer = 0;
    vertex_array_state_t vertex_state{};
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
};

struct buffer_ctx_state_t { // private to one context
    std::vector<GLuint> gen_arrays;
    std::vector<char> gen_array_exists;
    std::vector<GLuint> free_array_ids;
    std::vector<GLuint> element_array_buffer_per_vao;
    std::array<GLuint, 13> bound_buffers{};
    GLuint bound_array = 0;
#if defined(ZOMDROID_EXPERIMENTAL)
    ska::flat_hash_map<GLuint, vertex_array_state_t> vertex_array_states;
    std::vector<client_attrib_snapshot_t> client_attrib_stack;
    unsigned long long client_attrib_push_hits = 0;
    unsigned long long client_attrib_pop_hits = 0;
    unsigned long long client_attrib_restore_hits = 0;
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
    if (!ctx) ctx = std::make_unique<buffer_ctx_state_t>();
    g_bg = group.get();
    g_bc = ctx.get();
}

void mg_buffer_forget_context(unsigned long long ctx_id) {
    if (ctx_id == 0) return;
    std::lock_guard<std::mutex> lock(g_buf_mutex);
    const auto it = g_buf_ctxs.find(ctx_id);
    if (it == g_buf_ctxs.end()) return;
    if (g_bc == it->second.get()) g_bc = &g_buf_ctx_default;
    g_buf_ctxs.erase(it);
}

#define g_gen_buffers (g_bg->gen_buffers)
#define g_gen_buffer_exists (g_bg->gen_buffer_exists)
#define g_free_buffer_ids (g_bg->free_buffer_ids)
#define g_buffer_datasize (g_bg->buffer_datasize)
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

#if defined(ZOMDROID_EXPERIMENTAL)
static vertex_array_state_t& current_vertex_array_state() {
    return g_bc->vertex_array_states[g_bound_array];
}

static bool same_vertex_binding(const vertex_binding_state_t& a, const vertex_binding_state_t& b) {
    return a.buffer == b.buffer && a.offset == b.offset && a.stride == b.stride && a.divisor == b.divisor &&
           a.configured == b.configured;
}

static bool same_vertex_attrib(const vertex_attrib_state_t& a, const vertex_attrib_state_t& b) {
    return a.enabled == b.enabled && a.size == b.size && a.type == b.type && a.normalized == b.normalized &&
           a.stride == b.stride && a.pointer == b.pointer && a.buffer == b.buffer && a.divisor == b.divisor &&
           a.binding == b.binding && a.relative_offset == b.relative_offset && a.integer == b.integer &&
           a.configured == b.configured && a.uses_binding_model == b.uses_binding_model;
}

static GLuint driver_buffer_name(GLuint frontend_name) {
    if (frontend_name == 0) return 0;
    if (!has_buffer(frontend_name)) return frontend_name;
    const GLuint real = find_real_buffer(frontend_name);
    return real != 0 ? real : frontend_name;
}

static bool client_attrib_trace_milestone(unsigned long long hits) {
    return hits == 1 || hits == 1024 || hits == 65536;
}
#endif

static inline int ensure_buffer_capacity(GLuint id) {
    if ((int)g_gen_buffers.size() <= (int)id) {
        g_gen_buffers.resize(id + 1, 0);
        g_gen_buffer_exists.resize(id + 1, 0);
        if (g_buffer_datasize.size() <= (size_t)id) g_buffer_datasize.resize(id + 1, 0);
    }
    return 0;
}

static inline int ensure_array_capacity(GLuint id) {
    if ((int)g_gen_arrays.size() <= (int)id) {
        g_gen_arrays.resize(id + 1, 0);
        g_gen_array_exists.resize(id + 1, 0);
        if (g_element_array_buffer_per_vao.size() <= (size_t)id) g_element_array_buffer_per_vao.resize(id + 1, 0);
    }
    return 0;
}

GLuint gen_buffer() {
    if (!g_free_buffer_ids.empty()) {
        GLuint id = g_free_buffer_ids.back();
        g_free_buffer_ids.pop_back();
        ensure_buffer_capacity(id);
        g_gen_buffers[id] = 0;
        g_gen_buffer_exists[id] = 1;
        g_buffer_datasize[id] = 0;
        if (id > (GLuint)maxBufferId) maxBufferId = id;
        return id;
    }
    maxBufferId++;
    ensure_buffer_capacity((GLuint)maxBufferId);
    g_gen_buffers[maxBufferId] = 0;
    g_gen_buffer_exists[maxBufferId] = 1;
    g_buffer_datasize[maxBufferId] = 0;
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
        g_free_buffer_ids.push_back(key);
    }
}

GLuint find_real_buffer(GLuint key) {
    if (key < g_gen_buffers.size() && g_gen_buffer_exists[key]) return g_gen_buffers[key];
    return 0;
}

GLuint get_ibo_by_vao(GLuint vao) {
    if (vao < g_element_array_buffer_per_vao.size()) return g_element_array_buffer_per_vao[vao];
    return 0;
}

GLuint find_bound_array() {
    return g_bound_array;
}

void update_vao_ibo_binding(GLuint vao, GLuint ibo) {
    ensure_array_capacity(vao);
    g_element_array_buffer_per_vao[vao] = ibo;
}

void set_buffer_data_size(GLuint buffer, size_t size) {
    ensure_buffer_capacity(buffer);
    g_buffer_datasize[buffer] = size;
}

size_t get_buffer_data_size(GLuint buffer) {
    if (buffer < g_buffer_datasize.size()) return g_buffer_datasize[buffer];
    return 0;
}

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
        g_element_array_buffer_per_vao[id] = 0;
        if (id > (GLuint)maxArrayId) maxArrayId = id;
        return id;
    }
    maxArrayId++;
    ensure_array_capacity((GLuint)maxArrayId);
    g_gen_arrays[maxArrayId] = 0;
    g_gen_array_exists[maxArrayId] = 1;
    g_element_array_buffer_per_vao[maxArrayId] = 0;
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
        if (key < g_element_array_buffer_per_vao.size()) g_element_array_buffer_per_vao[key] = 0;
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
    g_gen_buffers.resize(1, 0);
    g_gen_buffer_exists.resize(1, 0);
    g_buffer_datasize.resize(1, 0);
}

void InitVertexArrayMap(size_t expectedSize) {
    g_gen_arrays.reserve(expectedSize + 2);
    g_gen_array_exists.reserve(expectedSize + 2);
    g_element_array_buffer_per_vao.reserve(expectedSize + 2);
    g_gen_arrays.resize(1, 0);
    g_gen_array_exists.resize(1, 0);
    g_element_array_buffer_per_vao.resize(1, 0);
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
        if (find_real_buffer(buffers[i])) {
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
    set_bound_buffer_by_target(target, buffer);

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
    CHECK_GL_ERROR
}

static std::vector<GLuint> g_buffer_map_ssbo_id;

void glBindBufferRange(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size) {
    LOG()
    LOG_D("glBindBufferRange, target = %s, index = %d, buffer = %d, offset = %p, size = %zi", glEnumToString(target),
          index, buffer, (void*)offset, size)

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
    if (bindingindex < kTrackedVertexAttribs) {
        auto& binding = current_vertex_array_state().bindings[bindingindex];
        binding.buffer = buffer;
        binding.offset = offset;
        binding.stride = stride;
        binding.configured = true;
    }
#endif
    if (!has_buffer(buffer) || buffer == 0) {
        GLES.glBindVertexBuffer(bindingindex, buffer, offset, stride);
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

void glBufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage) {
    LOG()
    LOG_D("glBufferData, target = %s, size = %d, data = 0x%x, usage = %s", glEnumToString(target), size, data,
          glEnumToString(usage))
    borrowed_target_t t(target);
    GLES.glBufferData(t.target, size, data, usage);
    set_buffer_data_size(find_bound_buffer_by_target(target), size);
    CHECK_GL_ERROR
}

// Both of these were plain pass-throughs in gl/gl_native.cpp. They live here now
// so that GL_PARAMETER_BUFFER reaches the driver as a target it understands.
void glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data) {
    LOG()
    LOG_D("glBufferSubData, target = %s, offset = %p, size = %zi", glEnumToString(target), (void*)offset, size)
    borrowed_target_t t(target);
    GLES.glBufferSubData(t.target, offset, size, data);
    CHECK_GL_ERROR
}

void glGetBufferParameteriv(GLenum target, GLenum pname, GLint* params) {
    LOG()
    LOG_D("glGetBufferParameteriv, target = %s, pname = %s", glEnumToString(target), glEnumToString(pname))
    borrowed_target_t t(target);
    GLES.glGetBufferParameteriv(t.target, pname, params);
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
        trace_zomdroid_buffer_call("MAP_OES_EXIT", target, 0, 0, access, ptr);
        return ptr;
    }
    GLint buffer_size;
    glGetBufferParameteriv(target, GL_BUFFER_SIZE, &buffer_size);
    if (buffer_size <= 0 || glGetError() != GL_NO_ERROR) {
        return nullptr;
    }
    GLbitfield flags = 0;
    switch (access) {
    case GL_READ_ONLY:
        flags = GL_MAP_READ_BIT;
        break;
    case GL_WRITE_ONLY:
        flags = GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT;
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
    if (!GLES.glMapBufferRange) {
        trace_zomdroid_buffer_call("MAP_RANGE_MISSING", target, offset, length, access, nullptr);
        mg_set_gl_error(GL_INVALID_OPERATION);
        return nullptr;
    }
    borrowed_target_t t(target);
    void* ptr = GLES.glMapBufferRange(t.target, offset, length, access);
    trace_zomdroid_buffer_call("MAP_RANGE_EXIT", target, offset, length, access, ptr);
    return ptr;
}

GLboolean glUnmapBuffer(GLenum target) {
    LOG()
    LOG_D("%s(%s)", __func__, glEnumToString(target));
    trace_zomdroid_buffer_call("UNMAP_ENTER", target, 0, 0, 0, nullptr);
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
        borrowed_target_t t(target);
        GLES.glBufferStorageEXT(t.target, size, data, flags);
        // Allocates storage just as glBufferData does, so it owes the same record.
        set_buffer_data_size(find_bound_buffer_by_target(target), size);
    }
    CHECK_GL_ERROR
}

void glFlushMappedBufferRange(GLenum target, GLintptr offset, GLsizeiptr length) {
    LOG()
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
        if (find_real_array(arrays[i])) {
            GLuint real_array = find_real_array(arrays[i]);
            GLES.glDeleteVertexArrays(1, &real_array);
            CHECK_GL_ERROR
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
    g_bound_array = array;

    // update bound ibo
    set_bound_buffer_by_target(GL_ELEMENT_ARRAY_BUFFER, get_ibo_by_vao(array));

    if (!has_array(array) || array == 0) {
        LOG_D("Does not have va=%d found!", array)
        GLES.glBindVertexArray(array);
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
    GLES.glBindVertexArray(real_array);
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
                                         old.buffer != saved.buffer || old.integer != saved.integer;
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
    if (index < kTrackedVertexAttribs) current_vertex_array_state().attribs[index].enabled = GL_TRUE;
    GLES.glEnableVertexAttribArray(index);
}

NATIVE_FUNCTION_HEAD(void, glDisableVertexAttribArray, GLuint index)
    if (index < kTrackedVertexAttribs) current_vertex_array_state().attribs[index].enabled = GL_FALSE;
    GLES.glDisableVertexAttribArray(index);
}

NATIVE_FUNCTION_HEAD(void, glVertexAttribPointer, GLuint index, GLint size, GLenum type, GLboolean normalized,
                     GLsizei stride, const void* pointer)
    if (index < kTrackedVertexAttribs) {
        auto& attrib = current_vertex_array_state().attribs[index];
        attrib.size = size;
        attrib.type = type;
        attrib.normalized = normalized;
        attrib.stride = stride;
        attrib.pointer = reinterpret_cast<uintptr_t>(pointer);
        attrib.buffer = find_bound_buffer_by_target(GL_ARRAY_BUFFER);
        attrib.binding = index;
        attrib.relative_offset = 0;
        attrib.integer = false;
        attrib.configured = true;
        attrib.uses_binding_model = false;
    }
    GLES.glVertexAttribPointer(index, size, type, normalized, stride, pointer);
}

NATIVE_FUNCTION_HEAD(void, glVertexAttribIPointer, GLuint index, GLint size, GLenum type, GLsizei stride,
                     const void* pointer)
    if (index < kTrackedVertexAttribs) {
        auto& attrib = current_vertex_array_state().attribs[index];
        attrib.size = size;
        attrib.type = type;
        attrib.normalized = GL_FALSE;
        attrib.stride = stride;
        attrib.pointer = reinterpret_cast<uintptr_t>(pointer);
        attrib.buffer = find_bound_buffer_by_target(GL_ARRAY_BUFFER);
        attrib.binding = index;
        attrib.relative_offset = 0;
        attrib.integer = true;
        attrib.configured = true;
        attrib.uses_binding_model = false;
    }
    GLES.glVertexAttribIPointer(index, size, type, stride, pointer);
}

NATIVE_FUNCTION_HEAD(void, glVertexAttribDivisor, GLuint index, GLuint divisor)
    if (index < kTrackedVertexAttribs) {
        auto& state = current_vertex_array_state();
        state.attribs[index].divisor = divisor;
        state.bindings[index].divisor = divisor;
    }
    GLES.glVertexAttribDivisor(index, divisor);
}

NATIVE_FUNCTION_HEAD(void, glVertexAttribFormat, GLuint attribindex, GLint size, GLenum type, GLboolean normalized,
                     GLuint relativeoffset)
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
    if (attribindex < kTrackedVertexAttribs && bindingindex < kTrackedVertexAttribs) {
        auto& attrib = current_vertex_array_state().attribs[attribindex];
        attrib.binding = bindingindex;
        attrib.uses_binding_model = true;
    }
    GLES.glVertexAttribBinding(attribindex, bindingindex);
}

NATIVE_FUNCTION_HEAD(void, glVertexBindingDivisor, GLuint bindingindex, GLuint divisor)
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
