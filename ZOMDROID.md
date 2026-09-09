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

Schema 3 additionally reports exact repeated uniform values, vertex-attrib state and conservative
adjacent `glDrawElements` batching eligibility. `batch_e=candidates/adjacent/runs/max_run`; adjacent
is the upper bound on driver submissions a future multi-draw implementation could remove.
`batch_break=state/uniform/attrib/resource/other_draw/signature` explains why runs ended. Only direct
indexed triangle draws backed by an element buffer qualify, and every uncertain mutation is a barrier.
All associated
comparison/cache work remains inside `MOBILEGLUES_PZ_CENSUS=1`; disabling or omitting the variable
removes that work from the rendering path.

The independent VAO optimization is selected through the renderer environment:

```text
MOBILEGLUES_PZ_VAO_FASTPATH=1  # enabled
MOBILEGLUES_PZ_VAO_FASTPATH=0  # disabled (default)
```

It skips a repeated bind only when both MobileGlues' frontend VAO and the real driver VAO are known
to match. Internal renderer binds update the same per-context shadow. Every other case reaches GLES.

The first vertex-attrib fast path is independently opt-in:

```text
MOBILEGLUES_PZ_ATTRIB_FASTPATH=1  # enabled
MOBILEGLUES_PZ_ATTRIB_FASTPATH=0  # disabled (default)
```

It currently removes only exact repeated `glEnableVertexAttribArray` and
`glDisableVertexAttribArray` calls, and only while the real driver VAO is confirmed.

The uniform fast path is independently opt-in:

```text
MOBILEGLUES_PZ_UNIFORM_FASTPATH=1  # enabled
MOBILEGLUES_PZ_UNIFORM_FASTPATH=0  # disabled (default)
```

Only valid, single-value writes with an identical program, location, type and bit-exact payload are
skipped. Relink, deletion, context switches and unsupported array writes invalidate the shadow;
internal alpha-test and buffer-texture writes keep it synchronized.

Whole-buffer write maps reuse MobileGlues' tracked allocation size instead of issuing a synchronous
`GL_BUFFER_SIZE` driver query before every `glMapBuffer`. Buffers whose size is unknown still use the
driver query, so pass-through names and unusual allocation paths retain the previous behavior.

The experimental dynamic-buffer streaming path is independently opt-in:

```text
MOBILEGLUES_PZ_BUFFER_STREAMING=1  # CPU staging enabled
MOBILEGLUES_PZ_BUFFER_STREAMING=0  # direct driver mapping (default)
```

It intercepts only complete write-only invalidating maps of mutable, tracked buffers. Whole-buffer
maps using either `GL_MAP_INVALIDATE_BUFFER_BIT` or `GL_MAP_INVALIDATE_RANGE_BIT` qualify. The application
writes into aligned CPU staging memory; unmap replaces the driver's store and uploads the completed
buffer in one call, avoiding a direct map/unmap synchronization with an in-flight GPU buffer. Reads,
partial maps, immutable storage and unknown buffer names keep the normal driver path. While enabled,
the first map attempts emit `ZOMDROID_PZ_BUFFER_STREAMING_PATTERN` with the access flags, tracked size
and exact fallback reason.

Repeated discard-then-map cycles can additionally be coalesced:

```text
MOBILEGLUES_PZ_BUFFER_DISCARD_COALESCE=1  # remove the redundant discard call
MOBILEGLUES_PZ_BUFFER_DISCARD_COALESCE=0  # submit it directly (default)
```

After a buffer has completed one qualifying CPU-staged upload, a same-size, same-usage
`glBufferData(..., NULL, ...)` before its next staged upload stays in the frontend. The staged unmap
replaces the store with the completed bytes, collapsing the discard and upload into one driver call.
This applies only to mutable array and element buffers and requires
`MOBILEGLUES_PZ_BUFFER_STREAMING=1`. Milestones are reported as
`ZOMDROID_PZ_BUFFER_DISCARD_COALESCE`.

The fixed-state shadow is independently opt-in:

```text
MOBILEGLUES_PZ_STATE_SHADOW=1  # redundant fixed-state calls skipped
MOBILEGLUES_PZ_STATE_SHADOW=0  # direct driver calls (default)
```

It tracks blend equations/functions, blend color, color mask, cull/front face, depth function/mask
and front/back stencil state per current context. Only exact repeats are skipped. Combined and
separate blend/stencil entry points update the same semantic state. Skip milestones are logged as
`ZOMDROID_PZ_STATE_SHADOW_SKIP`.

Runtime mipmap generation for Project Zomboid's chunk render targets can be reduced independently:

```text
MOBILEGLUES_PZ_RUNTIME_MIPMAP_SKIP=1  # skip learned level-0-only regeneration
MOBILEGLUES_PZ_RUNTIME_MIPMAP_SKIP=0  # submit every generation (default)
```

The first generation for each texture always reaches GLES. Once a measured 1024x1024 chunk texture
requests a mipmapped minification filter and MobileGlues applies its existing level-0 compatibility
fallback, later `glGenerateMipmap` calls are skipped because ordinary sampling remains on level 0.
Other sizes, texture targets and textures that have not taken the fallback remain on the normal
driver path. Skip milestones are logged as `ZOMDROID_PZ_RUNTIME_MIPMAP_SKIP`.
