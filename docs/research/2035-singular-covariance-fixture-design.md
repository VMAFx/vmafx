<!-- markdownlint-disable MD013 -->

# 2035 — Designing a fixture that can actually observe a numerical-degeneracy bug

**Date**: 2026-09-07
**Scope**: the SpEED singular-covariance path on the CUDA, SYCL and HIP twins.
**Outcome**: one first-order scoring bug found and fixed in the three
`speed_temporal` twins, and one undefined-behaviour read fixed in all six
([ADR-1218](../adr/1218-gpu-speed-singular-device-solution.md)). Three of the
four fixtures tried could not see either.

## The two defects

SpEED's 25x25 covariance matrix is "regular" only when every eigenvalue is at
least `1e-6`. The CPU zeroes the solution on a singular plane, reports the
singularity separately from the return code, and — when exactly one of the
reference and distorted sides is singular — returns `0` rather than the inflated
score a one-sided zero solution produces.

1. **Wrong buffer.** All six GPU twins zeroed the *host* staging buffer and
   uploaded nothing, so the score kernel read the *device* solution left over
   from the previous frame, or whatever the allocator returned on the first.
2. **Missing signal.** The three `speed_temporal` twins never reported
   singularity at all — [ADR-1202](../adr/1202-cuda-speed-chroma-4k-launch-bounds.md)
   fixed that for the chroma twins only — so the one-sided rule could not exist
   and the twin returned the kernel's score.

## Four fixtures, one of which works

The interesting part is how many plausible fixtures are blind to this.

### 1. Flat chroma plane at 128 — blind

The obvious "completely flat channel" from the CPU's own comment. It fails for a
reason that has nothing to do with the covariance: SpEED subtracts 128 in
`picture_copy`, so the plane becomes identically zero, and the independent term
is just the sample value. The score kernel's `sum(sol * indterm) / 25` is then
zero **whatever `sol` holds**. A stale solution cannot move the number.

### 2. Flat plane at a non-neutral level — still blind

Fixes the independent term, but the covariance is now the zero matrix, so every
eigenvalue is 0 and `update_entropy` reduces to a constant:

```c
entropy[b] += log2f(L * S[b] + sigma_nn) + log2f(2*pi*e);   /* L == 0 */
```

Every block's entropy becomes `25 * (log2(sigma_nn) + log2(2*pi*e))`, which is
below `base_entropy` for any `nn_floor > 0`, and `get_speed_score()` takes its
"no visible difference" branch and returns exactly `0`. Measured: `cpu=0.0`,
`gpu=0.0`.

### 3. Column-constant plane, both sides singular — still blind

Now the covariance has five large eigenvalues and twenty zeros, so the entropies
clear `base_entropy` and the variances *do* reach the score. But the CPU's own
singular handling zeroes its solution, so its variances are `0`,
`log2f(1 + 0) == 0`, and the per-block contribution is `0` again. The GPU would
have to hold a *non-zero* stale solution to differ — and on CUDA the allocator
happened to return zeroed pages. Measured: `cpu=0.0`, `gpu=0.0`. The bug is
real; this fixture cannot see it.

### 4. Exactly one side singular — works

Freeze the **reference** frames while the **distorted** frames keep moving. The
reference temporal difference is identically zero (singular); the distorted one
is textured (regular). The CPU hits its one-sided rule and returns `0`; the
unfixed twin returns the kernel's score:

```text
SpEED temporal/one-sided-singular parity FAIL: cpu=0.00000000 gpu=230.71379089
```

Identical on CUDA (RTX 4090), SYCL (Arc A380) and HIP (gfx1030), because the
divergence is in host-side scalar control flow shared in shape across the three.

## The second trap: the fixture was never regular

The existing SpEED parity fixtures are 768x432. SpEED estimates a 25x25
covariance from one 25-vector per 5x5 block, and

```text
chroma 384x216  ->  >> NUM_SCALES (16)  ->  24x13  ->  truncated 20x10  ->  4x2 = 8 blocks
```

Eight samples cannot give a 25x25 matrix full rank. `is_matrix_regular()` is
therefore false on **every frame** of every existing SpEED parity test: they run
the singular path exclusively and never exercise the regular one. That is a
second, independent reason they could not see either defect — the device
solution was never written by a solve, so there was never a stale one to read.

960x960 gives 36 chroma blocks and 144 luma blocks, enough for a full-rank
covariance on a textured frame, which is what lets a regular frame precede a
singular one.

A ramp is also not enough: `(row * 7 + col * 13) & 0xFF` spans a
low-dimensional subspace and stays rank-deficient at any block count. The new
fixtures use an xorshift-style integer hash for a generic, deterministic
texture.

## The generalisable rules

1. **A degeneracy fixture must leave the observable quantity non-degenerate.**
   Zeroing the input often zeroes the output through a second path, hiding the
   one under test. Trace what the assertion actually reads back to its inputs
   before trusting the fixture.
2. **Prefer the asymmetric case.** "Both sides degenerate" tends to collapse to
   a fixed point that every implementation agrees on. "Exactly one side
   degenerate" is where the reference's special-case rule lives, and therefore
   where a twin that lacks the rule diverges.
3. **Check that the fixture reaches the path you think it does.** Raising the
   log level to `VMAF_LOG_LEVEL_WARNING` for one run showed the singular warning
   firing on every frame of the *existing* tests — which is how the rank
   deficiency above came to light. A one-line change, and it reframed the whole
   investigation.
4. **A negative control is not optional.** Fixtures 1 through 3 all passed
   against the *unfixed* code. Without reverting the fix and re-running, three
   plausible-looking tests would have shipped as a regression gate for a bug
   they cannot detect.
