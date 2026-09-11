// MobileGlues - gl/pz_repack_renderer.h
// Isolated Project Zomboid tiny-quad repack renderer experiment.

#ifndef MOBILEGLUES_PZ_REPACK_RENDERER_H
#define MOBILEGLUES_PZ_REPACK_RENDERER_H

// Installs only when MOBILEGLUES_PZ_REPACK_RENDERER=1. The implementation is
// fail-closed: unsupported draw/state patterns stay on the original backend.
void mg_pz_repack_renderer_install(void);

#endif // MOBILEGLUES_PZ_REPACK_RENDERER_H
