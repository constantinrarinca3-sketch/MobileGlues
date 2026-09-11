// MobileGlues - gl/threaded_submission.h
// V4.5 wrapper around the pre-existing threaded submission layer.
// V4.4 barrier-name attribution is retained, while valid glDrawRangeElements
// calls bypass the stale name-only barrier classification and enter the exact
// draw_elements class used by the Material-Stream router. Invalid ranges remain
// barriers so fallback/error ordering is preserved.

#ifndef MOBILEGLUES_THREADED_SUBMISSION_V44_WRAPPER_H
#define MOBILEGLUES_THREADED_SUBMISSION_V44_WRAPPER_H

#define mg_ts_dispatch_slot mg_ts_dispatch_slot_v43
#include "threaded_submission_v43_base.h"
#undef mg_ts_dispatch_slot

#if defined(ZOMDROID_EXPERIMENTAL)

namespace mg_ts {

void pz_repack_note_backend_command(const char* name) __attribute__((weak));

struct v44_command_name_marker {
    const char* name;
    explicit v44_command_name_marker(const char* value) : name(value) {}
    static void execute(void* storage) {
        auto* marker = static_cast<v44_command_name_marker*>(storage);
        if (pz_repack_note_backend_command != nullptr) pz_repack_note_backend_command(marker->name);
    }
    static void destroy(void* storage) { static_cast<v44_command_name_marker*>(storage)->~v44_command_name_marker(); }
};

inline void v44_enqueue_barrier_name(const char* name) {
    if (name == nullptr || pz_repack_note_backend_command == nullptr || !availableAndActive()) return;
    (void)enqueueClassified<v44_command_name_marker>(backend_command_class::geometry_state, name);
}

template <typename... Args>
inline bool v45_dispatch_draw_range(const char*, void (*)(Args...), Args...) {
    return false;
}

inline bool v45_dispatch_draw_range(const char* name,
                                    void (*function)(GLenum, GLuint, GLuint, GLsizei, GLenum, const void*),
                                    GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                                    const void* indices) {
    if (!availableAndActive() || !nameEquals(name, "glDrawRangeElements")) return false;

    const backend_command_class classification =
        end >= start ? backend_command_class::draw_elements : backend_command_class::barrier;
    if (classification == backend_command_class::barrier) v44_enqueue_barrier_name(name);

    const bool draw_must_wait = draw_async_safe == nullptr || !draw_async_safe(true, false);
    const bool pointer_must_wait = !pointerArgumentsLookLikeOffsets(mode, start, end, count, type, indices);
    dispatch_call_classified(function, draw_must_wait || pointer_must_wait, classification,
                             mode, start, end, count, type, indices);
    return true;
}

} // namespace mg_ts

template <typename Function> class mg_ts_dispatch_slot;

template <typename R, typename... Args> class mg_ts_dispatch_slot<R (*)(Args...)> {
  public:
    using function_type = R (*)(Args...);
    using base_type = mg_ts_dispatch_slot_v43<function_type>;

    constexpr explicit mg_ts_dispatch_slot(const char* name = "") : base_(name), name_(name) {}

    mg_ts_dispatch_slot& operator=(function_type function) {
        base_ = function;
        return *this;
    }

    operator function_type() const { return static_cast<function_type>(base_); }
    explicit operator bool() const { return static_cast<bool>(base_); }
    bool operator==(std::nullptr_t) const { return base_ == nullptr; }
    bool operator!=(std::nullptr_t) const { return base_ != nullptr; }

    R operator()(Args... args) const {
        if constexpr (std::is_void_v<R>) {
            if (mg_ts::v45_dispatch_draw_range(name_, static_cast<function_type>(base_), args...)) return;
        }
        if (mg_ts::availableAndActive() && mg_ts::classForName(name_) == mg_ts::backend_command_class::barrier)
            mg_ts::v44_enqueue_barrier_name(name_);
        return base_(args...);
    }

  private:
    base_type base_;
    const char* name_;
};

#endif // ZOMDROID_EXPERIMENTAL

#endif // MOBILEGLUES_THREADED_SUBMISSION_V44_WRAPPER_H
