# Reporting Bad Cases

VMAF's predictions do not always reflect perceived quality — corner cases
and novel application scenarios outside the training distribution both
produce mispredictions. Bad-case reports are valuable for improving future
model versions.

## Upstream channel (Netflix/vmaf)

Netflix maintains a Google form to collect bad-case samples. Users can opt
in or out for public sharing:

- [Bad-case submission
  form](https://docs.google.com/forms/d/e/1FAIpQLSdJntNoBuucMSiYoK3SDWoY1QN0yiFAi5LyEXuOyXEWJbQBtQ/viewform?usp=sf_link)

## Fork channel (VMAFx/vmafx)

For bad cases that are specific to fork-added surfaces — SYCL / CUDA / HIP
numerical divergence, `--precision` output correctness, tiny-AI model
drift — open an issue on [VMAFx/vmafx](https://github.com/VMAFx/vmafx/issues)
with reproducer inputs and, if possible, the backend that produced the
anomalous result.

A cross-backend numeric diff can be generated via the `/cross-backend-diff`
skill before filing, which narrows the report to the specific feature and
scale where the divergence is observed.

## What to include in a report

A report that names the command, the build and the backend can be reproduced
without a round trip. Include:

1. The output of `vmaf --version`.
2. The full command line, including every `--feature`, `--model` and
   `--backend` argument.
3. The `backend_used` and `feature_backends` keys of the JSON output
   (`--json`), which record which backend produced each score.
4. The scores printed with `--precision max`, so that a last-digit difference
   is visible.
5. The input pixel format and bit depth, and the resolution.
6. A short clip, or a way to fetch the input, that reproduces the result.

File the report through the
[issue templates](https://github.com/VMAFx/vmafx/issues/new/choose): use
"bug report" for a wrong score, "performance regression" for a slowdown and
"hardware report" for a result from a device the project does not test. Open
and confirmed bugs are tracked in [the state ledger](../state.md).
