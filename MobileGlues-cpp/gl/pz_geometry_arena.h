// MobileGlues - gl/pz_geometry_arena.h
// Isolated Project Zomboid geometry-arena experiment.

#ifndef MOBILEGLUES_PZ_GEOMETRY_ARENA_H
#define MOBILEGLUES_PZ_GEOMETRY_ARENA_H

// Installs backend dispatch wrappers only when
// MOBILEGLUES_PZ_GEOMETRY_ARENA=1. The normal renderer and stable ZomDroid
// branch keep the original GLES table unchanged.
void mg_pz_geometry_arena_install(void);

#endif // MOBILEGLUES_PZ_GEOMETRY_ARENA_H
