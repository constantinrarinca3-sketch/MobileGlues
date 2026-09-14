#include "gl/pz_static_sequence_census.h"
#include "egl/context.h"
#include <EGL/egl.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" {
EGLBoolean eglMakeCurrent(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
EGLBoolean eglSwapBuffers(EGLDisplay, EGLSurface);
EGLBoolean eglSwapBuffersWithDamageKHR(EGLDisplay, EGLSurface, EGLint*, EGLint);
EGLBoolean eglSwapBuffersWithDamageEXT(EGLDisplay, EGLSurface, EGLint*, EGLint);
__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char*);
}

static std::string last_log;
static int failures = 0;
static int make_current_calls = 0;
static int swap_calls = 0;
static MGContext fake_context{};
thread_local MGContext* g_current_ctx = nullptr;
static unsigned long long next_context_id = 1;

extern "C" void write_log(const char* format, ...) {
    char line[8192] = {};
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    last_log = line;
}
extern "C" void write_log_n(const char*, ...) {}
int __android_log_print(int, const char*, const char*, ...) { return 0; }

extern "C" EGLBoolean mg_pz_original_eglMakeCurrent(EGLDisplay, EGLSurface, EGLSurface, EGLContext) {
    ++make_current_calls;
    fake_context.id = next_context_id;
    g_current_ctx = &fake_context;
    return EGL_TRUE;
}
extern "C" EGLBoolean mg_pz_original_eglSwapBuffers(EGLDisplay, EGLSurface) {
    ++swap_calls;
    return EGL_TRUE;
}
extern "C" EGLBoolean mg_pz_original_eglSwapBuffersWithDamageKHR(EGLDisplay, EGLSurface, EGLint*, EGLint) {
    ++swap_calls;
    return EGL_TRUE;
}
extern "C" EGLBoolean mg_pz_original_eglSwapBuffersWithDamageEXT(EGLDisplay, EGLSurface, EGLint*, EGLint) {
    ++swap_calls;
    return EGL_TRUE;
}
extern "C" __eglMustCastToProperFunctionPointerType mg_pz_original_eglGetProcAddress(const char*) {
    return nullptr;
}

static void expect(bool condition, const char* message) {
    if (condition) return;
    std::printf("FAIL %s\n", message);
    ++failures;
}

int main() {
    setenv("MOBILEGLUES_PZ_STATIC_SEQUENCE_CENSUS", "1", 1);
    const EGLContext a = reinterpret_cast<EGLContext>(static_cast<uintptr_t>(0x1000));
    next_context_id = 100;
    expect(eglMakeCurrent(nullptr, nullptr, nullptr, a) == EGL_TRUE, "make-current wrapper must forward success");
    expect(mg_pz_static_sequence_census_active, "first make-current must lazily initialize V5");
    for (int i = 0; i < 300; ++i) expect(eglSwapBuffers(nullptr, nullptr) == EGL_TRUE, "swap must forward success");
    expect(swap_calls == 300, "swap wrapper must forward exactly once");
    expect(last_log.find("frames=300") != std::string::npos, "swap wrapper must close static-sequence frames");
    expect(last_log.find("context_resets=1") != std::string::npos, "make-current must identify the first context");

    const auto p = eglGetProcAddress("eglSwapBuffers");
    expect(p == reinterpret_cast<__eglMustCastToProperFunctionPointerType>(&eglSwapBuffers),
           "eglGetProcAddress must return the V5 wrapper, not the renamed original");

    const EGLContext b = reinterpret_cast<EGLContext>(static_cast<uintptr_t>(0x2000));
    next_context_id = 200;
    eglMakeCurrent(nullptr, nullptr, nullptr, b);
    for (int i = 0; i < 100; ++i) eglSwapBuffersWithDamageKHR(nullptr, nullptr, nullptr, 0);
    for (int i = 0; i < 100; ++i) eglSwapBuffersWithDamageEXT(nullptr, nullptr, nullptr, 0);
    for (int i = 0; i < 100; ++i) eglSwapBuffers(nullptr, nullptr);
    expect(last_log.find("context_resets=1") != std::string::npos,
           "a context switch must invalidate cross-context sequence history");
    expect(make_current_calls == 2, "make-current wrapper must forward each call exactly once");

    // Drivers may recycle an EGLContext pointer. The stable context layer gives
    // each logical context a monotonic id, so the census must follow that id
    // rather than the raw driver handle.
    next_context_id = 300;
    eglMakeCurrent(nullptr, nullptr, nullptr, b);
    for (int i = 0; i < 300; ++i) eglSwapBuffers(nullptr, nullptr);
    expect(last_log.find("context_resets=1") != std::string::npos,
           "a recycled EGLContext handle with a new MGContext id must reset sequence history");

    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PZ static EGL checks passed", failures);
    return failures != 0;
}
