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

Schema 5 additionally reports exact repeated uniform values, vertex-attrib state and conservative
adjacent `glDrawElements` batching eligibility. `batch_e=candidates/adjacent/runs/max_run`; adjacent
is the upper bound on driver submissions a future multi-draw implementation could remove.
`batch_break=state/uniform/attrib/resource/other_draw/signature` explains why runs ended. Only direct
indexed triangle draws backed by an element buffer qualify, and every uncertain mutation is a barrier.
All associated
comparison/cache work remains inside `MOBILEGLUES_PZ_CENSUS=1`; disabling or omitting the variable
removes that work from the rendering path.

All bounded `ZOMDROID_*` optimization breadcrumbs and the threaded-submission report follow the
same switch. With Census disabled, shader/texture diagnostic queries, breadcrumb counters and
threaded queue telemetry stay off. Actual renderer failures remain logged.

The same census line also measures uncompressed texture traffic without another switch.
`tex_upload=image+subimage/data/bytes/largest`, `tex_src=RGBA/BGRA/other`, and
`tex_convert=calls/bytes` show whether CPU pixel conversion is a meaningful target. `tex_pbo`
counts uploads already sourced from an unpack PBO; `tex_drop` counts rejected conversions.
`tex_frames=any/over20/over33/over50/over100` shows exactly how many frames carrying texture data
also fell into each frame-time bucket.

Large Project Zomboid RGB/RGBA atlases can be stored at full resolution in ETC2:

```text
MOBILEGLUES_PZ_ETC2=1        # compress eligible 2D atlases of at least 512x512
MOBILEGLUES_PZ_ETC2_CACHE=1  # reuse content-addressed ETC2 blocks across launches
```

Both switches default to disabled. ETC2 reduces RGB8 storage sixfold and RGBA8 storage fourfold;
it is intended to reduce texture-memory pressure rather than steady-state draw time. The optional
disk cache avoids paying the CPU encoding cost after the first run. It defaults to the app-private
`/data/data/com.zomdroid/files/ngg_etc2cache` directory and a 1536 MiB LRU cap. The launcher may
override these through `MOBILEGLUES_PZ_ETC2_CACHE_DIR` and `MOBILEGLUES_PZ_ETC2_CACHE_MB`.

Mip levels inherit the compressed format selected at level zero. Block-aligned subimage updates are
encoded and submitted with `glCompressedTexSubImage2D`; an incompatible update is rejected instead
of writing uncompressed bytes into compressed storage. With Census enabled, `ZOMDROID_PZ_ETC2`
reports encoded uploads, cache hits, byte reduction, rejected updates and cumulative encode/I/O time.

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
MOBILEGLUES_PZ_BUFFER_STREAMING=1  # CPU staging + automatic persistent mapping
MOBILEGLUES_PZ_BUFFER_STREAMING=0  # direct driver mapping (default)
```

It intercepts only complete write-only invalidating maps of mutable, tracked buffers. Whole-buffer
maps using either `GL_MAP_INVALIDATE_BUFFER_BIT` or `GL_MAP_INVALIDATE_RANGE_BIT` qualify. The application
writes into aligned CPU staging memory; unmap replaces the driver's store and uploads the completed
buffer in one call, avoiding a direct map/unmap synchronization with an in-flight GPU buffer. Once a
repeated discard/full-map pattern is proven, `GL_EXT_buffer_storage` is available and the buffer is
not shared with another context, the stream is promoted automatically to coherent persistent
mapping. Later writes go directly into a four-slot GPU ring and fences prevent reuse while a draw is
still in flight, removing the staging-to-driver copy. Reads, partial maps, immutable storage and
unknown buffer names keep the normal driver path. While enabled, the first map attempts emit
`ZOMDROID_PZ_BUFFER_STREAMING_PATTERN`; persistent milestones use
`ZOMDROID_PZ_PERSISTENT_BUFFER_STREAM` when Census is enabled.

Repeated discard-then-map cycles can additionally be coalesced:

```text
MOBILEGLUES_PZ_BUFFER_DISCARD_COALESCE=1  # remove the redundant discard call
MOBILEGLUES_PZ_BUFFER_DISCARD_COALESCE=0  # submit it directly (default)
```

After a buffer has completed one qualifying CPU-staged upload, a same-size, same-usage
`glBufferData(..., NULL, ...)` before its next staged upload stays in the frontend. The staged unmap
replaces the store with the completed bytes, collapsing the discard and upload into one driver call.
This applies only to streamed array and element buffers and requires
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

## Dedicated GL submission thread (high-risk experiment)

The separate `zomdroid-threaded-submission-experimental` branch can move backend GL submission to
a worker thread:

```text
MOBILEGLUES_PZ_THREADED_SUBMISSION=1  # worker owns the EGL context and submits GL
MOBILEGLUES_PZ_THREADED_SUBMISSION=0  # direct submission on the render thread (default)
```

The application thread retains MobileGlues state translation and records ordered backend commands
into compact packets in a fixed SPSC queue. Each packet carries up to 32 calls and is published,
woken and completed as one queue unit; synchronous calls and presentation flush a partial packet.
Commands with return values, output pointers or caller-owned input that
cannot safely outlive the call wait for the worker. Small uniform arrays and buffer uploads are
copied before returning so those calls can remain asynchronous. Draws remain asynchronous only when
the element and enabled vertex inputs are backed by GL buffers. Presentation drains the frame and
returns the backend `eglSwapBuffers` result so surface loss is reported on the calling thread.

This path changes EGL context ownership and is deliberately isolated from the frozen stable branch.
Its `ZOMDROID_PZ_THREADED_SUBMISSION` report shows command and packet totals, average packet fill,
synchronous waits, queue-full waits, presentation wait time, maximum queue depth and swap failures.
The report is collected and emitted only when `MOBILEGLUES_PZ_CENSUS=1`.
