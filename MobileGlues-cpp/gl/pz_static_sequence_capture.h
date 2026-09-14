#ifndef MOBILEGLUES_PZ_STATIC_SEQUENCE_CAPTURE_H
#define MOBILEGLUES_PZ_STATIC_SEQUENCE_CAPTURE_H

#include <GL/gl.h>

void mg_pz_static_sequence_capture_arrays(GLenum mode, GLint first, GLsizei count, GLsizei instances,
                                          GLuint baseinstance = 0);
void mg_pz_static_sequence_capture_elements(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                            GLsizei instances, GLint basevertex = 0, GLuint baseinstance = 0);

#endif
