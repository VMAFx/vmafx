# ADR-0846: C++23 Wave 8 — convert `read_json_model.c` to `.cpp`

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: `cpp23`, `build`, `core`, `refactor`, `fork-local`

## Context

The fork is incrementally migrating libvmaf C translation units to C++23
(Wave series, ADR-0708 established the pattern). `read_json_model.c` is the
JSON model-parsing layer — responsible for deserialising every `.json` model
file into `VmafModel` structs. It is exercised by every VMAF scoring run.

The original C file had one structural issue worth addressing:
`vmaf_read_json_model` used a `goto fail:` cleanup label to ensure
`vmaf_model_destroy` was called and `*model` was nulled on any parse-error
exit path. The `goto` pattern is correct but makes control flow non-obvious
and requires manual maintenance as new early-exit paths are added. In
addition, `model_collection_parse` used two naked `malloc`/`free` pairs for
`cfg_name`, requiring explicit `free` on every exit path.

## Decision

Convert `core/src/read_json_model.c` to `read_json_model.cpp` compiled under
`cpp_std=c++23` as an isolated static library (`read_json_model_cpp23_lib`)
following the ADR-0708 / ADR-0755 precedent. The four public entry points
(`vmaf_read_json_model_from_buffer`, `vmaf_read_json_model_from_path`,
`vmaf_read_json_model_collection_from_buffer`,
`vmaf_read_json_model_collection_from_path`) retain their original C signatures
and are declared `extern "C"` in the implementation, so all existing C callers
link without modification.

C++23 improvements applied:

- `goto fail:` / manual destroy in `vmaf_read_json_model` replaced with a
  scoped `ModelParseGuard` RAII class — destructor calls `vmaf_model_destroy`
  + nulls `*model`; `release()` transfers ownership on success.
- `malloc(cfg_name_sz)` + two `free(cfg_name)` calls in
  `model_collection_parse` replaced with `std::make_unique<char[]>`.
- `strdup(key)` + `free(key)` in `parse_feature_opts_object` replaced with
  `std::string key{...}` — RAII frees the copy on every path.
- `(char *)malloc(...)` C-style casts replaced with `static_cast<>`.
- `NULL` replaced with `nullptr` throughout.
- `[[nodiscard]]` applied to `grow_count` and `ensure_*_capacity`.
- `uint64_t cfg->flags` → `enum VmafModelFlags` explicit cast at the one
  internal call site where C silently converted the integer.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep `.c`, add guard macros | No build-system change | Doesn't fix the goto; still C; diverges from Wave direction | Wave strategy mandates `.cpp` |
| Inline into `model.cpp` | Fewer TUs | Mixes concerns; `read_json_model` is independently testable | Separation is load-bearing for test isolation |
| Full C++23 `std::expected` error path | Idiomatic modern C++ | ABI-incompatible return types; cannot expose as `extern "C"` | Public C ABI must be preserved |

## Consequences

- **Positive**: `goto`-free teardown; `unique_ptr` eliminates naked-free
  double-free risk on future edits; `[[nodiscard]]` catches dropped error
  codes at compile time.
- **Negative**: One more isolated static lib in meson.build (minimal
  maintenance cost; mirrors existing pattern).
- **Neutral / follow-ups**: `read_json_model.c` remains on disk but is no
  longer compiled (comment in meson.build notes this). It can be deleted
  in a follow-on cleanup PR once the `.cpp` is stable.

## References

- ADR-0708: first C++20 pilot TU (`metadata_handler.cpp`).
- ADR-0755: Wave 7 (`cpu.cpp`).
- ADR-0729: Wave 3 (`model.cpp`).
- Wave 8 PR: `feat/cpp23-read-json-model-20260529`.
- Instruction: paraphrased user direction — "convert `read_json_model.c` to
  `.cpp`, tight scope, apply C++23 idioms including RAII over goto".
