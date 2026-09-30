<!-- markdownlint-disable MD013 MD060 -->
# Research-2131: who frees the `vmaf` CLI's option dictionaries

- **Status**: Active
- **Workstream**: `T-CLI-PRE-REGISTRATION-OPTS-DICT-LEAK-2026-09-30` (bug fix, no ADR)
- **Last updated**: 2026-09-30

## Question

`vmaf --feature cambi=full_ref=true` on an input that cannot be opened exits
with a LeakSanitizer report of 158 bytes. Which exit paths leak the option
dictionaries the CLI builds, and where can the CLI release them without
freeing one that libvmaf already took?

## Sources

- `core/tools/cli_parse.cpp`: `parse_feature_config()` and `apply_model_opt()`
  build the dictionaries with `vmaf_feature_dictionary_set()`; `cli_free()`.
- `core/tools/vmaf.cpp`: `run_cli()`, `register_cli_feature()`,
  `load_one_model_entry()`, `overload_model_collection_features()`,
  `cleanup_cli_run_state()`.
- `core/include/libvmaf/libvmaf.h` (`vmaf_use_feature()`) and
  `core/include/libvmaf/model.h` (`vmaf_model_feature_overload()`,
  `vmaf_model_collection_feature_overload()`): the ownership contract
  (Netflix/vmaf#1242).
- `core/src/libvmaf.c` `vmaf_use_feature()`, `core/src/feature/feature_extractor.cpp`
  `vmaf_feature_extractor_context_create()`, `core/src/opt.cpp` `vmaf_option_set()`.
- PR #1642 review findings (the reproducer and the pre-registration scope).

## Findings

- The three libvmaf calls take the dictionary on every path except their
  argument guards. `cli_free()` freed only `feature_cfg[i].buf` and
  `model_config[i].buf`, so a dictionary stayed allocated whenever the run
  did not reach its call: `open_cli_inputs()` failures (missing file, odd
  height with 4:2:0), `load_cli_models()` failures (a model feature that does
  not fit the frame, before the overloads are applied), and in
  `register_cli_features()` every feature after the first failure, and the
  ADR-0498 refusal of a feature pinned to a backend the run did not start
  (`--backend cpu --feature psnr_cuda=...`), which returns before the
  hand-off.
- Measured on master `10f27efe2`, clang 22 ASan build, 64x64 inputs: missing
  input with `--feature psnr=enable_chroma=true` and a
  `--model path=...:vif.vif_enhn_gain_limit=1.0` overload 329 bytes / 8
  allocations; odd height 329; `vmaf_v1.0.16_3d0h` on 64x64 329; failing
  `--feature cambi` before `--feature psnr=enable_chroma=true` 163; unknown
  extractor with options 158; `--backend cpu --feature
  psnr_cuda=enable_chroma=false` 164, where LeakSanitizer also turns the
  intended exit 100 into 1. `--feature psnr=no_such_option=1` and a complete
  run are clean.
- Exit codes do not change. Without a sanitizer, master and the fix exit
  alike on all eight paths the regression test drives (release builds, no
  LTO): 255 for a missing input, an odd height, an unknown extractor and an
  unknown option, 234 (`-EINVAL`) for a model or feature that does not fit,
  100 for the refused pinned backend, 0 for a complete run.
- The overload dictionary's leak is invisible to the default LeakSanitizer
  scan: a stale pointer in the exit-time stack frames keeps it reachable.
  `LSAN_OPTIONS=use_stacks=0:use_registers=0` reports it; the regression test
  sets that, which is safe on these paths because no worker thread is alive
  at exit.
- `vmaf_use_feature()` returns `-EINVAL` in two cases the caller cannot tell
  apart by the code: an unknown extractor name (argument guard, dictionary
  handed back) and an option the extractor rejects
  (`vmaf_fex_ctx_parse_options()` inside context creation, dictionary already
  freed). With a NULL dictionary every option setter returns 0, so
  `vmaf_use_feature(vmaf, name, NULL)` fails with `-EINVAL` only for an
  unknown name. The CLI runs that second call only after the first failed
  with `-EINVAL`; for a known name it registers one more extractor, with
  default options, in a context the run is about to close, which
  `vmaf_close()` releases. The probe stays sound only while a registration
  failure ends the run. A `vmaf_feature_backend_twin()` pre-check cannot
  replace it: that call returns `-EINVAL` both for an unknown name and for a
  registered GPU extractor such as `psnr_cuda`.

## Alternatives explored

| Option | Result | Why not chosen |
|---|---|---|
| Free the dictionaries on each `run_cli()` error return | Needs a free on every early return and knowledge of which ones libvmaf already took | Ownership spread over many returns; the next early return would leak again |
| `cli_free()` frees what is left, hand-offs clear the pointer (chosen) | One release point; a hand-off that forgets to clear double-frees in every normal run, so tests and ASan catch it at once | — |
| Leave the unknown-name case alone | Simpler `register_cli_feature()` | One dictionary still leaks on a typo in `--feature name=...` |
| Add a public "is this extractor registered" query | Exact without a second call | New public API and exported symbol for a CLI error path |

## Open questions

- `cli_parse()`'s own `usage()` exits (a bad option string in the middle of a
  `--model` value, more than 32 overloads) still leave the half-built
  dictionaries to the process exit. The shipped CLI exits with the stack
  live, so LeakSanitizer does not report them; only the fuzz harness, which
  longjmps out of `exit()`, can lose them, and it runs with leak detection
  off for that reason.

## Related

- `docs/state.md` `T-CLI-PRE-REGISTRATION-OPTS-DICT-LEAK-2026-09-30`,
  `T-CLI-PARSE-ERROR-PATH-LEAK-2026-09-16` (the parse-time paths, #1425).
- `core/tools/test/test_vmaf_option_dict_ownership.sh`,
  `core/test/test_cli_parse.c` `release_parsed()`.
