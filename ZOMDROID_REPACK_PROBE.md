# ZomDroid PZ Repack Probe

Base snapshot: `312855ae9a47b8fea1952e8aa80207fad6073788` (`zomdroid-threaded-stable-clean`).

This branch is an observe-only gate for the proposed PZ tiny-draw repack / instance-style renderer. It does not change draw arguments, shader sources, resources, draw order, or buffer contents.

## Enable

Keep the normal stable-clean experiment flags and add:

```text
MOBILEGLUES_PZ_REPACK_PROBE=1
```

`MOBILEGLUES_PZ_CENSUS=1` is still recommended so the ordinary 300-frame census can be compared with this probe.

Startup confirmation:

```text
ZOMDROID_PZ_REPACK_PROBE enabled=1 mode=backend_observe_only ...
```

The probe reports at indexed backend draw 1, 1024, 65536, then every 250000 indexed backend draws.

## Fields

- `indexed`: indexed backend draws observed by the probe.
- `tri`: `GL_TRIANGLES` indexed draws.
- `fan`: `GL_TRIANGLE_FAN` indexed draws.
- `shape`: structural repack candidates: `GL_TRIANGLES`, 3..96 indices, index count divisible by 3, one instance, supported unsigned index type. This is an upper-bound candidate count, not a correctness proof.
- `quad6_shape`: candidates with exactly 6 indices. This is intentionally named `shape`: topology has not yet been read/validated.
- `exact=3/6/9/12`: exact triangle index-count histogram.
- `le=12/24/48/96`: cumulative small-draw histogram.
- `type=u8/u16/u32/other`: index type histogram.
- `instanced`: draws already using more than one instance.
- `basev`: non-zero base-vertex draws.
- `offset32`: index pointers that look like EBO offsets (<= 32-bit address range).
- `relaxed=runs/adjacent/max_run/percent`: optimistic adjacency upper bound. It deliberately ignores vertex-buffer and vertex-attrib churn, and breaks when backend program/active-texture/texture binding changes. Uniform and fixed-state changes are not yet modeled, so this number must not be treated as safe-to-merge work.
- `delta=indexed/shape/quad6_shape/relaxed_adjacent`: activity since the previous probe report.

## Decision rule

The first test is only a go/no-go gate. A real renderer is justified only if the dense-building route shows a large `shape` population and a useful relaxed adjacency ceiling. If `shape` is small, or `relaxed_adjacent` remains near zero, abandon this route before implementing repacking.

If the gate is positive, phase 2 validates real index topology and vertex-layout compatibility before any draw is replaced. Phase 3 is the first behavior-changing A/B implementation.
