<!-- GENERATED (`make docs-fragments-write`); do not hand-edit. -->
# AGENTS.md — core/src/sycl

Generated index. Match touched paths against `Touching`; read each matching
page first (none -> skip). Text above table binds every file. `Touching`
relative; leading `/` = repo root. New invariant -> `AGENTS.d/` page
([how](../../../docs/development/agents-index.md)).

Orientation for agents working on SYCL / DPC++ backend runtime. Parent:
[../../AGENTS.md](../../AGENTS.md).

## Topic pages

| Touching | Read first | Invariant |
| --- | --- | --- |
| `/core/src/meson.build`, `check_aot_image.py`, `coff_add_anchor.py`, `common.cpp`, `queue_props.h` | [build-and-aot-invariants](AGENTS.d/build-and-aot-invariants.md) | -fsycl link args, compile-time AOT + zebin/fatbin check, Windows MSVC device-link anchor, and immediate queue. |
| `common.cpp`, `/core/src/meson.build` | [cleanup-graph-and-build](AGENTS.d/cleanup-graph-and-build.md) | Goto-free shared-frame cleanup, sticky graph_wait error handling, graph_register order, ADRs, and SYCL builds. |
| `common.cpp`, `common.h`, `picture_sycl.cpp`, `dmabuf_import.cpp`, `/core/src/feature/sycl/sycl_compat.h` | [scope-runtime-and-subgroups](AGENTS.d/scope-runtime-and-subgroups.md) | SYCL runtime scope, sycl_compat.h macros, VmafSyclState aggregate init, duped dma-buf fds, and 16/32 subgroups. |
| `common.cpp`, `dmabuf_import.cpp`, `dispatch_strategy.cpp`, `/core/src/libvmaf.c` | [shared-planes-and-va-import](AGENTS.d/shared-planes-and-va-import.md) | Shared luma/chroma plane uploads, synchronous upload_plane, zero-copy luma-only gate, and P010 VA normalization. |
| `/core/src/meson.build`, `scratch_check.cpp`, `dmabuf_import.cpp`, `/core/src/feature/sycl/*.cpp` | [strict-fp-fp64-and-scratch](AGENTS.d/strict-fp-fp64-and-scratch.md) | sycl_strict_fp_args compile/link flags, fp64-free parallel_for lambdas, zero-scratch audit, and VA surface ABI. |
| `import_*.c`, `vmafx_sycl*`, `detile.h`, `queue_props.h` | [vmafx-device-frames](AGENTS.d/vmafx-device-frames.md) | VMAFx SYCL frame import, immediate library queue, join() fence ordering, shared detile.h, and semaphore dup. |
