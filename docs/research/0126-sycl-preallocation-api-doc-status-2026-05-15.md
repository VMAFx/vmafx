# Research 0126 — SYCL preallocation API doc status

## Question

`docs/api/gpu.md` still warned operators that
`vmaf_sycl_preallocate_pictures()` was a no-op stub. Is that warning still
accurate on current master?

## Findings

- `core/src/libvmaf.c::vmaf_sycl_preallocate_pictures()` rejects missing
  contexts / missing SYCL state, rejects duplicate pools with `-EBUSY`, and
  creates a 2-deep SYCL picture pool for `DEVICE` and `HOST`.
- `core/src/libvmaf.c::vmaf_sycl_picture_fetch()` fetches from that pool
  when configured, and only falls back to `vmaf_picture_alloc()` when no pool
  exists.
- `core/test/test_sycl_pic_preallocation.c` covers `NONE`, `DEVICE`,
  `HOST`, duplicate-preallocation rejection, and invalid enum rejection.
- `docs/backends/sycl/overview.md` already describes the real pool semantics;
  only the generic GPU API page was stale.

## Alternatives Considered

| Option | Decision | Rationale |
| ------ | -------- | --------- |
| Leave the warning until a fresh SYCL hardware run | Rejected | The warning contradicted shipped code and a dedicated unit test; keeping it misleads API users. |
| Remove the whole simple-path section | Rejected | The API remains useful for callers that prefer whole-picture ownership over the frame-buffer API. |
| Replace the warning with the real enum semantics | Accepted | Aligns the generic API page with implementation, backend docs, and tests without changing code. |

## Decision

Update `docs/api/gpu.md` to describe the live SYCL picture-preallocation
pool and keep the frame-buffer API positioned as the alternative path for
callers that want the backend-managed Y-plane double buffer.

## Validation

Documentation-only change. The code claim is grounded in
`core/src/libvmaf.c` and the existing `test_sycl_pic_preallocation`
coverage.
