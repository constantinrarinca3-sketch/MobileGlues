// V5 diagnostic frontend: preserve the public GL entry points while the real
// drawing.cpp definitions are source-renamed by CMake. No rendering behavior is
// changed; each wrapper only snapshots draw identity when the opt-in census is on.

#include "pz_static_sequence_capture.h"
#include "pz_static_sequence_census.h"

#if defined(__GNUC__)
#define MG_PZ_EXPORT __attribute__((visibility("default")))
#else
#define MG_PZ_EXPORT
#endif

extern "C" {
void mg_pz_original_glDrawArrays(GLenum, GLint, GLsizei);
void mg_pz_original_glDrawArraysInstanced(GLenum, GLint, GLsizei, GLsizei);
void mg_pz_original_glDrawElements(GLenum, GLsizei, GLenum, const void*);
void mg_pz_original_glDrawElementsInstanced(GLenum, GLsizei, GLenum, const void*, GLsizei);
void mg_pz_original_glDrawElementsBaseVertex(GLenum, GLsizei, GLenum, const void*, GLint);
void mg_pz_original_glDrawRangeElements(GLenum, GLuint, GLuint, GLsizei, GLenum, const void*);
void mg_pz_original_glDrawRangeElementsBaseVertex(GLenum, GLuint, GLuint, GLsizei, GLenum, const void*, GLint);
void mg_pz_original_glDrawElementsInstancedBaseVertex(GLenum, GLsizei, GLenum, const void*, GLsizei, GLint);
void mg_pz_original_glDrawArraysInstancedBaseInstance(GLenum, GLint, GLsizei, GLsizei, GLuint);
void mg_pz_original_glDrawElementsInstancedBaseInstance(GLenum, GLsizei, GLenum, const void*, GLsizei, GLuint);
void mg_pz_original_glDrawElementsInstancedBaseVertexBaseInstance(GLenum, GLsizei, GLenum, const void*, GLsizei,
                                                                  GLint, GLuint);

MG_PZ_EXPORT void glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    if (mg_pz_static_sequence_census_active) mg_pz_static_sequence_capture_arrays(mode, first, count, 1, 0);
    mg_pz_original_glDrawArrays(mode, first, count);
}

MG_PZ_EXPORT void glDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instancecount) {
    if (mg_pz_static_sequence_census_active)
        mg_pz_static_sequence_capture_arrays(mode, first, count, instancecount, 0);
    mg_pz_original_glDrawArraysInstanced(mode, first, count, instancecount);
}

MG_PZ_EXPORT void glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    if (mg_pz_static_sequence_census_active)
        mg_pz_static_sequence_capture_elements(mode, count, type, indices, 1, 0, 0);
    mg_pz_original_glDrawElements(mode, count, type, indices);
}

MG_PZ_EXPORT void glDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                          GLsizei instancecount) {
    if (mg_pz_static_sequence_census_active)
        mg_pz_static_sequence_capture_elements(mode, count, type, indices, instancecount, 0, 0);
    mg_pz_original_glDrawElementsInstanced(mode, count, type, indices, instancecount);
}

MG_PZ_EXPORT void glDrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                           GLint basevertex) {
    if (mg_pz_static_sequence_census_active)
        mg_pz_static_sequence_capture_elements(mode, count, type, indices, 1, basevertex, 0);
    mg_pz_original_glDrawElementsBaseVertex(mode, count, type, indices, basevertex);
}

MG_PZ_EXPORT void glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                                      const void* indices) {
    if (mg_pz_static_sequence_census_active)
        mg_pz_static_sequence_capture_elements(mode, count, type, indices, 1, 0, 0);
    mg_pz_original_glDrawRangeElements(mode, start, end, count, type, indices);
}

MG_PZ_EXPORT void glDrawRangeElementsBaseVertex(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                                                const void* indices, GLint basevertex) {
    if (mg_pz_static_sequence_census_active)
        mg_pz_static_sequence_capture_elements(mode, count, type, indices, 1, basevertex, 0);
    mg_pz_original_glDrawRangeElementsBaseVertex(mode, start, end, count, type, indices, basevertex);
}

MG_PZ_EXPORT void glDrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                                    GLsizei instancecount, GLint basevertex) {
    if (mg_pz_static_sequence_census_active)
        mg_pz_static_sequence_capture_elements(mode, count, type, indices, instancecount, basevertex, 0);
    mg_pz_original_glDrawElementsInstancedBaseVertex(mode, count, type, indices, instancecount, basevertex);
}

MG_PZ_EXPORT void glDrawArraysInstancedBaseInstance(GLenum mode, GLint first, GLsizei count, GLsizei instancecount,
                                                    GLuint baseinstance) {
    if (mg_pz_static_sequence_census_active)
        mg_pz_static_sequence_capture_arrays(mode, first, count, instancecount, baseinstance);
    mg_pz_original_glDrawArraysInstancedBaseInstance(mode, first, count, instancecount, baseinstance);
}

MG_PZ_EXPORT void glDrawElementsInstancedBaseInstance(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                                      GLsizei instancecount, GLuint baseinstance) {
    if (mg_pz_static_sequence_census_active)
        mg_pz_static_sequence_capture_elements(mode, count, type, indices, instancecount, 0, baseinstance);
    mg_pz_original_glDrawElementsInstancedBaseInstance(mode, count, type, indices, instancecount, baseinstance);
}

MG_PZ_EXPORT void glDrawElementsInstancedBaseVertexBaseInstance(GLenum mode, GLsizei count, GLenum type,
                                                                const void* indices, GLsizei instancecount,
                                                                GLint basevertex, GLuint baseinstance) {
    if (mg_pz_static_sequence_census_active)
        mg_pz_static_sequence_capture_elements(mode, count, type, indices, instancecount, basevertex, baseinstance);
    mg_pz_original_glDrawElementsInstancedBaseVertexBaseInstance(mode, count, type, indices, instancecount,
                                                                 basevertex, baseinstance);
}
}
