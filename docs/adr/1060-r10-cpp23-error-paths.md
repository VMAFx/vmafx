<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1060: Round 10 C++23 wave error-path cleanup

- **Status**: Accepted
- **Date**: 2026-06-06
- **Deciders**: Lusoris
- **Tags**: `cpp23`, `correctness`, `memory`, `error-handling`, `fork-local`

## Context

The C++23 wave files (`read_json_model.cpp`, `opt.cpp`, `log.cpp`,
`gpu_dispatch_env.cpp`, `feature_extractor.cpp`) introduced new code paths
that require careful error handling. A round-10 audit identified five
concrete error-path defects:

1. **`vmaf_feature_extractor_context_create` (feature_extractor.cpp:491)**:
   When `vmaf_fex_ctx_parse_options` fails, the function returned the error
   code without freeing `f->fex->priv`, `f->fex` (x), and `f`. Three heap
   allocations leaked on every failed option-parse.

2. **`pthread_mutex_init` return unchecked (feature_extractor.cpp:736)**:
   `vmaf_fex_ctx_pool_create` called `pthread_mutex_init` but discarded its
   return value. On failure (e.g. `ENOMEM` on low-memory systems) the pool
   would be returned as if successfully initialised. The existing `free_p`
   label also did not free `p->fex_list` when reached from the mutex path.

3. **`pthread_cond_init` unchecked + `vmaf_dictionary_copy` unchecked
   (feature_extractor.cpp:791, 797)**:
   `get_fex_list_entry` discarded both return values. A cond-init or
   dict-copy failure left a partially-constructed slot whose resources
   (cond object, ctx_list allocation) were neither cleaned up nor recorded.

4. **`vmaf_fex_ctx_pool_flush` discards flush errors
   (feature_extractor.cpp:933)**:
   The inner `vmaf_feature_extractor_context_flush` call result was silently
   discarded. Callers received 0 even when temporal-extractor flush failed.

5. **`model_parse` does not check `json_get_error` after the key loop
   (read_json_model.cpp:543)**:
   If the JSON stream entered an error state while `json_skip`-ing unknown
   keys *after* a successful `model_dict` parse, `model_parse` returned 0
   (success). Callers would then proceed with a partially-parsed model.

`opt.cpp`, `log.cpp`, and `gpu_dispatch_env.cpp` are clean — no actionable
error-path gaps found.

## Decision

Fix all five defects in-place with minimal, targeted changes:

- Add `free(f->fex->priv); goto free_x;` in the context-create parse-options
  error path.
- Check `pthread_mutex_init` return; add `free_fex_list` label to free
  `p->fex_list` before `free_p`.
- Check `pthread_cond_init` return; roll back `slot->ctx_list` and destroy
  the cond on `vmaf_dictionary_copy` failure.
- Propagate the first non-zero error from `vmaf_feature_extractor_context_flush`
  in `vmaf_fex_ctx_pool_flush`.
- Check `json_get_error(s)` after the `model_parse` key loop and return
  `-EINVAL` when the stream is in an error state.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| RAII wrappers for the pool slot | Would prevent entire class of partial-init leaks | Requires broader C++23 refactor of pool struct | Deferred to a future wave; this PR targets minimal, safe fixes |
| `(void)`-cast discards for pthread | Matches pre-existing pattern for some C functions | Hides real failures on resource-constrained hosts | Not acceptable per SEI CERT ERR33-C |
| `std::expected` for `model_parse` | Type-safe, composable | Requires touching many more call sites | Out of scope for a bug-fix PR |

## Consequences

- **Positive**: No more heap leak on failed option parse. Pool creation fails
  cleanly on pthread failures. Flush errors surface to callers. Malformed JSON
  files after `model_dict` are rejected.
- **Negative**: `vmaf_fex_ctx_pool_flush` now returns non-zero on flush
  failure — callers that unconditionally ignored the return value now see an
  error. In practice all callers already check flush error codes.
- **Neutral / follow-ups**: Remaining POSIX pthread API call sites elsewhere
  in the C sources (`*.c`) are out of scope for this C++-wave PR.

## References

- Round-10 agent brief: r10-error-paths cluster.
- ADR-0772: C++23 atomic + placement-new policy in feature_extractor.cpp.
- SEI CERT ERR33-C: Detect and handle standard-library errors.
- Power of 10 Rule 7: Check return value of every non-void function.
