# ADR-1169: The fork's default VMAF model is `vmaf_v1.0.16_3d0h`

- **Status:** Accepted
- **Date:** 2026-09-03
- **Deciders:** Lusoris
- **Supersedes:** none
- **Superseded by:** none

## Context

[ADR-1168](1168-default-model-single-source.md) made the fork's default model a
single definition and deliberately left its *value* alone, because changing it
appeared to collide with the Netflix golden gate. This ADR makes the change and
records why that collision turned out not to exist.

**Why now, rather than after 1.0.0.** The fork's own models — the tiny-AI
heads, the `vmaf-tune` calibration, the benchmark baselines — are trained and
tuned against whatever the default scores. Shipping 1.0.0 on `vmaf_v0.6.1` and
moving to the v1 generation afterwards means doing that training, benchmarking
and tuning **twice**. Per user direction, the model generation is settled before
1.0.0 so the retraining programme runs once, against the model the release
actually ships.

**Why `_3d0h`.** [`docs/models/v1.md`](../models/v1.md) identifies
`vmaf_v1.0.16_3d0h` as the standard 1080p model at a 3H viewing distance — the
same condition `vmaf_v0.6.1` was trained for, and therefore its direct
counterpart. The other v1 variants target different viewing conditions
(`5d0h` phone, `1d5h_2160` 4K at 1.5H, `3d0h_2160` 4K at 3H on a [0, 110]
range) and are not general-purpose defaults.

**What the golden gate actually said.** Flipping the default fails exactly one
test, `vmafexec_test.py::test_run_vmafexec_runner_use_default_built_in_model`,
and ADR-1168 recorded that as an unresolvable conflict with
[ADR-0024](0024-netflix-golden-preserved.md). That was wrong, and the error
message says so:

```text
KeyError: KeyError('VMAFEXEC_vif_scale0_score')
```

It is **not** an `assertAlmostEqual` mismatch. No golden value drifts. The test
asserts values for the v0.6.1 feature family — `vif_scale0..3`, `motion2` — and
the v1.0.16 family does not compute those features at all. It emits
`integer_aim_*`, `cambi_*`, `speed_chroma_*`, `integer_adm*` and
`integer_motion2/3` instead. The test failed because its *feature keys* were
absent, not because a number moved.

That distinction matters, because ADR-0024 protects assertion **values**. It is
satisfiable here.

**Upstream is not doing this.** Netflix published the v1.0.16 models as
additional `--model` choices and left their built-in default at `vmaf_v0.6.1`
(`libvmaf/tools/cli_parse.c`, upstream master, verified 2026-09-03). Their tests
pass because their default is unchanged. This is a deliberate fork divergence,
and the fork owns its consequences.

## Decision

`VMAF_DEFAULT_MODEL_VERSION` becomes `vmaf_v1.0.16_3d0h`. Under ADR-1168 that
is one line, and the gate names every mirror that must follow.

1. **The default and the 4K ladder move to the v1 generation together.**
   `Model1080P` / `MODEL_1080P` follow the default; `Model4K` / `MODEL_4K`
   become `vmaf_v1.0.16_1d5h_2160`, the 4K default named by `docs/models/v1.md`.
   Splitting the ladder across two model generations would produce scores that
   are not comparable across a resolution boundary.
2. **NEG stays on the v0.6.1 family, and is no longer derived by
   concatenation.** Netflix published NEG variants for `vmaf_v0.6.1`,
   `vmaf_float_v0.6.1` and `vmaf_4k_v0.6.1` only; there is no NEG counterpart to
   any `vmaf_v1.0.16_*` model. The previous Python mirror computed
   `DEFAULT_MODEL + "neg"`, which after this change would synthesise
   `vmaf_v1.0.16_3d0hneg` — a model that does not exist and that libvmaf rejects
   at load. `DEFAULT_MODEL_NEG` and `DefaultNEGVersion` are now independent
   constants naming the v0.6.1 family, with the reason in the source.
   **Consequence: asking for NEG also changes model generation.**
3. **The one affected golden test names its model instead of inheriting it.**
   `test_run_vmafexec_runner_use_default_built_in_model` now passes
   `models: ["name=vmaf:version=vmaf_v0.6.1"]`. **No assertion value is
   touched.** Naming the model reproduces the previous no-`--model` invocation
   byte-for-byte: identical metric-key set and identical pooled values, verified
   against a master build.
4. **The coverage that test gave up is replaced, not dropped.** A fork-added
   `python/test/default_model_test.py` asserts that the no-`--model` invocation
   matches an explicitly-named `vmaf_v1.0.16_3d0h`, that it emits the v1 feature
   family, that it does *not* emit `vif_scale0`, that `vmaf_v0.6.1` is still
   selectable, and that the NEG default stays on the v0.6.1 family. It hardcodes
   **no score values** — inventing golden numbers for our own default would be
   asserting our output back at ourselves.
5. **Two named exceptions keep `vmaf_v0.6.1`:** the AOM CTC preset, because the
   Common Test Conditions specification mandates that exact model (already
   carrying a `vmaf-model-pin:` marker), and
   `VmafQualityRunner.DEFAULT_MODEL_FILEPATH` in the `compat/python-vmaf`
   harness, whose purpose is reproducing Netflix's published numbers.

## Consequences

- **Positive**: the fork's model generation is settled before 1.0.0, so the
  tiny-AI retraining, `vmaf-tune` recalibration and benchmark rebaselining
  happen once against the shipped default.
- **Positive**: users get the newer, more accurate model by default, and
  10-bit-aware banding (`cambi`) and chroma (`speed_chroma`) features are in the
  default score.
- **Negative, and the headline user-visible change**: **every score changes.**
  On the standard 576x324 pair the default moves from 76.667831 to 82.816059.
  Anyone comparing against historical numbers must either pin
  `--model version=vmaf_v0.6.1` or rebaseline. This is a breaking change to
  output values, though not to any API.
- **Negative**: `--neg` now crosses model generations, because no v1 NEG model
  exists. A user asking for NEG gets a v0.6.1-family score.
- **Negative**: the fork diverges from upstream on a user-visible default, which
  is a permanent rebase-sensitive difference in `cli_parse.cpp`. Recorded in
  `docs/rebase-notes.md`.
- **Neutral**: `vmaf_v0.6.1` and every other model remain selectable and
  built in. Nothing is removed.

## Alternatives considered

| Option | Pros | Cons | Verdict |
| --- | --- | --- | --- |
| **Default `vmaf_v1.0.16_3d0h`, golden test names its model (chosen)** | Model generation settled before 1.0.0, so retraining runs once; no golden value altered; lost coverage replaced by a fork test | Every default score changes; permanent upstream divergence; NEG crosses generations | The only option that gets the v1 generation into 1.0.0 without touching a golden value |
| Keep `vmaf_v0.6.1` as default, ship 1.0.0, migrate later | Zero divergence now; no score change | Forces the whole training / benchmark / tune programme to run twice, which is the specific cost the maintainer called out | Rejected |
| Change the golden test's `assertAlmostEqual` values to the v1 numbers | Test still covers "the default" directly | Modifies Netflix golden assertions — forbidden by ADR-0024 and the first global rule | Rejected outright |
| Make `use_default_built_in_model` send an explicit `vmaf_v0.6.1` in the harness instead of editing the test | Leaves `python/test/` untouched entirely | Makes the flag lie: "use default built-in model" would stop using the default. Hides the divergence in harness plumbing where no reader would find it | Rejected — dishonest surface |
| Flip only the C/CLI default, leave Go and Python tools on v0.6.1 | Smaller blast radius | `vmaf` and `vmafx-tune` would report different numbers for the same input; defeats the single source ADR-1168 just established | Rejected |
| Adopt `vmaf_v1.0.16_5d0h` or a 4K variant as the default | — | Those target phone and 4K viewing conditions; `_3d0h` is the documented 1080p/3H counterpart to v0.6.1 | Rejected |

## References

- req — per user direction: the new Netflix model must be the default before
  1.0.0, because otherwise the fork's own models have to be retrained, retested,
  benchmarked and tuned a second time after the release.
- req — the maintainer questioned the claimed golden-gate conflict on the
  grounds that Netflix released the model and their tests pass. Investigating
  that objection produced the `KeyError` finding above and is the reason this
  ADR exists; the earlier conflict claim in ADR-1168 was wrong.
- [ADR-1168](1168-default-model-single-source.md) — the single-source mechanism
  that makes this a one-line change.
- [ADR-0024](0024-netflix-golden-preserved.md) — protects golden assertion
  values; satisfied here, none changed.
- [ADR-1122](1122-vmaf-v1-model-port.md) — the port that made the v1.0.16 models
  available as built-ins.
- [`docs/models/v1.md`](../models/v1.md) — names `vmaf_v1.0.16_3d0h` the
  standard 1080p / 3H model.
- Upstream `Netflix/vmaf` master, `libvmaf/tools/cli_parse.c`: still defaults to
  `vmaf_v0.6.1` (verified 2026-09-03).
