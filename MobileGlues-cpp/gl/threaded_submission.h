// MobileGlues - gl/threaded_submission.h
// Experimental backend command submission for the isolated ZomDroid build.

#ifndef MOBILEGLUES_THREADED_SUBMISSION_H
#define MOBILEGLUES_THREADED_SUBMISSION_H

#if defined(ZOMDROID_EXPERIMENTAL)

#include <EGL/egl.h>
#include <GL/gl.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace mg_ts {

constexpr size_t kCommandPayloadBytes = 256;
constexpr size_t kInlineCopyBytes = 192;

using command_fn = void (*)(void*);

struct reservation {
    void* storage;
    uint64_t sequence;
};

// Weak because the small host tests link individual GL translation units. The
// Android library supplies these from threaded_submission.cpp.
bool active() __attribute__((weak));
reservation reserve(command_fn execute, command_fn destroy, size_t payload_size,
                    size_t payload_alignment) __attribute__((weak));
void publish(uint64_t sequence) __attribute__((weak));
void wait(uint64_t sequence) __attribute__((weak));
void flush_pending() __attribute__((weak));
bool draw_async_safe(bool indexed, bool indirect) __attribute__((weak));

using egl_bind_api_fn = EGLBoolean (*)(EGLenum);
using egl_make_current_fn = EGLBoolean (*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
using egl_release_thread_fn = EGLBoolean (*)();
using egl_swap_buffers_fn = EGLBoolean (*)(EGLDisplay, EGLSurface);
using egl_swap_damage_fn = EGLBoolean (*)(EGLDisplay, EGLSurface, EGLint*, EGLint);

bool adopt_context(EGLDisplay display, EGLSurface draw, EGLSurface read, EGLContext context,
                   egl_bind_api_fn bind_api, egl_make_current_fn make_current,
                   egl_release_thread_fn release_thread) __attribute__((weak));
bool release_context() __attribute__((weak));
void shutdown() __attribute__((weak));
bool owns_context_for_caller() __attribute__((weak));
bool submit_swap(EGLDisplay display, EGLSurface surface, egl_swap_buffers_fn full_swap,
                 egl_swap_damage_fn damage_swap, const EGLint* rects, EGLint rect_count,
                 bool synchronous, EGLBoolean* result) __attribute__((weak));

inline bool availableAndActive() {
    return active != nullptr && reserve != nullptr && publish != nullptr && wait != nullptr && active();
}

template <typename Command, typename... Args> uint64_t enqueue(Args&&... args) {
    static_assert(sizeof(Command) <= kCommandPayloadBytes, "threaded command is too large");
    static_assert(alignof(Command) <= alignof(std::max_align_t), "threaded command alignment is too large");
    const reservation slot = reserve(&Command::execute, &Command::destroy, sizeof(Command), alignof(Command));
    if (slot.storage == nullptr) return 0;
    new (slot.storage) Command(std::forward<Args>(args)...);
    publish(slot.sequence);
    return slot.sequence;
}

template <typename Fn, typename R, typename... Args> struct call_command {
    Fn function;
    std::tuple<std::decay_t<Args>...> arguments;
    R* result;

    call_command(Fn fn, R* out, Args... args) : function(fn), arguments(args...), result(out) {}

    static void execute(void* storage) {
        auto* command = static_cast<call_command*>(storage);
        *command->result = std::apply(command->function, command->arguments);
    }
    static void destroy(void* storage) { static_cast<call_command*>(storage)->~call_command(); }
};

template <typename Fn, typename... Args> struct call_command<Fn, void, Args...> {
    Fn function;
    std::tuple<std::decay_t<Args>...> arguments;

    call_command(Fn fn, Args... args) : function(fn), arguments(args...) {}

    static void execute(void* storage) {
        auto* command = static_cast<call_command*>(storage);
        std::apply(command->function, command->arguments);
    }
    static void destroy(void* storage) { static_cast<call_command*>(storage)->~call_command(); }
};

template <typename R, typename... Args>
R dispatch_call(R (*function)(Args...), bool synchronous, Args... args) {
    if (!availableAndActive()) return function(args...);
    if constexpr (std::is_void_v<R>) {
        using command = call_command<decltype(function), void, Args...>;
        const uint64_t sequence = enqueue<command>(function, args...);
        if (sequence == 0) {
            function(args...);
            return;
        }
        if (synchronous) wait(sequence);
    } else {
        R result{};
        using command = call_command<decltype(function), R, Args...>;
        const uint64_t sequence = enqueue<command>(function, &result, args...);
        if (sequence == 0) return function(args...);
        wait(sequence);
        return result;
    }
}

enum class slot_policy : uint8_t {
    automatic,
    synchronous,
    packet_flush,
    pointer_offset,
    uniform_copy,
    buffer_copy,
};

inline bool nameEquals(const char* lhs, const char* rhs) { return std::strcmp(lhs, rhs) == 0; }

inline bool nameStartsWith(const char* value, const char* prefix) {
    return std::strncmp(value, prefix, std::strlen(prefix)) == 0;
}

inline bool isDrawCommand(const char* name) {
    return nameStartsWith(name, "glDrawArrays") || nameStartsWith(name, "glDrawElements") ||
           nameStartsWith(name, "glDrawRangeElements") || nameStartsWith(name, "glMultiDraw");
}

inline slot_policy policyForName(const char* name) {
    if (nameEquals(name, "glFinish")) return slot_policy::synchronous;
    if (nameEquals(name, "glFlush")) return slot_policy::packet_flush;
    if (nameEquals(name, "glBufferData") || nameEquals(name, "glBufferSubData") ||
        nameEquals(name, "glBufferStorageEXT"))
        return slot_policy::buffer_copy;
    if ((nameStartsWith(name, "glUniform") || nameStartsWith(name, "glProgramUniform")) &&
        name[std::strlen(name) - 1] == 'v')
        return slot_policy::uniform_copy;
    if (nameStartsWith(name, "glDrawElements") || nameStartsWith(name, "glDrawRangeElements") ||
        nameEquals(name, "glDrawArraysIndirect") || nameEquals(name, "glDrawElementsIndirect") ||
        nameEquals(name, "glMultiDrawArraysIndirectEXT") || nameEquals(name, "glMultiDrawElementsIndirectEXT") ||
        nameEquals(name, "glVertexAttribPointer") || nameEquals(name, "glVertexAttribIPointer"))
        return slot_policy::pointer_offset;
    return slot_policy::automatic;
}

template <typename T> inline bool pointerArgumentLooksLikeOffset(T value) {
    if constexpr (!std::is_pointer_v<std::decay_t<T>>) {
        return true;
    } else {
        return value == nullptr || reinterpret_cast<uintptr_t>(value) <= std::numeric_limits<uint32_t>::max();
    }
}

template <typename... Args> inline bool pointerArgumentsLookLikeOffsets(Args... args) {
    return (pointerArgumentLooksLikeOffset(args) && ... && true);
}

inline unsigned uniformElementsForName(const char* name) {
    const char* uniform = std::strstr(name, "Uniform");
    if (uniform == nullptr) return 0;
    uniform += 7;
    if (std::strncmp(uniform, "Matrix", 6) == 0) {
        uniform += 6;
        if (uniform[0] < '2' || uniform[0] > '4') return 0;
        const unsigned columns = static_cast<unsigned>(uniform[0] - '0');
        const unsigned rows = uniform[1] == 'x' && uniform[2] >= '2' && uniform[2] <= '4'
                                  ? static_cast<unsigned>(uniform[2] - '0')
                                  : columns;
        return columns * rows;
    }
    return uniform[0] >= '1' && uniform[0] <= '4' ? static_cast<unsigned>(uniform[0] - '0') : 0;
}

template <typename Fn, typename T> struct uniform_vector_command {
    Fn function;
    GLint location;
    GLsizei count;
    alignas(std::max_align_t) unsigned char values[kInlineCopyBytes];

    uniform_vector_command(Fn fn, GLint loc, GLsizei n, const T* source, size_t bytes)
        : function(fn), location(loc), count(n) {
        std::memcpy(values, source, bytes);
    }
    static void execute(void* storage) {
        auto* command = static_cast<uniform_vector_command*>(storage);
        command->function(command->location, command->count, reinterpret_cast<const T*>(command->values));
    }
    static void destroy(void* storage) { static_cast<uniform_vector_command*>(storage)->~uniform_vector_command(); }
};

template <typename Fn> struct uniform_matrix_command {
    Fn function;
    GLint location;
    GLsizei count;
    GLboolean transpose;
    alignas(std::max_align_t) unsigned char values[kInlineCopyBytes];

    uniform_matrix_command(Fn fn, GLint loc, GLsizei n, GLboolean trans, const GLfloat* source, size_t bytes)
        : function(fn), location(loc), count(n), transpose(trans) {
        std::memcpy(values, source, bytes);
    }
    static void execute(void* storage) {
        auto* command = static_cast<uniform_matrix_command*>(storage);
        command->function(command->location, command->count, command->transpose,
                          reinterpret_cast<const GLfloat*>(command->values));
    }
    static void destroy(void* storage) { static_cast<uniform_matrix_command*>(storage)->~uniform_matrix_command(); }
};

template <typename Fn, typename T> struct program_uniform_vector_command {
    Fn function;
    GLuint program;
    GLint location;
    GLsizei count;
    alignas(std::max_align_t) unsigned char values[kInlineCopyBytes];

    program_uniform_vector_command(Fn fn, GLuint prog, GLint loc, GLsizei n, const T* source, size_t bytes)
        : function(fn), program(prog), location(loc), count(n) {
        std::memcpy(values, source, bytes);
    }
    static void execute(void* storage) {
        auto* command = static_cast<program_uniform_vector_command*>(storage);
        command->function(command->program, command->location, command->count,
                          reinterpret_cast<const T*>(command->values));
    }
    static void destroy(void* storage) {
        static_cast<program_uniform_vector_command*>(storage)->~program_uniform_vector_command();
    }
};

template <typename Fn> struct program_uniform_matrix_command {
    Fn function;
    GLuint program;
    GLint location;
    GLsizei count;
    GLboolean transpose;
    alignas(std::max_align_t) unsigned char values[kInlineCopyBytes];

    program_uniform_matrix_command(Fn fn, GLuint prog, GLint loc, GLsizei n, GLboolean trans, const GLfloat* source,
                                   size_t bytes)
        : function(fn), program(prog), location(loc), count(n), transpose(trans) {
        std::memcpy(values, source, bytes);
    }
    static void execute(void* storage) {
        auto* command = static_cast<program_uniform_matrix_command*>(storage);
        command->function(command->program, command->location, command->count, command->transpose,
                          reinterpret_cast<const GLfloat*>(command->values));
    }
    static void destroy(void* storage) {
        static_cast<program_uniform_matrix_command*>(storage)->~program_uniform_matrix_command();
    }
};

inline bool validCopySize(GLsizei count, unsigned elements, size_t element_size, size_t* bytes) {
    if (count <= 0 || elements == 0) return false;
    const size_t n = static_cast<size_t>(count);
    if (n > std::numeric_limits<size_t>::max() / elements || n * elements > kInlineCopyBytes / element_size)
        return false;
    *bytes = n * elements * element_size;
    return true;
}

template <typename T>
bool tryUniformCopy(void (*function)(GLint, GLsizei, const T*), unsigned elements, GLint location, GLsizei count,
                    const T* value) {
    size_t bytes = 0;
    if (!availableAndActive() || value == nullptr || !validCopySize(count, elements, sizeof(T), &bytes)) return false;
    using command = uniform_vector_command<decltype(function), T>;
    return enqueue<command>(function, location, count, value, bytes) != 0;
}

inline bool tryUniformCopy(void (*function)(GLint, GLsizei, GLboolean, const GLfloat*), unsigned elements,
                           GLint location, GLsizei count, GLboolean transpose, const GLfloat* value) {
    size_t bytes = 0;
    if (!availableAndActive() || value == nullptr || !validCopySize(count, elements, sizeof(GLfloat), &bytes))
        return false;
    using command = uniform_matrix_command<decltype(function)>;
    return enqueue<command>(function, location, count, transpose, value, bytes) != 0;
}

template <typename T>
bool tryUniformCopy(void (*function)(GLuint, GLint, GLsizei, const T*), unsigned elements, GLuint program,
                    GLint location, GLsizei count, const T* value) {
    size_t bytes = 0;
    if (!availableAndActive() || value == nullptr || !validCopySize(count, elements, sizeof(T), &bytes)) return false;
    using command = program_uniform_vector_command<decltype(function), T>;
    return enqueue<command>(function, program, location, count, value, bytes) != 0;
}

inline bool tryUniformCopy(void (*function)(GLuint, GLint, GLsizei, GLboolean, const GLfloat*), unsigned elements,
                           GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat* value) {
    size_t bytes = 0;
    if (!availableAndActive() || value == nullptr || !validCopySize(count, elements, sizeof(GLfloat), &bytes))
        return false;
    using command = program_uniform_matrix_command<decltype(function)>;
    return enqueue<command>(function, program, location, count, transpose, value, bytes) != 0;
}

template <typename Fn, typename... Args> bool tryUniformCopy(Fn, unsigned, Args...) { return false; }

template <typename Fn, typename... Args> struct owned_pointer_command {
    Fn function;
    std::tuple<std::decay_t<Args>...> arguments;
    void* owned_data;

    owned_pointer_command(Fn fn, void* data, Args... args) : function(fn), arguments(args...), owned_data(data) {}
    static void execute(void* storage) {
        auto* command = static_cast<owned_pointer_command*>(storage);
        std::apply(command->function, command->arguments);
    }
    static void destroy(void* storage) {
        auto* command = static_cast<owned_pointer_command*>(storage);
        std::free(command->owned_data);
        command->~owned_pointer_command();
    }
};

constexpr size_t kMaximumOwnedUpload = 32U * 1024U * 1024U;

template <typename Last>
bool tryBufferCopy(void (*function)(GLenum, GLsizeiptr, const void*, Last), GLenum target, GLsizeiptr size,
                   const void* data, Last last) {
    if (!availableAndActive() || size < 0 || static_cast<uint64_t>(size) > kMaximumOwnedUpload) return false;
    void* copy = nullptr;
    if (data != nullptr && size > 0) {
        copy = std::malloc(static_cast<size_t>(size));
        if (copy == nullptr) return false;
        std::memcpy(copy, data, static_cast<size_t>(size));
    }
    using command = owned_pointer_command<decltype(function), GLenum, GLsizeiptr, const void*, Last>;
    const uint64_t sequence = enqueue<command>(function, copy, target, size, copy, last);
    if (sequence == 0) std::free(copy);
    return sequence != 0;
}

inline bool tryBufferCopy(void (*function)(GLenum, GLintptr, GLsizeiptr, const void*), GLenum target, GLintptr offset,
                          GLsizeiptr size, const void* data) {
    if (!availableAndActive() || size < 0 || static_cast<uint64_t>(size) > kMaximumOwnedUpload) return false;
    void* copy = nullptr;
    if (data != nullptr && size > 0) {
        copy = std::malloc(static_cast<size_t>(size));
        if (copy == nullptr) return false;
        std::memcpy(copy, data, static_cast<size_t>(size));
    }
    using command = owned_pointer_command<decltype(function), GLenum, GLintptr, GLsizeiptr, const void*>;
    const uint64_t sequence = enqueue<command>(function, copy, target, offset, size, copy);
    if (sequence == 0) std::free(copy);
    return sequence != 0;
}

template <typename Fn, typename... Args> bool tryBufferCopy(Fn, Args...) { return false; }

template <typename Fn> struct shared_buffer_data_command {
    Fn function;
    GLenum target;
    GLsizeiptr size;
    GLenum usage;
    std::shared_ptr<std::vector<unsigned char>> storage;
    size_t offset;

    shared_buffer_data_command(Fn fn, GLenum target_in, GLsizeiptr size_in, GLenum usage_in,
                               std::shared_ptr<std::vector<unsigned char>> storage_in, size_t offset_in)
        : function(fn), target(target_in), size(size_in), usage(usage_in), storage(std::move(storage_in)),
          offset(offset_in) {}

    static void execute(void* payload) {
        auto* command = static_cast<shared_buffer_data_command*>(payload);
        const void* data = command->storage->data() + command->offset;
        command->function(command->target, command->size, data, command->usage);
    }
    static void destroy(void* payload) {
        static_cast<shared_buffer_data_command*>(payload)->~shared_buffer_data_command();
    }
};

template <typename Fn>
bool trySharedBufferData(Fn function, GLenum target, GLsizeiptr size, GLenum usage,
                         const std::shared_ptr<std::vector<unsigned char>>& storage, size_t offset) {
    if (!availableAndActive() || !storage || size <= 0 || offset > storage->size() ||
        static_cast<uint64_t>(size) > storage->size() - offset)
        return false;
    using command = shared_buffer_data_command<Fn>;
    return enqueue<command>(function, target, size, usage, storage, offset) != 0;
}

} // namespace mg_ts

template <typename Function> class mg_ts_dispatch_slot;

template <typename R, typename... Args> class mg_ts_dispatch_slot<R (*)(Args...)> {
  public:
    using function_type = R (*)(Args...);

    constexpr explicit mg_ts_dispatch_slot(const char* name = "")
        : function_(nullptr), name_(name), policy_(mg_ts::slot_policy::automatic), uniform_elements_(0) {}

    mg_ts_dispatch_slot& operator=(function_type function) {
        function_ = function;
        policy_ = mg_ts::policyForName(name_);
        uniform_elements_ = policy_ == mg_ts::slot_policy::uniform_copy ? mg_ts::uniformElementsForName(name_) : 0;
        return *this;
    }

    operator function_type() const { return function_; }
    explicit operator bool() const { return function_ != nullptr; }
    bool operator==(std::nullptr_t) const { return function_ == nullptr; }
    bool operator!=(std::nullptr_t) const { return function_ != nullptr; }

    bool submitSharedBufferData(GLenum target, GLsizeiptr size, GLenum usage,
                                const std::shared_ptr<std::vector<unsigned char>>& storage, size_t offset) const {
        if constexpr (std::is_void_v<R> && sizeof...(Args) == 4 &&
                      std::is_invocable_r_v<void, function_type, GLenum, GLsizeiptr, const void*, GLenum>) {
            return mg_ts::trySharedBufferData(function_, target, size, usage, storage, offset);
        }
        return false;
    }

    R operator()(Args... args) const {
        if (!mg_ts::availableAndActive()) return function_(args...);
        if constexpr (std::is_void_v<R>) {
            if (policy_ == mg_ts::slot_policy::uniform_copy &&
                mg_ts::tryUniformCopy(function_, uniform_elements_, args...))
                return;
            if (policy_ == mg_ts::slot_policy::buffer_copy && mg_ts::tryBufferCopy(function_, args...)) return;

            constexpr bool has_pointer = (std::is_pointer_v<std::decay_t<Args>> || ... || false);
            const bool draw_must_wait = mg_ts::isDrawCommand(name_) &&
                                        (mg_ts::draw_async_safe == nullptr ||
                                         !mg_ts::draw_async_safe(std::strstr(name_, "Elements") != nullptr,
                                                                 std::strstr(name_, "Indirect") != nullptr));
            const bool synchronous = draw_must_wait || policy_ == mg_ts::slot_policy::synchronous ||
                                     (has_pointer &&
                                      (policy_ != mg_ts::slot_policy::pointer_offset ||
                                       !mg_ts::pointerArgumentsLookLikeOffsets(args...)));
            mg_ts::dispatch_call(function_, synchronous, args...);
            if (policy_ == mg_ts::slot_policy::packet_flush && mg_ts::flush_pending != nullptr)
                mg_ts::flush_pending();
        } else {
            return mg_ts::dispatch_call(function_, true, args...);
        }
    }

  private:
    function_type function_;
    const char* name_;
    mg_ts::slot_policy policy_;
    unsigned uniform_elements_;
};

#endif // ZOMDROID_EXPERIMENTAL

#endif // MOBILEGLUES_THREADED_SUBMISSION_H
