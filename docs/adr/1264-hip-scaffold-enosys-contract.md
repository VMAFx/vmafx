<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1264: The HIP scaffold posture reports `-ENOSYS`, and its tests check both sites

- **Status**: Proposed
- **Date**: 2026-09-19
- **Deciders**: Lusoris
- **Tags**: hip, testing, scaffold, gpu, fork-local

## Context

`enable_hipcc` defaults to **false**, so the ordinary `-Denable_hip=true` build compiles the
HIP host code but no device kernels. `core/meson_options.txt` states the contract for that
posture: extractors "fall back to `-ENOSYS` at `init()`", and every HIP parity test has a
skip branch that turns `-ENOSYS` into `[skip: …]` and passes.

Four tests did not. On a default HIP build (`ROCm 7.2.4`, `gfx1036`) `master` failed
`test_hip_psnr_hvs_parity`, `test_hip_psnr_hvs_parity_large`, `test_hip_float_vif_parity`
and `test_hip_speed_singular_parity` — 173 ok, 4 fail — for two distinct reasons.

**The extractor side.** `float_vif_hip.c` and `integer_psnr_hvs_hip.c` wrote their scaffold
path as:

```c
int err = vmaf_hip_kernel_submit_pre_launch(&s->lc, s->ctx, NULL, 0, 0);
if (err != 0)
    return err;
return -ENOSYS;
```

That call passes `rb == NULL`, and rejecting a NULL `rb` is the helper's *first* statement, so
it always returned `-EINVAL` and the `-ENOSYS` below was unreachable. The extractor reported
"invalid argument" where the contract promises "not implemented", so the tests' skip branch
never matched and they failed instead.

**The test side.** `speed_temporal_hip` returns `-ENOSYS` correctly, but it does so from
`extract()`, which surfaces through `vmaf_read_pictures()` — registration succeeds, because
only `extract()` knows there is no kernel. `test_hip_speed_singular_parity.c` checked
`-ENOSYS` only at the `vmaf_use_feature()` site. Its own file header claimed "the same skip
contract as `test_hip_speed_temporal_parity.c`", and that sibling has always checked both
sites; this one simply never implemented the second.

## Decision

The scaffold posture reports `-ENOSYS` and nothing else. A scaffold path does not call
kernel-submit helpers with placeholder arguments; it returns `-ENOSYS` directly. Every HIP
parity test recognises `-ENOSYS` at **both** observation points — `vmaf_use_feature()` and the
frame submit — because which one fires depends on whether the extractor gives up at
registration or at extract.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Make `vmaf_hip_kernel_submit_pre_launch` accept `rb == NULL` | Both call sites keep working unchanged | A NULL readback buffer is genuinely invalid for a real submit; loosening the guard to serve a path that wants nothing from the helper weakens it for the ~20 callers that do | Fixes the symptom by removing a correct precondition |
| Keep the call and map its error to `-ENOSYS` in the scaffold path | Smallest diff | Encodes "this call always fails and we ignore it", which is a comment explaining a no-op; the next reader cannot tell whether the call matters | The call has no effect at all — deleting it is the honest form |
| Register scaffold extractors as unavailable so `vmaf_use_feature()` fails | One observation point instead of two, so tests get simpler | Registration is where the fork advertises the extractor set; making it depend on `enable_hipcc` changes what `--feature` accepts between builds of the same version | Moves a build-configuration detail into the public feature list |
| Mark the four tests `should_fail` | No code change | Exactly the defect ADR-1211 and PR #1493 spent effort undoing: a real regression then reads as the expected failure | Hides the contract instead of honouring it |

## Consequences

- **Positive**: the default HIP build's fast suite is green (177 ok, 0 fail) and says
  `[skip: HIP scaffold ENOSYS …]` where it means it. With `enable_hipcc=true` the same four
  tests run against real kernels and pass (183 ok, 0 fail on `gfx1036`).
- **Negative**: two observation points remain, so a new HIP parity test still has to handle
  both. `core/src/feature/hip/AGENTS.md` and `docs/rebase-notes.md` say so.
- **Neutral / follow-ups**: the remaining `test_hip_adm_parity` unexpected pass and two
  expected fails are the stale `should_fail` markers PR #1493 addresses, not this change.

## References

- `core/src/feature/hip/float_vif_hip.c`, `core/src/feature/hip/integer_psnr_hvs_hip.c`.
- `core/test/test_hip_speed_singular_parity.c`, and
  `core/test/test_hip_speed_temporal_parity.c` as the pattern it now matches.
- `core/meson_options.txt` — the `enable_hipcc` description stating the `-ENOSYS` contract.
- Bug ledger `L-79` (`.workingdir/BUGS.md`).
- Source: `req` — the user's direction to work the bug ledger and fix every entry.
