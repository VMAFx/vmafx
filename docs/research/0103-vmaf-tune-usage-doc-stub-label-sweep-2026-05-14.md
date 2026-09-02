# Research 0103 — `vmaf-tune` Usage-Doc Stub Label Sweep

Date: 2026-05-14

## Question

The `vmaf-tune` usage tree still labelled several user-visible pages as
stubs even though the backing code has since landed. Which labels are stale,
and what should the docs say now?

## Findings

- `corpus --coarse-to-fine` is implemented in `vmaftune.corpus` and wired
  through the CLI. The standalone page now documents the two-pass CRF search
  instead of a placeholder.
- Phase E ladder generation is implemented in `vmaftune.ladder`. The default
  sampler runs the canonical `18,23,28,33,38` CRF sweep through the normal
  Phase A encode-and-score path, then feeds the sampled rows into Pareto hull
  and knee selection.
- `recommend-saliency --saliency-aware` is the live CLI surface for saliency
  ROI encoding. The older `recommend --saliency-aware` wording was stale.
- `fast` has production wiring for sample extraction, proxy inference, and a
  verify pass. `--smoke` remains a dependency-free test mode, not the only
  executable path.

## Decision

Retire the stale stub/scaffold labels from the dedicated usage pages and the
main `vmaf-tune.md` page. Keep explicit production limits where they are still
true, especially that Phase E's default sampler is a fixed 5-point CRF sweep
rather than a full Phase B bisect.

## Alternatives considered

- Leave the stub labels and rely on source/tests for truth. Rejected because
  they are user-discoverable documentation and directly mislead operators.
- Delete the dedicated pages until a broader docs rewrite. Rejected because
  the surfaces are live and need human-readable usage documentation in the
  same PR.

## Validation

```bash
rg -n 'scaffold-only|Status: scaffold only|\(stub\)|\*\*Stub\*\*|recommend --saliency-aware|advisory in scaffold' \
  docs/usage/vmaf-tune.md \
  docs/usage/vmaf-tune-coarse-to-fine.md \
  docs/usage/vmaf-tune-bitrate-ladder.md \
  docs/usage/vmaf-tune-ladder-default-sampler.md \
  docs/usage/vmaf-tune-saliency-aware.md
```
