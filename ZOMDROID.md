# MobileGlues for ZomDroid — experimental ARM64 fork

This branch keeps upstream MobileGlues rendering behavior intact and adds an isolated Android
ARM64 build named `libMobileGluesZomDroid.so`. It is an experimental renderer candidate for
Project Zomboid Build 42 in ZomDroid; it is not a release replacement for NG-GL4ES.

## Safety boundary

- distinct library name and renderer selection, so NG-GL4ES remains untouched;
- system EGL/GLES are loaded dynamically by MobileGlues;
- the launcher must set `MG_DIR_PATH` to an app-private directory before loading the library;
- no ANGLE library is bundled or selected by this build;
- the Actions gate requires ARM64 ELF and the EGL/GL/GLX entry points used by ZomDroid GLFW;
- LGPL-2.1 license and corresponding source must accompany any redistributed binary.

## First device gate

The first APK integration should expose `MOBILEGLUES_EXPERIMENTAL` as a separate renderer and use
GLES 3.2. Test cold start, credits, menu, world load, zoom, lighting/weather, world exit and a
five-minute driving route. Any black world, missing texture, shader failure, delayed world or crash
rejects the renderer without changing another route.
