// MobileGlues - gl/pz_repack_draw_router.cpp
// Project Zomboid experiment: normalize equivalent indexed draw entry points
// into the repack renderer's glDrawElements interception point. Unsupported or
// semantically different calls stay on their original backend entry point.

#include "pz_repack_draw_router.h"

#if defined(ZOMDROID_EXPERIMENTAL)

#include "../gles/loader.h"
#include "log.h"

#include <cstdlib>
#include <cstring>

namespace {

struct originals_t {
    glDrawElements_PTR draw_elements = nullptr;
    glDrawElementsInstanced_PTR draw_elements_instanced = nullptr;
    glDrawElementsBaseVertex_PTR draw_elements_base_vertex = nullptr;
    glDrawElementsInstancedBaseVertex_PTR draw_elements_instanced_base_vertex = nullptr;
    glDrawRangeElements_PTR draw_range_elements = nullptr;
    glDrawRangeElementsBaseVertex_PTR draw_range_elements_base_vertex = nullptr;
};

struct stats_t {
    unsigned long long calls = 0;
    unsigned long long plain = 0;
    unsigned long long instanced = 0;
    unsigned long long basevertex = 0;
    unsigned long long instanced_basevertex = 0;
    unsigned long long range = 0;
    unsigned long long range_basevertex = 0;

    unsigned long long normalized = 0;
    unsigned long long norm_instanced = 0;
    unsigned long long norm_basevertex = 0;
    unsigned long long norm_instanced_basevertex = 0;
    unsigned long long norm_range = 0;
    unsigned long long norm_range_basevertex = 0;

    unsigned long long tri6_u16_all = 0;
    unsigned long long tri6_u16_normalized = 0;
    unsigned long long fan4_u16_all = 0;
    unsigned long long fan4_u16_normalized = 0;

    unsigned long long fallback_nonzero_basevertex = 0;
    unsigned long long fallback_instance_count = 0;
    unsigned long long fallback_invalid_range = 0;
};

originals_t g_orig;
stats_t g_stats;

bool is_tri6_u16(GLenum mode, GLsizei count, GLenum type) {
    return mode == GL_TRIANGLES && count == 6 && type == GL_UNSIGNED_SHORT;
}

bool is_fan4_u16(GLenum mode, GLsizei count, GLenum type) {
    return mode == GL_TRIANGLE_FAN && count == 4 && type == GL_UNSIGNED_SHORT;
}

void note_input_shape(GLenum mode, GLsizei count, GLenum type) {
    if (is_tri6_u16(mode, count, type)) ++g_stats.tri6_u16_all;
    if (is_fan4_u16(mode, count, type)) ++g_stats.fan4_u16_all;
}

void note_normalized_shape(GLenum mode, GLsizei count, GLenum type) {
    if (is_tri6_u16(mode, count, type)) ++g_stats.tri6_u16_normalized;
    if (is_fan4_u16(mode, count, type)) ++g_stats.fan4_u16_normalized;
}

void report() {
    LOG_I("ZOMDROID_PZ_REPACK_ROUTE calls=%llu entry=%llu/%llu/%llu/%llu/%llu/%llu "
          "normalized=%llu route=%llu/%llu/%llu/%llu/%llu "
          "tri6_u16=%llu/%llu fan4_u16=%llu/%llu fallback=%llu/%llu/%llu",
          g_stats.calls,
          g_stats.plain, g_stats.instanced, g_stats.basevertex, g_stats.instanced_basevertex,
          g_stats.range, g_stats.range_basevertex,
          g_stats.normalized,
          g_stats.norm_instanced, g_stats.norm_basevertex, g_stats.norm_instanced_basevertex,
          g_stats.norm_range, g_stats.norm_range_basevertex,
          g_stats.tri6_u16_all, g_stats.tri6_u16_normalized,
          g_stats.fan4_u16_all, g_stats.fan4_u16_normalized,
          g_stats.fallback_nonzero_basevertex, g_stats.fallback_instance_count,
          g_stats.fallback_invalid_range)
}

void maybe_report() {
    const unsigned long long n = g_stats.calls;
    if (n == 1 || n == 1024 || n == 65536 || (n != 0 && n % 250000ULL == 0)) report();
}

void forward_normalized(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    ++g_stats.normalized;
    note_normalized_shape(mode, count, type);
    g_orig.draw_elements(mode, count, type, indices);
}

void router_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    ++g_stats.calls;
    ++g_stats.plain;
    note_input_shape(mode, count, type);
    g_orig.draw_elements(mode, count, type, indices);
    maybe_report();
}

void router_glDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                    GLsizei instancecount) {
    ++g_stats.calls;
    ++g_stats.instanced;
    note_input_shape(mode, count, type);
    if (instancecount == 1) {
        ++g_stats.norm_instanced;
        forward_normalized(mode, count, type, indices);
    } else {
        ++g_stats.fallback_instance_count;
        g_orig.draw_elements_instanced(mode, count, type, indices, instancecount);
    }
    maybe_report();
}

void router_glDrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                     GLint basevertex) {
    ++g_stats.calls;
    ++g_stats.basevertex;
    note_input_shape(mode, count, type);
    if (basevertex == 0) {
        ++g_stats.norm_basevertex;
        forward_normalized(mode, count, type, indices);
    } else {
        ++g_stats.fallback_nonzero_basevertex;
        g_orig.draw_elements_base_vertex(mode, count, type, indices, basevertex);
    }
    maybe_report();
}

void router_glDrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                              GLsizei instancecount, GLint basevertex) {
    ++g_stats.calls;
    ++g_stats.instanced_basevertex;
    note_input_shape(mode, count, type);
    if (instancecount == 1 && basevertex == 0) {
        ++g_stats.norm_instanced_basevertex;
        forward_normalized(mode, count, type, indices);
    } else {
        if (instancecount != 1) ++g_stats.fallback_instance_count;
        if (basevertex != 0) ++g_stats.fallback_nonzero_basevertex;
        g_orig.draw_elements_instanced_base_vertex(mode, count, type, indices, instancecount, basevertex);
    }
    maybe_report();
}

void router_glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                                const void* indices) {
    ++g_stats.calls;
    ++g_stats.range;
    note_input_shape(mode, count, type);
    if (end >= start) {
        ++g_stats.norm_range;
        forward_normalized(mode, count, type, indices);
    } else {
        ++g_stats.fallback_invalid_range;
        g_orig.draw_range_elements(mode, start, end, count, type, indices);
    }
    maybe_report();
}

void router_glDrawRangeElementsBaseVertex(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                                          const void* indices, GLint basevertex) {
    ++g_stats.calls;
    ++g_stats.range_basevertex;
    note_input_shape(mode, count, type);
    if (end >= start && basevertex == 0) {
        ++g_stats.norm_range_basevertex;
        forward_normalized(mode, count, type, indices);
    } else {
        if (end < start) ++g_stats.fallback_invalid_range;
        if (basevertex != 0) ++g_stats.fallback_nonzero_basevertex;
        g_orig.draw_range_elements_base_vertex(mode, start, end, count, type, indices, basevertex);
    }
    maybe_report();
}

template <typename Pointer, typename Slot>
Pointer capture(const Slot& slot) {
    return static_cast<Pointer>(slot);
}

} // namespace

void mg_pz_repack_draw_router_install(void) {
    const char* value = std::getenv("MOBILEGLUES_PZ_REPACK_RENDERER");
    if (value == nullptr || std::strcmp(value, "1") != 0) return;

    // Install after mg_pz_repack_renderer_install(). The glDrawElements pointer
    // captured here is deliberately the repack renderer wrapper, while the
    // other pointers still point at their original backend entry points.
    g_orig.draw_elements = capture<glDrawElements_PTR>(GLES.glDrawElements);
    g_orig.draw_elements_instanced = capture<glDrawElementsInstanced_PTR>(GLES.glDrawElementsInstanced);
    g_orig.draw_elements_base_vertex = capture<glDrawElementsBaseVertex_PTR>(GLES.glDrawElementsBaseVertex);
    g_orig.draw_elements_instanced_base_vertex =
        capture<glDrawElementsInstancedBaseVertex_PTR>(GLES.glDrawElementsInstancedBaseVertex);
    g_orig.draw_range_elements = capture<glDrawRangeElements_PTR>(GLES.glDrawRangeElements);
    g_orig.draw_range_elements_base_vertex =
        capture<glDrawRangeElementsBaseVertex_PTR>(GLES.glDrawRangeElementsBaseVertex);

    if (!g_orig.draw_elements) {
        LOG_W_FORCE("ZOMDROID_PZ_REPACK_ROUTE disabled reason=draw_elements_missing")
        return;
    }

    GLES.glDrawElements = router_glDrawElements;
    if (g_orig.draw_elements_instanced) GLES.glDrawElementsInstanced = router_glDrawElementsInstanced;
    if (g_orig.draw_elements_base_vertex) GLES.glDrawElementsBaseVertex = router_glDrawElementsBaseVertex;
    if (g_orig.draw_elements_instanced_base_vertex)
        GLES.glDrawElementsInstancedBaseVertex = router_glDrawElementsInstancedBaseVertex;
    if (g_orig.draw_range_elements) GLES.glDrawRangeElements = router_glDrawRangeElements;
    if (g_orig.draw_range_elements_base_vertex)
        GLES.glDrawRangeElementsBaseVertex = router_glDrawRangeElementsBaseVertex;

    LOG_I("ZOMDROID_PZ_REPACK_ROUTE enabled=1 normalize=basevertex0+range+single_instance "
          "telemetry=entrypoint+shape+fallback")
}

#else

void mg_pz_repack_draw_router_install(void) {}

#endif
