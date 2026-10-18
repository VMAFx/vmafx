<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3339: Upstream Consumer ABI Drop-in Parity Verification and Defect Plant Gate

- **Status**: Accepted
- **Date**: 2026-10-18
- **Deciders**: lusoris
- **Tags**: compat, abi, testing, ci

## Context

The fork provides a drop-in binary compatible `libvmaf.so.3` shared library that routes public C API calls through `libvmafx.so.1` ([ADR-1852](1852-vmafx-api-redesign.md)). While in-tree parity test harnesses (`scripts/dev/upstream_parity_harness.c`) compare feature extraction numerical accuracy via internal feature collector hooks, they require static linkage (`libvmaf.a`) and wrap internal symbols (`--wrap=vmaf_feature_collector_init`). This does not prove that an external unmodified binary dynamically linking `libvmaf.so.3` can seamlessly substitute Netflix's shared object with bit-identical results. Furthermore, issue #2473 requires an active regression defense proving that intentional or inadvertent ABI behavioral divergence immediately trips the parity gate.

## Decision

We will implement `scripts/dev/compat_abi_consumer.c`, an unmodified C consumer using strictly public Netflix libvmaf C API headers (`libvmaf/libvmaf.h`, `model.h`, `picture.h`), compiled once against upstream headers and dynamically linked with `-lvmaf`. At runtime, `LD_LIBRARY_PATH` directs dynamic symbol resolution to the target library directory (upstream `libvmaf.so.3` vs fork `libvmaf.so.3` which dynamically links `libvmafx.so.1`).

To discover metric names and aggregate metrics without private APIs, the consumer queries JSON output via `vmaf_write_output()` into an ephemeral file, extracting metric alias keys matching those stored in the feature collector and parsing aggregate metrics (such as APSNR).

To prove defect detection, an LD_PRELOAD plant shim (`scripts/dev/compat_abi_plant.c`) intercepts `vmaf_feature_score_pooled()` and `vmaf_score_pooled()`, perturbing pooled scores by 1 ULP via `nextafter()`, causing `--plant` to fail with exit code 1.

The parity guard (`scripts/dev/upstream_parity.py`) is extended with `--abi` and `--plant`, enforcing SONAME validation (`libvmaf.so.3`), DT_NEEDED linkage verification, and dynamic loader resolution checks (`ldd`) to eliminate silent fallbacks.

In CI, the gate runs in `.github/workflows/tests-and-quality-gates.yml` on `ubuntu-26.04` with `--unpinned` (compiling both trees with the host GCC; the hosted image's GCC defaults to `-march=x86-64-v3` with FMA enabled, so the upstream build is held to the pinned image's `-march=x86-64` baseline, because Netflix's build leaves floating-point contraction on and fused multiply-adds moved its model scores by up to 3e-12). Upstream's float ADM multiplies by a reciprocal estimate (`_mm_rcp_ss`, ADR-1442) whose value differs between Intel and AMD processors, so the bounds of `float_adm.divide` and `model.float-adm-divide` carry a measurement from an Intel runner next to the AMD one (4.06e-07 and 1.803e-05 on a Xeon Platinum 8573C; the full matrix has not run on an Intel processor), while local and release preflight runs enforce the pinned dev container image (`vmaf-dev-mcp:local`).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Option A: Discover metric names via temp JSON output | Zero internal headers, purely public API, robust across models and feature sets | Requires ephemeral temp file write and JSON parsing | Chosen: Completely hermetic, portable, matches public consumer usage without external IPC |
| Option B: Pass metric names from Python driver | No temp file in C consumer | Requires two-way IPC or argument passing protocol between Python runner and consumer | Fragile when consumer runs independently or via direct invocation |
| Option C: Query internal feature collector in consumer | Direct in-memory access to names | Requires non-public headers (`libvmaf_priv.h`) or linker `--wrap`, defeating external consumer proof | Breaks ABI drop-in consumer premise |

## Consequences

- **Positive**: Verifiable proof of drop-in dynamic ABI compatibility between Netflix `libvmaf.so.3` and fork `libvmaf.so.3`. Defect plant reliably trips the gate on 1 ULP pooled score perturbation.
- **Negative**: Ephemeral JSON write and parse in the consumer adds marginal execution overhead (~10ms per run).
- **Neutral / follow-ups**: Make targets `compat-abi-parity` and `compat-abi-parity-plant` added. CI workflow in `tests-and-quality-gates.yml` executes probe and plant under `ubuntu-26.04`.

## References

- Issue #2473: identical scores to Netflix's `libvmaf.so.3` through an unmodified upstream consumer, and a planted compat defect that fails the job.
- Operator order `vmafx-next-20261017-2359` (2026-10-17, paraphrased): take the next milestone 0.1.0 issue to a mergeable pull request.
- [ADR-1852](1852-vmafx-api-redesign.md), [upstream parity guide](../development/upstream-parity.md).
