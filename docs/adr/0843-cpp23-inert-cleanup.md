# ADR-0843: Drop inert cpp23 shadow files where meson uses the `.c` original

| Field       | Value                              |
|-------------|-----------------------------------|
| Status      | Accepted                          |
| Date        | 2026-05-29                        |
| Deciders    | lusoris                           |
| Replaces    | —                                 |
| Superseded  | —                                 |

## Context

The cpp23 migration effort (Waves 1–5, ADR-0708/0720/0721/0723/0725/0727/0729/0731/0733/0735)
created `.cpp` sister files alongside each `.c` original with the intent that the meson
build would be switched over in a later wave. Auditing the current `core/src/meson.build`
(PR #100 finding) reveals that **Waves 1–5 are entirely inert**: meson still references
the `.c` originals for every one of these pairs. The `.cpp` files are compiled by no target
and produce dead code that misleads future contributors into thinking the migration is wired.

Wave 7 (`libvmaf_cpu_static_lib`, `cpu.cpp`, `cpp_std=c++23`) is fully wired — meson uses
`cpu.cpp` so the inert file is the companion `cpu.c` original.

Wave 1 (`metadata_handler_cpp20_lib`, `metadata_handler.cpp`) is fully wired — meson uses
`.cpp`.

Waves 8 and 9 (ADR-0108 brief: `opt_cpp23_lib`, `read_json_model_cpp23_lib`,
`picture_pool_cpp23_lib`, etc.) were previously cited as "in-flight at PR #120 / #124" but
both those PRs have since merged and neither wired any cpp23 target; the description was
stale. `opt.cpp` exists but meson uses `opt.c`; `read_json_model.cpp`, `picture_pool.cpp`,
`gpu_picture_pool.cpp`, and `gpu_dispatch_env.cpp` do not exist yet.

**Inert files removed by this ADR:**

| File                              | Wave       |
|-----------------------------------|------------|
| `core/src/cpu.c`                  | Wave 7 (superseded by cpu.cpp) |
| `core/src/dict.cpp`               | Wave 2 (ADR-0727) |
| `core/src/fex_ctx_vector.cpp`     | Wave 2 (ADR-0723) |
| `core/src/log.cpp`                | Wave 1 (ADR-0725) |
| `core/src/mem.cpp`                | Wave 1 (ADR-0720) |
| `core/src/model.cpp`              | Wave 3 (ADR-0729) |
| `core/src/opt.cpp`                | Wave 8 (ADR-0721, unwired) |
| `core/src/output.cpp`             | Wave 4 (ADR-0733) |
| `core/src/ref.cpp`                | Wave 5 (ADR-0735) |
| `core/src/thread_locale.cpp`      | Wave 5 (ADR-0735) |
| `core/src/feature/feature_name.cpp`   | Wave 3 (ADR-0729) |
| `core/src/feature/luminance_tools.cpp`| Wave 3 (ADR-0731) |
| `core/src/feature/mkdirp.cpp`         | Wave 3 (ADR-0731) |
| `core/src/feature/picture_copy.cpp`   | Wave 3 (ADR-0729) |
| `core/src/feature/psnr_tools.cpp`     | Wave 3 (ADR-0731) |

## Decision

Delete all 15 inert files. The `.c` originals remain the live meson targets. When a future
wave wires the cpp23 versions, the shadow `.c` (or the migration note in the `.cpp`) will
document the historical C source.

## Alternatives considered

**Keep them as documentation.** The files contain C++23 rewrites with migration notes. However,
having uncompiled source files with migration notes that are out of date with the real meson
wiring is more confusing than helpful; the wave ADRs (ADR-0720 etc.) document the intent.

**Wire them now instead of deleting.** Each wave's wire-up is a non-trivial meson change that
requires careful override-options isolation per TU. This ADR closes the dead-code audit;
future per-wave PRs do the wiring.

## References

- req: Audit cpp23 Wave 1-9 meson wire-up status per PR #100 finding; open PR if cleanups apply.
- ADR-0708: metadata_handler C++20 pilot
- ADR-0720/0721/0723/0725/0727/0729/0731/0733/0735: individual Wave ADRs
- ADR-0386/0535/0628: ADR numbering policy
