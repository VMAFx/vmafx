<!-- GENERATED (`make docs-fragments-write`); do not hand-edit. -->
# Engine lifecycle, models, Rust, and MCP: `core/` agent invariants

core/include/libvmaf/*, core/src/libvmaf.c, model.c, predict.c, picture*,
thread_pool*, fex_ctx_vector*, core/src/feature/*.c, core/src/mcp/*,
core/src/rust/**, core/api/vmafx.toml, core/tools/cli_parse.cpp, and
core/tools/vmaf.cpp.

Parent index: [AGENTS.md](AGENTS.md).

Generated index. Match touched paths against `Touching`; read each matching
page first (none -> skip). Text above table binds every file. `Touching`
relative; leading `/` = repo root. New invariant -> `AGENTS.d/` page
([how](../docs/development/agents-index.md)).

## Topic pages

| Touching | Read first | Invariant |
| --- | --- | --- |
| `src/libvmaf.c`, `include/libvmaf/libvmaf.h` | [context-lifecycle-and-dispatch](AGENTS.d/context-lifecycle-and-dispatch.md) | vmaf_init never reads output handle; subsystems init owns framesync lifecycle; output writers test ferror. |
| `src/fex_ctx_vector.cpp`, `src/fex_ctx_vector.h` | [feature-context-and-registration](AGENTS.d/feature-context-and-registration.md) | Feature context registration compares option keys for deduplication and manages option-copy lifecycle. |
| `src/mcp/mcp.c`, `src/mcp/mcp_internal.h` | [mcp-runtime](AGENTS.d/mcp-runtime.md) | Embedded MCP runtime conforms to ADR-0209 transport contract and lifecycle. |
| `include/libvmaf/model.h`, `src/model.c`, `src/predict.c` | [model-loading-and-defaults](AGENTS.d/model-loading-and-defaults.md) | Default model version defined solely by VMAF_DEFAULT_MODEL_VERSION; model loading scales without ceiling. |
| `src/picture.c`, `src/picture_pool.c`, `src/libvmaf.c` | [picture-pool-and-ownership](AGENTS.d/picture-pool-and-ownership.md) | Picture pool fetch errors signal availability; vmaf_read_pictures centralizes picture ownership release. |
| `src/rust/**`, `api/vmafx.toml`, `src/feature/*.c`, `src/libvmaf.c` | [rust-core-migration](AGENTS.d/rust-core-migration.md) | New host-side work lands in Rust unless the ADR-2478 phase table says that layer is still C. |
| `src/picture_sample_range.c`, `src/picture_sample_range.h`, `src/libvmaf.c`, `include/libvmaf/libvmaf.h`, `tools/cli_parse.cpp`, `tools/vmaf.cpp` | [sample-range-check](AGENTS.d/sample-range-check.md) | Samples above 2^bpc - 1 are invalid input; only opt-in read_pictures check scans for them. |
| `src/thread_pool.c`, `src/thread_pool.h` | [thread-pool-and-jobs](AGENTS.d/thread-pool-and-jobs.md) | Thread pool enforces bounded admission, job recycling, and inline buffer reuse. |
