<!-- markdownlint-disable MD013 MD060 -->
# ADR-1176: Metal motion_v2 mirror closeout and reflect-101 parity contract

- **Status**: Accepted
- **Date**: 2026-09-04
- **Deciders**: Lusoris
- **Tags**: `metal`, `gpu`, `motion-v2`, `parity`, `boundary`, `closeout`, `fork-local`

## Context

[ADR-1166](1166-upstream-issue-harvest.md) recorded the upstream triage findings from
Netflix/vmaf#1580 and noted that the Metal `integer_motion_v2` mirror kernel used
`2 * sup - idx - 1` at the high boundary rather than reflect-101 (`2 * (sup - 1) - idx`).
At triage time, ADR-1166 recorded this item as deferred under the premise that fixing
it would move Metal scores and required dedicated Apple Silicon GPU verification and
snapshot regeneration.

PR #1223 (commit `71da046db`) subsequently implemented the reflect-101 fold in
`core/src/feature/metal/integer_motion_v2.metal:74-80`:

```metal
inline int mv2_mirror(int idx, int sup)
{
    if (sup <= 1) return 0;
    while (idx < 0 || idx >= sup)
        idx = (idx < 0) ? -idx : 2 * (sup - 1) - idx;
    return idx;
}
```

This implementation iterated the bounce to handle out-of-bounds indices and corrected
the boundary formula to `2 * (sup - 1) - idx`, matching:

- CPU `core/src/feature/integer_motion_v2.c:157` (`mirror`)
- CUDA `core/src/feature/cuda/integer_motion_v2/motion_v2_score.cu:51` (`mv2_mirror`)
- SYCL `core/src/feature/sycl/integer_motion_v2_sycl.cpp:109` (`dev_mirror_mv2`)
- HIP `core/src/feature/hip/integer_motion_v2/motion_v2_score.hip:67` (`mv2_mirror`, [ADR-1106](1106-hip-motion-v2-mirror-reflect101-correction.md))

However, several closeout conditions remained unmet on `master`:

1. The kernel header comment in `core/src/feature/metal/integer_motion_v2.metal:20-23`
   contradicted the code by continuing to claim `2 * size - idx - 1` padding.
2. `docs/state.md` and ADR-1166 still described the Metal fix as open/deferred.
3. No Metal score snapshots exist in `testdata/` (`git ls-tree origin/master testdata | grep -i metal`
   returns 0), so no snapshot regeneration (`/regen-snapshots`) was actually required.
4. In `core/test/test_metal_motion_v2_parity.c`, non-Apple hosts returning `-ENODEV` from
   `vmaf_metal_state_init` emitted `[skip: no Metal device]` to stderr but exited with 0.
   Because Meson hides passing stderr in standard test runs, CI logs could not prove whether
   the test executed on live hardware or skipped.

## Decision

1. **Close out the Metal motion_v2 mirror fix**: Formally record that the reflect-101
   kernel fix landed in PR #1223 (`71da046db`). This ADR supersedes in part the deferral
   recorded in ADR-1166 §Neutral / follow-ups.
2. **Align documentation with implementation**: Correct the header comment in
   `core/src/feature/metal/integer_motion_v2.metal` to document the iterated reflect-101
   fold (`2 * (sup - 1) - idx`). Record the invariant in `core/src/feature/metal/AGENTS.md`.
3. **No snapshot regeneration needed**: Confirm that no fork-added Metal reference
   snapshots exist under `testdata/`. CPU golden assertions remain unaffected.
4. **Make test skip observable**: In `core/test/test_metal_motion_v2_parity.c`, set
   `mu_skipped = 1` on the `-ENODEV` branch so `test.c` exits 77 (Meson's standard skip code),
   log explicit device confirmation to stdout when hardware is active, and configure the test in
   `core/test/meson.build` with `should_fail : false`, `protocol : 'exitcode'`, and `verbose : true`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Leave ADR-1166 deferral open | Zero documentation work | Leaves `state.md` and ADR-1166 out of sync with actual code in tree | Rejected: code was already merged in #1223; state tracking must reflect reality. |
| Regenerate testdata snapshots | Follows standard GPU change playbook | No Metal snapshots exist in `testdata/` | Rejected: `testdata/` only contains CPU and select CUDA/SYCL snapshots. |
| Keep exit 0 on `-ENODEV` skip | Matches legacy Metal parity tests | Indistinguishable from real device pass in CI logs | Rejected: exits 77 with verbose stdout gives clear observability of real hardware runs. |

## Consequences

- **Positive**: Code, comments, ADRs, and `docs/state.md` are unified; cross-backend parity
  contract is documented; CI logs unambiguously distinguish device execution from skip.
- **Negative**: None. Kernel code is unchanged from PR #1223; CPU golden data is untouched.
- **Neutral / follow-ups**: Other `test_metal_*_parity.c` tests may adopt the observable
  `mu_skipped = 1` exit 77 pattern in future maintenance sweeps.

## References

- PR #1223 (commit `71da046db`) — upstream issue harvest batch implementation
- [ADR-1166](1166-upstream-issue-harvest.md) — upstream issue harvest triage (superseded in part)
- [ADR-1106](1106-hip-motion-v2-mirror-reflect101-correction.md) — HIP motion_v2 mirror correction
- [ADR-0421](0421-metal-first-kernel-motion-v2.md) — Metal motion_v2 initial kernel specification
- [ADR-0214](0214-gpu-parity-ci-gate.md) — cross-backend parity gate (places=4)
- `core/src/feature/metal/integer_motion_v2.metal`
- `core/test/test_metal_motion_v2_parity.c`
- Source: req
