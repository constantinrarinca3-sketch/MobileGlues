// V5 diagnostic EGL frontend. egl.cpp is source-renamed by CMake so these
// public wrappers can close census frames and track context identity without
// modifying the validated EGL implementation.

#include "pz_static_sequence_census.h"

#include <EGL/egl.h>
#include <cstdint>
#include <cstring>

#if defined(__GNUC__)
#define MG_PZ_EXPORT __attribute__((visibility("default")))
#else
#define MG_PZ_EXPORT
#endif

extern "C" {
EGLBoolean mg_pz_original_eglMakeCurrent(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
EGLBoolean mg_pz_original_eglSwapBuffers(EGLDisplay, EGLSurface);
EGLBoolean mg_pz_original_eglSwapBuffersWithDamageKHR(EGLDisplay, EGLSurface, EGLint*, EGLint);
EGLBoolean mg_pz_original_eglSwapBuffersWithDamageEXT(EGLDisplay, EGLSurface, EGLint*, EGLint);
__eglMustCastToProperFunctionPointerType mg_pz_original_eglGetProcAddress(const char*);

MG_PZ_EXPORT EGLBoolean eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
    mg_pz_static_sequence_ensure_initialized();
    const EGLBoolean result = mg_pz_original_eglMakeCurrent(dpy, draw, read, ctx);
    if (result == EGL_TRUE && mg_pz_static_sequence_census_active) {
        const auto token = ctx == EGL_NO_CONTEXT ? 0ULL
                                                 : static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(ctx));
        mg_pz_static_sequence_context_changed(token);
    }
    return result;
}

MG_PZ_EXPORT EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
    mg_pz_static_sequence_ensure_initialized();
    const EGLBoolean result = mg_pz_original_eglSwapBuffers(dpy, surface);
    if (mg_pz_static_sequence_census_active) mg_pz_static_sequence_present();
    return result;
}

MG_PZ_EXPORT EGLBoolean eglSwapBuffersWithDamageKHR(EGLDisplay dpy, EGLSurface surface, EGLint* rects,
                                                    EGLint n_rects) {
    mg_pz_static_sequence_ensure_initialized();
    const EGLBoolean result = mg_pz_original_eglSwapBuffersWithDamageKHR(dpy, surface, rects, n_rects);
    if (mg_pz_static_sequence_census_active) mg_pz_static_sequence_present();
    return result;
}

MG_PZ_EXPORT EGLBoolean eglSwapBuffersWithDamageEXT(EGLDisplay dpy, EGLSurface surface, EGLint* rects,
                                                    EGLint n_rects) {
    mg_pz_static_sequence_ensure_initialized();
    const EGLBoolean result = mg_pz_original_eglSwapBuffersWithDamageEXT(dpy, surface, rects, n_rects);
    if (mg_pz_static_sequence_census_active) mg_pz_static_sequence_present();
    return result;
}

MG_PZ_EXPORT __eglMustCastToProperFunctionPointerType eglGetProcAddress(const char* procname) {
    if (procname != nullptr) {
        if (std::strcmp(procname, "eglMakeCurrent") == 0)
            return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(&eglMakeCurrent);
        if (std::strcmp(procname, "eglSwapBuffers") == 0)
            return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(&eglSwapBuffers);
        if (std::strcmp(procname, "eglSwapBuffersWithDamageKHR") == 0)
            return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(&eglSwapBuffersWithDamageKHR);
        if (std::strcmp(procname, "eglSwapBuffersWithDamageEXT") == 0)
            return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(&eglSwapBuffersWithDamageEXT);
        if (std::strcmp(procname, "eglGetProcAddress") == 0)
            return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(&eglGetProcAddress);
    }
    return mg_pz_original_eglGetProcAddress(procname);
}
}
