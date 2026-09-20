// Ordered Java -> MobileGlues model-pass markers for the ZomDroid build.
#ifndef MOBILEGLUES_PZ_MODEL_PASS_H
#define MOBILEGLUES_PZ_MODEL_PASS_H

#include <GL/gl.h>
#include <cstdint>

enum class mg_pz_model_pass : uint8_t {
    none = 0,
    opaque = 1,
    transparent = 2,
};

constexpr GLenum MG_PZ_MARKER_SOURCE_APPLICATION = 0x824A;
constexpr GLenum MG_PZ_MARKER_TYPE = 0x8268;
constexpr GLuint MG_PZ_MARKER_OPAQUE_BEGIN = 0x5A420101U;
constexpr GLuint MG_PZ_MARKER_OPAQUE_END = 0x5A420102U;
constexpr GLuint MG_PZ_MARKER_TRANSPARENT_BEGIN = 0x5A420103U;
constexpr GLuint MG_PZ_MARKER_TRANSPARENT_END = 0x5A420104U;
constexpr GLint MG_PZ_MARKER_UNIFORM_LOCATION = -1;

// Returns true only for one of the four reserved ZBetterFPS messages. A true
// result means the frontend consumed it and it must not be sent to the driver.
bool mg_pz_model_pass_handle_marker(GLenum source, GLenum type, GLuint id);
// Consumes the GL20-safe Java transport: glUniform1f(-1, bit_cast<float>(id)).
// Location -1 is a guaranteed no-op if an unmatched build forwards the call.
bool mg_pz_model_pass_handle_uniform_marker(GLint location, GLfloat value);
mg_pz_model_pass mg_pz_model_pass_current();

// Census hooks. All are constant-time no-ops while census is disabled.
void mg_pz_model_pass_gl_call();
void mg_pz_model_pass_draw(GLsizei count, GLsizei instances);
void mg_pz_model_pass_present();
void mg_pz_model_pass_reset();

#endif
