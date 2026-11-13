<!-- GENERATED (`make docs-fragments-write`); do not hand-edit. -->
# Engine, pictures, framesync, and I/O: `core/src/` agent invariants

libvmaf.c, picture*, gpu_picture_pool.cpp, framesync*, conversion_*,
read_json_model.c, read_json_model.cpp, log*, output.cpp, thread_locale*,
opt.cpp, dict.cpp, metadata_handler*, pdjson*, feature/feature_extractor.cpp,
and feature/feature_name.cpp.

Parent index: [AGENTS.md](AGENTS.md).

Generated index. Match touched paths against `Touching`; read each matching
page first (none -> skip). Text above table binds every file. `Touching`
relative; leading `/` = repo root. New invariant -> `AGENTS.d/` page
([how](../../docs/development/agents-index.md)).

## Topic pages

| Touching | Read first | Invariant |
| --- | --- | --- |
| `conversion_context.c`, `conversion_context.h`, `conversion_policy.c`, `conversion_policy.h`, `read_json_model.c`, `read_json_model.cpp` | [conversion-target](AGENTS.d/conversion-target.md) | Convert to model's conversion_target on host; input colour lives on context. |
| `picture.h`, `picture_pool.h` | [doxygen-header-invariants](AGENTS.d/doxygen-header-invariants.md) | Internal core/src headers carry Doxygen file briefs and parameter comments. |
| `framesync.c`, `framesync.h` | [framesync-producer](AGENTS.d/framesync-producer.md) | Framesync buffer error paths invoke vmaf_framesync_abort to prevent consumer cond_wait hang. |
| `picture.c`, `picture_pool.c`, `picture_pool.cpp`, `gpu_picture_pool.cpp` | [init-and-teardown](AGENTS.d/init-and-teardown.md) | Out-parameter init clears handles on failure; teardown owners replace partial cleanup ladders. |
| `log.c`, `log.h`, `log.cpp` | [logging-and-diagnostics](AGENTS.d/logging-and-diagnostics.md) | Fork diagnostics route through vmaf_log with BUG-048 format lock; log.c respects C23 va_start. |
| `pdjson.c`, `pdjson.h`, `framesync.c` | [memory-and-concurrency-safety](AGENTS.d/memory-and-concurrency-safety.md) | pdjson enforces 512 container limit; pthread init, malloc, and size bounds checked without leak. |
| `metadata_handler.cpp`, `metadata_handler.h` | [metadata-handler](AGENTS.d/metadata-handler.md) | C++20 metadata handler preserves extern "C" symbols and unique_ptr lifecycle. |
| `output.cpp`, `thread_locale.cpp`, `thread_locale.h`, `opt.cpp`, `dict.cpp`, `feature/feature_name.cpp` | [output-writers-and-locale](AGENTS.d/output-writers-and-locale.md) | Score capacity checks use >=; JSON writers guard delimiters; locale pushes flush before pop. |
| `libvmaf.c`, `feature/feature_extractor.cpp` | [picture-ownership-and-dispatch](AGENTS.d/picture-ownership-and-dispatch.md) | PREV_REF counted refs, n-2 kept only for reader; vmaf_read_pictures owns both pictures; advance per frame. |
