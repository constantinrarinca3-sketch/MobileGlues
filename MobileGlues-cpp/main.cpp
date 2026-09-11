// MobileGlues - main.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#include "config/settings.h"
#include "config/stats.h"
#include "egl/egl.h"
#include "egl/loader.h"
#include "gl/envvars.h"
#include "gl/gl.h"
#include "gl/log.h"
#include "gl/mg.h"
#include "gl/pz_census.h"
#include "gl/pz_repack_probe.h"
#include "gl/pz_repack_renderer.h"
#include "gl/pz_repack_draw_router.h"
#include "gles/loader.h"
#include "includes.h"
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

#define DEBUG 0

#ifndef __APPLE__
__attribute__((used))
#endif
const char* license = "GNU LGPL-2.1 License";

#if defined(ZOMDROID_EXPERIMENTAL)
extern "C" __attribute__((visibility("default"), used))
const char* mg_zomdroid_build_id(void) {
    return "MobileGlues-2.0.0-ZomDroid-material-stream-4.1";
}
#endif

void init_config() {
    if (!check_path()) return;
    config_refresh();
    // One dlopen of this library is one launch. Counting it here, before any
    // rendering work, means a game that crashes on the first frame still counts.
    bump_launch_count();
}

void show_license() {
    LOG_V("The Open Source License of MobileGlues: ");
    LOG_V("  %s", license);
}

#if PROFILING

PERFETTO_TRACK_EVENT_STATIC_STORAGE();

void init_perfetto() {
    perfetto::TracingInitArgs args;

    args.backends |= perfetto::kSystemBackend;
    perfetto::Tracing::Initialize(args);
    perfetto::TrackEvent::Register();
}
#endif

void proc_init() {
    init_config();

    clear_log();
    start_log();
    mg_pz_census_init();

    LOG_V("Initializing %s ...", RENDERERNAME);
    show_license();

    init_settings();

    load_libs();
    init_target_egl();
    init_target_gles();
    mg_pz_buffer_streaming_install();
    mg_pz_repack_renderer_install();
    mg_pz_repack_draw_router_install();
    init_target_gl();
}

__attribute__((constructor)) static void init() {
#if PROFILING
    init_perfetto();
#endif
    proc_init();
}

__attribute__((destructor)) static void deinit() {
    // Stop the experimental worker while the backend EGL/GLES libraries are
    // still loaded and while every command function pointer it may execute is
    // valid. Static-destruction order across translation units is unspecified;
    // relying on the worker object's destructor was a use-after-dlclose hazard.
    mg_pz_threaded_submission_shutdown();
#if PROFILING
    perfetto::TrackEvent::Flush();
#endif
}
