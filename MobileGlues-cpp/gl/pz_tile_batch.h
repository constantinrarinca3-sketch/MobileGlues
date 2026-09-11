// MobileGlues - gl/pz_tile_batch.h
// Experimental order-preserving Project Zomboid StateRun compiler.

#ifndef MOBILEGLUES_PZ_TILE_BATCH_H
#define MOBILEGLUES_PZ_TILE_BATCH_H

#include <GL/gl.h>

#if defined(ZOMDROID_EXPERIMENTAL)

using mg_pz_tile_batch_multidraw_fn = void(GLAPIENTRY*)(GLenum, const GLsizei*, GLenum,
                                                        const void* const*, GLsizei);

void mg_pz_tile_batch_note_shader(GLuint shader, bool eligible);
bool mg_pz_tile_batch_shader_eligible(GLuint shader);
void mg_pz_tile_batch_shader_deleted(GLuint shader);
void mg_pz_tile_batch_attach_shader(GLuint program, GLuint shader);
void mg_pz_tile_batch_detach_shader(GLuint program, GLuint shader);
void mg_pz_tile_batch_program_linked(GLuint program);
void mg_pz_tile_batch_program_deleted(GLuint program);

// Returns true when the value belongs to the rewritten PZ default shader and
// has been retained by the batcher instead of sent to GLES immediately.
bool mg_pz_tile_batch_uniform1f(GLuint program, GLint location, GLfloat value);

// Returns true when this range is retained for an ordered combined draw.
bool mg_pz_tile_batch_draw_range(GLenum mode, GLuint start, GLuint end, GLsizei count,
                                 GLenum type, const void* indices);

// Called at the GLES dispatch boundary. Any real state/resource/query command
// is an ordering boundary for retained ranges.
void mg_pz_tile_batch_before_backend(const char* command);
void mg_pz_tile_batch_flush();
void mg_pz_tile_batch_present();

#if defined(MOBILEGLUES_TESTING)
void mg_pz_tile_batch_test_backend(mg_pz_tile_batch_multidraw_fn function);
#endif

#endif

#endif
