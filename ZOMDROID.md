# MobileGlues for ZomDroid — ARM64 optimization branch

This branch keeps upstream MobileGlues rendering behavior intact and adds an isolated Android
ARM64 build named `libMobileGluesZomDroid.so`. It is an experimental renderer candidate for
Project Zomboid Build 42 in ZomDroid; it is not a release replacement for NG-GL4ES.

## Safety boundary

- distinct library name and renderer selection, so NG-GL4ES remains untouched;
- system EGL/GLES are loaded dynamically by MobileGlues;
- the launcher must set `MG_DIR_PATH` to an app-private directory before loading the library;
- no ANGLE library is bundled or selected by this build;
- the Actions gate requires ARM64 ELF, 16 KB page compatibility and the EGL/GL/GLX entry points
  used by ZomDroid GLFW;
- LGPL-2.1 license and corresponding source must accompany any redistributed binary.

## First device gate

The first APK integration should expose `MOBILEGLUES_EXPERIMENTAL` as a separate renderer and use
GLES 3.2. Test cold start, credits, menu, world load, zoom, lighting/weather, world exit and a
five-minute driving route. Any black world, missing texture, shader failure, delayed world or crash
rejects the renderer without changing another route.

## Project Zomboid census (optimization branch)

The diagnostic census is controlled only through the renderer environment:

```text
MOBILEGLUES_PZ_CENSUS=1  # enabled
MOBILEGLUES_PZ_CENSUS=0  # disabled
```

An absent variable is also disabled. When enabled, the renderer emits one compact
`ZOMDROID_PZ_CENSUS` line every 300 presented frames with frame-time buckets, draw workload,
redundant state calls and buffer upload/map traffic. It does not alter rendering or identify
zombies by itself; compare repeatable routes with low and high zombie counts. Keep it disabled
for ordinary play because the per-call counting is diagnostic overhead.

Schema 2 additionally reports exact repeated uniform values and vertex-attrib state. All associated
comparison/cache work remains inside `MOBILEGLUES_PZ_CENSUS=1`; disabling or omitting the variable
removes that work from the rendering path.

The independent VAO optimization is selected through the renderer environment:

```text
MOBILEGLUES_PZ_VAO_FASTPATH=1  # enabled
MOBILEGLUES_PZ_VAO_FASTPATH=0  # disabled (default)
```

It skips a repeated bind only when both MobileGlues' frontend VAO and the real driver VAO are known
to match. Internal renderer binds update the same per-context shadow. Every other case reaches GLES.
