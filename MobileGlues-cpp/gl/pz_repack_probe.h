// MobileGlues - gl/pz_repack_probe.h
// Diagnostic gate for the isolated Project Zomboid tiny-draw repack experiment.

#ifndef MOBILEGLUES_PZ_REPACK_PROBE_H
#define MOBILEGLUES_PZ_REPACK_PROBE_H

// Installs backend dispatch probes only when MOBILEGLUES_PZ_REPACK_PROBE=1.
// The probe never changes draw arguments, resources, ordering, or shaders.
void mg_pz_repack_probe_install(void);

#endif // MOBILEGLUES_PZ_REPACK_PROBE_H
