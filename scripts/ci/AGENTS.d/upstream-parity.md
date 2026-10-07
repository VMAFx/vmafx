---
paths:
  - scripts/ci/upstream_parity.d/*
  - scripts/ci/upstream_parity_allowlist.py
  - scripts/ci/tests/test_upstream_parity_allowlist.py
  - scripts/dev/upstream_parity.py
  - scripts/dev/upstream_parity_matrix.py
  - scripts/dev/upstream_parity_harness.c
  - scripts/dev/tests/test_upstream_parity.py
  - scripts/docs/generate-upstream-parity-allowlist.py
  - testdata/bench_upstream_ab.py
invariant: Inherited code = Netflix bits, measured in dev image only; difference = ADR + fragment; stale = remove.
---
<!-- markdownlint-disable MD013 MD060 -->
# Upstream parity guard and its allowlist

**Rule (ADR-1487).** Code inherited from Netflix/vmaf evaluates as
recorded Netflix head does. Difference allowed only by ADR, listed with its
measured size. Unintended difference -> revert to upstream's expression
(scalar, SIMD, exact twins), not fragment with ADR.

**Guard.** `scripts/dev/upstream_parity.py` (`make upstream-parity` = probe,
`make upstream-parity-full` = full matrix). Builds Netflix/vmaf at pin
(`upstream_parity_pin.py`, same heading as Licence Provenance job) and
this tree with `setup-golden-build.sh`; one harness
(`upstream_parity_harness.c`) per tree; every collector value at `%.17g`;
dispatch `scalar` / `default` (+ `avx2` in full). Exit 0 pass, 1 parity
fails, 2 could not compare.

**Environment.** Measure in dev image only: `--container [IMAGE]` (make
targets pass it; no network, caller's uid, checkout at its own path, pin
fetched on host first). Documents record image id, compilers, libc;
documents of two environments are not compared. Host = `--unpinned`, verdict
advisory, never evidence, never moves bound (upstream `ciede` `powf(x, 2)`
differs between glibc 2.43 and 2.44). Builds + run cache per environment:
`build-upstream-parity/image-<id>/`, `host/`; never reuse across them.

**Heap check.** `--heap-check` (full target) reruns every request with
`MALLOC_PERTURB_=170`. This tree's output changes -> fail (read of
unwritten memory: fix it). Upstream output changes -> undefined: only
`bound: inf` may cover it; finite bound there fails. Never report exit 2 or run that did not happen
as pass. Not required check; no hosted job yet
(`T-UPSTREAM-PARITY-GUARD-HOSTED-JOB-2026-10-02`).

**Fails on:** difference no fragment covers; difference above its fragment's
`bound`; fragment with no attributed difference (stale) while run in its
scope ran; crash of this tree's harness. Filtered run set (`--only`,
`--dispatch`, bench) reports stale, does not fail on it.

**Fragments.** `upstream_parity.d/<extractor>.<topic>` (`model.<topic>` for
scores; needs explicit `runs`). Keys and kinds:
`upstream_parity_allowlist.py` docstring. Parser = `parse_fragment_fields()`
/ `check_fragment_adrs()` of `cross_backend_calibration.py`, shared with
`exact_twins.d`; change grammar there, both directories follow.
Attribution: deliberate before pending, then smallest bound, then name.
Table = `docs/development/upstream-parity-allowlist.md`, GENERATED
(`make docs-fragments-write`; conflict -> master's side, regenerate).

- New deliberate deviation: ADR first (upstream file + line at pin, fork
  lines, reason, size, what ends it), then fragment, same pull request.
  `bound` = measured maximum in image rounded up, never guess, never
  "a bit more"; `inf` where heap check says upstream is undefined.
  `evidence` names image it was measured in.
- `pending-revert` / `pending-port`: no ADR, names `branch`. pull
  request that lands branch deletes fragment. Stale on master =
  delete it; do not widen scope or add fixture to keep it alive.
- deliberate fragment's bound next to pending one on same metrics is
  sized on tree with revert applied; pending fragment keeps
  differences of its own and goes stale when revert lands. Re-measure
  both when either changes.
- `upstream:` optional. Add `Netflix/vmaf#<n>` when pull request or issue
  exists.
- Pin moves (port, sync): full matrix in same pull request; fragment
  made stale by port is removed there.

**Harness.** Upstream side links with `--wrap=vmaf_feature_collector_init`;
this tree's golden build is LTO, where linker does not wrap, so
harness asks `vmaf_feature_collector_get()` (`core/src/libvmaf_priv.h`).
Keep that accessor. Renaming it or collector's `feature_vector` /
`aggregate_vector` fields breaks build of harness, not score.

**Matrix.** `upstream_parity_matrix.py`. Required fixtures = three Netflix
golden pairs; derived fixtures need only those; every other clip optional
(skipped with printed reason). New extractor shared with upstream, new
option, new model file -> add it there. Changing derived fixture changes
measured maxima: re-measure bounds.

**Tests (no build):** `scripts/ci/tests/test_upstream_parity_allowlist.py`,
`scripts/dev/tests/test_upstream_parity.py`; pre-commit hook
`test-upstream-parity-guard`. Human guide:
`docs/development/upstream-parity.md`.
