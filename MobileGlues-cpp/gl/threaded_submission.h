// MobileGlues - gl/threaded_submission.h
// V4.8 performance-ceiling wrapper: retain V4.5 draw-range classification and
// elide only exact redundant scalar integer texture-parameter submissions.

#ifndef MOBILEGLUES_THREADED_SUBMISSION_V44_WRAPPER_H
#define MOBILEGLUES_THREADED_SUBMISSION_V44_WRAPPER_H

#define mg_ts_dispatch_slot mg_ts_dispatch_slot_v43
#include "threaded_submission_v43_base.h"
#undef mg_ts_dispatch_slot

#if defined(ZOMDROID_EXPERIMENTAL)

#include "texture.h"

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

inline TextureObject* v48_exact_bound_texture(GLenum target) {
    const TextureTarget converted = ConvertGLEnumToTextureTarget(target);
    if (converted == TextureTarget::UNKNWON) return nullptr;

    // Require the driver's tracked binding and the frontend object binding to
    // agree before using any object-local parameter shadow. This deliberately
    // disables the optimization when the driver binding shadow is untrustworthy
    // (for example while FSR1 is enabled) or an internal GLES.* bind bypassed the
    // frontend. In those cases the original hard-barrier call is preserved.
    GLuint driver_texture = 0;
    if (!mg_driver_texture_binding(target, &driver_texture)) return nullptr;
    TextureObject* texture = mgGetTexObjectByTarget(target);
    if (texture == nullptr || texture->texture != driver_texture) return nullptr;
    return texture;
}

inline void v48_clear_integer_texture_param_shadow(TextureObject* texture) {
    if (texture == nullptr) return;
    for (auto& entry : texture->pz_integer_param_shadow) entry.valid = false;
}

inline void v48_clear_bound_integer_texture_param_shadow(GLenum target) {
    v48_clear_integer_texture_param_shadow(v48_exact_bound_texture(target));
}

// Returns true only when this exact texture object has already had the exact
// effective integer value submitted for this pname. No GL default is assumed.
// If there is no spare slot, the call stays conservative and is forwarded.
inline bool v48_texture_parameteri_is_redundant(GLenum target, GLenum pname, GLint param) {
    TextureObject* texture = v48_exact_bound_texture(target);
    if (texture == nullptr) return false;

    for (auto& entry : texture->pz_integer_param_shadow) {
        if (!entry.valid || entry.pname != pname) continue;
        if (entry.param == param) return true;
        entry.param = param;
        return false;
    }

    for (auto& entry : texture->pz_integer_param_shadow) {
        if (entry.valid) continue;
        entry.pname = pname;
        entry.param = param;
        entry.valid = true;
        return false;
    }
    return false;
}

template <typename... Args>
inline bool v48_dispatch_tex_parameteri(const char*, void (*)(Args...), Args...) {
    return false;
}

inline bool v48_dispatch_tex_parameteri(const char* name,
                                        void (*)(GLenum, GLenum, GLint),
                                        GLenum target, GLenum pname, GLint param) {
    if (!availableAndActive() || !nameEquals(name, "glTexParameteri")) return false;
    // Called by gl/texture.cpp after pname conversion and after the PZ runtime
    // mipmap path has converted the effective parameter, so this is exactly the
    // value that would otherwise be submitted to the GLES driver.
    return v48_texture_parameteri_is_redundant(target, pname, param);
}

// Alternate texture-parameter entry points can mutate the same object state.
// Invalidate before forwarding so the next integer call has to re-establish an
// exact submitted value before it can be elided.
template <typename... Args>
inline void v48_note_other_texture_parameter(const char*, void (*)(Args...), Args...) {}

inline void v48_note_other_texture_parameter(const char* name,
                                             void (*)(GLenum, GLenum, GLfloat),
                                             GLenum target, GLenum, GLfloat) {
    if (nameEquals(name, "glTexParameterf")) v48_clear_bound_integer_texture_param_shadow(target);
}

inline void v48_note_other_texture_parameter(const char* name,
                                             void (*)(GLenum, GLenum, const GLfloat*),
                                             GLenum target, GLenum, const GLfloat*) {
    if (nameEquals(name, "glTexParameterfv")) v48_clear_bound_integer_texture_param_shadow(target);
}

inline void v48_note_other_texture_parameter(const char* name,
                                             void (*)(GLenum, GLenum, const GLint*),
                                             GLenum target, GLenum, const GLint*) {
    if (nameEquals(name, "glTexParameteriv") || nameEquals(name, "glTexParameterIiv"))
        v48_clear_bound_integer_texture_param_shadow(target);
}

inline void v48_note_other_texture_parameter(const char* name,
                                             void (*)(GLenum, GLenum, const GLuint*),
                                             GLenum target, GLenum, const GLuint*) {
    if (nameEquals(name, "glTexParameterIuiv")) v48_clear_bound_integer_texture_param_shadow(target);
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
            if (mg_ts::v48_dispatch_tex_parameteri(name_, static_cast<function_type>(base_), args...)) return;
            mg_ts::v48_note_other_texture_parameter(name_, static_cast<function_type>(base_), args...);
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
