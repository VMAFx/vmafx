# Reporting Bad Cases

VMAF's predictions do not always reflect perceived quality — corner cases
and novel application scenarios outside the training distribution both
produce mispredictions. Bad-case reports are valuable for improving future
model versions.

## Upstream channel (Netflix/vmaf)

Netflix maintains a Google form to collect bad-case samples. Users can opt
in or out for public sharing:

- [Bad-case submission form](https://docs.google.com/forms/d/e/1FAIpQLSdJntNoBuucMSiYoK3SDWoY1QN0yiFAi5LyEXuOyXEWJbQBtQ/viewform?usp=sf_link)

## Fork channel (VMAFx/vmafx)

For bad cases that are specific to fork-added surfaces — SYCL / CUDA / HIP
numerical divergence, `--precision` output correctness, tiny-AI model
drift — open an issue on [VMAFx/vmafx](https://github.com/VMAFx/vmafx/issues)
with reproducer inputs and, if possible, the backend that produced the
anomalous result.

A cross-backend numeric diff can be generated via the `/cross-backend-diff`
skill before filing, which narrows the report to the specific feature and
scale where the divergence is observed.
