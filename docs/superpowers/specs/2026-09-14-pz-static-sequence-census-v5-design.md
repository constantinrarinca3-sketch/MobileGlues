# PZ Static-Sequence Census V5

## Base and isolation

V5 is based on stable commit `c987cc0d3d4268fd0c2fbc549500738c633ec05f` (`fix: honor PZ tile-depth drawPixels per draw`).
It lives only on `zomdroid-pz-static-sequence-census-v5`. Stable branches are not modified.
The V5 branch is explicitly ignored by the iOS push workflow and is not added to the Android ARM64 push whitelist.

## Goal

Measure whether Project Zomboid submits enough repeated renderer work to justify a later retained/replay compiler. V5 is diagnostic only: it never skips, merges, reorders, replays, or changes a draw.

Enable only with:

`MOBILEGLUES_PZ_STATIC_SEQUENCE_CENSUS=1`

Every other value, including an absent variable, leaves V5 disabled.

## Integration strategy

The validated `drawing.cpp`, `egl.cpp`, `pz_census.cpp`, threaded submission, and the `drawPixels` fix remain source-unchanged.

For the V5 experimental build only, CMake source-renames the relevant public `glDraw*` and EGL entry points in their original translation units. Two new frontend translation units export the normal names:

- `pz_static_sequence_frontend.cpp` captures full draw identity and immediately forwards to the renamed original draw implementation.
- `pz_static_sequence_egl.cpp` observes successful `eglMakeCurrent`, closes frames after the three swap paths, and makes `eglGetProcAddress` return the V5 wrappers for wrapped EGL names.

This preserves the exact validated renderer implementation while giving the diagnostic the arguments that the older generic census hook does not expose (`first`, index type/offset, base vertex and base instance).

## Captured draw identity

The structural signature includes:

- indexed vs array draw;
- primitive mode, count and instance count;
- array `first`;
- index type and index pointer/byte offset token;
- base vertex and base instance;
- current program and VAO;
- frontend array and element buffer names;
- draw framebuffer;
- tracked bindings for texture units 0-3 for `GL_TEXTURE_2D` and `GL_TEXTURE_2D_ARRAY`.

A second tracked-resource fingerprint additionally includes the tracked lifetime, content version and size of the array/element buffers when those identities are known.

The tracked-resource metric is deliberately **not** called replay-safe or exact-content reuse: V5 does not yet fingerprint every uniform, fixed-function state value, sampler unit above 3, image binding, SSBO, or other possible draw dependency. It is a feasibility signal only.

## Cross-frame measurements

For every frame V5 records at most 16,384 draw signatures. Excess draws are not allocated dynamically; they are counted as `overflow`.

Across consecutive frames it reports:

- `shape_pos_repeat`: same structural signature at the same draw position;
- `block_repeat`: draws belonging to verified repeated 16-draw blocks, even when shifted in the stream;
- `repeatable` / `repeat_pct`: union of the two structural repeat signals;
- `tracked_resource_known`, `tracked_resource_repeat`, `tracked_resource_pct`;
- `tracked_persist2`, `tracked_persist10`, `tracked_persist100`;
- `longest_pos` and `longest_block`;
- capture high-water and overflow;
- context reset count.

A context change clears cross-context history. Reports are emitted every 300 presented frames as:

`ZOMDROID_PZ_STATIC_SEQ schema=1 ...`

## Safety invariants

1. No GL/EGL query is added to the draw path.
2. No backend command is suppressed or reordered.
3. All wrappers forward to the original implementation exactly once.
4. V5 storage is bounded.
5. Disabled V5 does not allocate census vectors.
6. Index offsets, base vertex and base instance are part of structural identity, preventing the false-positive class found during implementation.
7. Hash block candidates are verified against their underlying signatures before being counted.
8. Material Stream V4.x remains frozen and is not reintroduced.

## Decision gate

V5 does not authorize a replay cache by itself. A high repeat percentage only justifies the next diagnostic stage, which must add complete draw-state dependency fingerprints before any rendering mutation is allowed.
