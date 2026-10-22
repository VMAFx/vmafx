<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3359: The SYCL primary queue, which runs the VA-surface import, uses immediate command lists

- **Status**: Accepted
- **Date**: 2026-10-02
- **Deciders**: lusoris
- **Tags**: `sycl`, `zero-copy`, `level-zero`, `correctness`, `driver-workaround`, `fork-local`

## Context

The zero-copy path of the `libvmaf_sycl` filter imports every decoder surface
with `zeMemAllocDevice` (DMA-BUF), copies or de-tiles its luma into the shared
frame slot, and frees the import with `zeMemFree` after the frame's queue wait
(`core/src/sycl/dmabuf_import.cpp`, ADR-1121). That work runs on the primary
SYCL queue, which several extractors and the host-upload path share.

On an Intel Arc A380 (i915, compute-runtime 26.35.39758.10, Level Zero 1.34.0)
with Unified Runtime's batched command lists
(`UR_L0_USE_IMMEDIATE_COMMANDLISTS=0`, the setting
[the bundling page](../backends/sycl/bundling.md) recommends for Arc A-series and
`scripts/test/sycl-dev-container.sh` forces), zero-copy `cambi` differed from
host upload in 3 to 7 of every 10 runs. Measured in the debug session
`sycl-zerocopy-cambi-nondeterminism`:

- From a random frame on, the import's writes into the shared slots stop
  landing; the slots keep two old frames and every extractor scores them.
  Canary kernels on the same queue are dropped as well. `wait_and_throw()`
  reports no error.
- The driver maps each new import at the GPU address the previous frame's import
  just freed. Delaying the free (new address every frame) or never freeing
  (a per-surface import cache) avoids the failure; waiting for every queue
  before the free does not.
- With immediate command lists globally, or on the primary queue alone, the
  failure does not occur, with the same address reuse. The primary queue's
  mode is what matters: a separate import-only queue with immediate command
  lists next to a batched primary queue still failed (8 of 12 runs), while a
  batched import queue next to an immediate primary queue did not (0 of 12).
  The primary queue is the one the imports are made against and the one
  `vmaf_sycl_queue_wait` synchronises before freeing them every frame.

The failure was measured on the i915 kernel driver only (Fedora 43, kernel
6.19.14). On the same A380 model under the xe kernel driver, 180 zero-copy runs
on 2026-10-10 under the batched setting gave 0 scores that differed from host
upload. The defect sits in the driver stack (a submission is silently
discarded), not in libvmaf's synchronisation. An upstream report is drafted,
not filed.

## Decision

We will create the primary SYCL queue (`sycl_queue_props()` in
`core/src/sycl/common.cpp`), on which the VA-surface import runs, with
`sycl::ext::intel::property::queue::immediate_command_list`, guarded by
`SYCL_EXT_INTEL_QUEUE_IMMEDIATE_COMMAND_LIST`. The property is spelled once,
in `vmaf_sycl_in_order_props()` (`core/src/sycl/queue_props.h`), which the
VMAFx library queue (`library_queue_props()` in
`core/src/sycl/vmafx_sycl_rt.cpp`, ADR-2091) uses too. The copy queue, the combined
graph queue and every per-extractor queue keep the process's command-list mode,
so the batched-mode advice for Arc A-series stays for them. Without the
extension macro (another SYCL implementation) the property is left out; on a
backend other than Level Zero it has no effect. The primary queue stays
immediate while the driver defect exists; `zerocopy-e2e.sh --repeat 10` under
`UR_L0_USE_IMMEDIATE_COMMANDLISTS=0` is the re-test.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Immediate primary queue (chosen) | 0 divergent runs in every measured case; zero-copy throughput within noise of HEAD | Extractors that share the primary queue and the host-upload path also leave the batched mode (measured within noise) | Chosen (D-08, option C' as measured) |
| Separate import-only queue with immediate command lists | Leaves the primary queue's mode alone | Still fails: 8 of 12 runs, `--repeat 10` 4/10 and 6/10 on src01 `cambi` | Does not fix the defect |
| Session cache of imports per surface | No address reuse, independent of the command-list mode | Every live import joins each submission's residency: +7 % ms per frame, -6 to -7 % fps at 1080p | Throughput loss above the phase budget |
| Delay each free by two frames | -2 to -4 % fps | Works only because the driver does not recycle the address yet; undocumented | Fragile |
| Stop recommending batched command lists | No code change | Gives up the Arc A-series workaround for every kernel | Rejected (D-08) |

## Consequences

- **Positive**: zero-copy scores are deterministic and equal host upload under
  both command-list modes.
- **Negative**: the kernels that run on the primary queue (among them
  `float_ssim`, `ssim`, `psnr_hvs`, `ciede`, SpEED, `float_vif` and the host
  upload path's helpers) also run on immediate command lists; measured on the
  A380 their host-upload run time moved by -5.5 % to +5.7 %, inside the
  run-to-run spread. The workaround depends on the DPC++ extension and must be
  re-tested when the driver changes.
- **Neutral / follow-ups**: once compute-runtime fixes the drop, re-test with
  `--repeat 10` under `UR_L0_USE_IMMEDIATE_COMMANDLISTS=0` with the property
  removed before dropping the queue.

## References

- Draft PR #2217 (Tualua) carried this record as ADR-1763, and ADR-2091 and
  Research-2160 cite it by that number; the rework renumbered it with
  `scripts/adr/next-free.sh --claim` because the open draft still holds ADR-1763.
- `req` (user, 2026-10-02), D-08: "option C' — only the VA import queue uses `sycl::ext::intel::property::queue::immediate_command_list`; the `UR_L0_USE_IMMEDIATE_COMMANDLISTS=0` advice for other kernels stays." The VA import queue is the primary queue; a separate import-only queue was measured and does not fix the defect (Context).
- [ADR-1121](1121-sycl-qsv-zerocopy-p010-normalization.md), [ADR-1688](1688-sycl-zero-copy-luma-only-admission.md), [ADR-3360](3360-sycl-filter-twin-routing.md).
- Testing: [docs/development/sycl-zerocopy-testing.md](../development/sycl-zerocopy-testing.md).
- Drafted as ADR-1596 on the Tualua fork; renumbered to 1763 when it was ported to
  `VMAFx/vmafx` master, above the numbers its branches claim (up to 1762).
