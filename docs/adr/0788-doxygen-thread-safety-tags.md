# ADR-0788: Doxygen doc-comment and @thread-safety tags on public C-API

| Field    | Value                             |
|----------|-----------------------------------|
| Status   | Accepted                          |
| Date     | 2026-05-29                        |
| Tags     | api, docs, thread-safety, libvmaf |

## Context

A sweep of `core/include/libvmaf/*.h` (PR #115 thread-safety audit follow-up)
found that several public functions were missing Doxygen `@brief`, `@param`, and
`@return` tags. In addition, no function carried a `@thread-safety` annotation,
which forced readers to hunt through implementation code to determine whether
concurrent use was safe. The rule adopted here is: one VmafContext per thread —
a VmafContext is not thread-safe because the internal feature-extractor pipeline
shares per-context state (scheduler queue, score cache, picture pool) that is
not protected by fine-grained locking. Callers that want parallel scoring
create
independent VmafContexts.

The gaps existed in:

- `picture.h` — `vmaf_picture_alloc`, `vmaf_picture_unref` (no Doxygen at
  all)
- `feature.h` — `vmaf_feature_dictionary_set`,
  `vmaf_feature_dictionary_free` (no Doxygen)
- `model.h` — all eight model/collection load/destroy/overload functions
  (no Doxygen)
- `libvmaf.h` — `vmaf_version` (thin comment, no `@brief`/`@return`); all
  other functions had Doxygen but no `@thread-safety` tag
- `dnn.h` — `vmaf_dnn_session_close` (no Doxygen block)

The GPU-backend headers (`libvmaf_cuda.h`, `libvmaf_sycl.h`, `libvmaf_hip.h`,
`libvmaf_vulkan.h`, `libvmaf_metal.h`, `libvmaf_mcp.h`) were already adequately
documented and required no changes.

## Decision

Add Doxygen `@brief`, `@param`, `@return`, and `@thread-safety` blocks to every
public function that was missing any of these. The `@thread-safety` wording is
standardised as:

> Not thread-safe. Use one VmafContext per thread.

The sole exception is `vmaf_version()`, which reads a compile-time constant and
is safe to call from any thread.

## Alternatives considered

- **No alternatives:** this is a documentation-completeness fix with a single
  correct outcome. No architectural trade-off exists.

## References

- req: "Sweep `core/include/libvmaf/*.h` for missing Doxygen comments on
  public functions. Per PR #115 thread-safety audit: also add
  `@thread-safety` tags noting 'not thread-safe; one VmafContext per
  thread' per ADR-0777 follow-up #1."
