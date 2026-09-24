# Research-2082: SYCL upload host-buffer lifetime

- **Status**: Active
- **Workstream**: BUG-040, [ADR-0214](../adr/0214-gpu-parity-ci-gate.md)
- **Last updated**: 2026-09-24

## Question

Why did otherwise identical 4K inputs produce nondeterministic `integer_adm2`
and VMAF scores on the Intel Arc A380, and where can libvmaf enforce the host
picture lifetime without serialising unrelated SYCL compute? A second question
was whether the same ownership rule survived the worker-thread path and a
combined CUDA+SYCL build's early returns.

## Sources

- [`vmaf_sycl_shared_frame_upload`](../../core/src/sycl/common.cpp) records the
  final distorted-plane copy as `last_upload_event` on an in-order
  `copy_queue`.
- [`vmaf_read_pictures`](../../core/src/libvmaf.c) owns the serial, worker, and
  combined-backend cleanup paths.
- [`test_sycl_cuda_serial_upload_lifetime.c`](../../core/test/test_sycl_cuda_serial_upload_lifetime.c)
  is the public-API poison regression for the serial and one-worker paths.
- [`test_sycl_4k_repeat_determinism.py`](../../testdata/test_sycl_4k_repeat_determinism.py)
  compares complete normalised 4K reports from one exact binary and library.
- The durable rebase contract is in
  [`docs/rebase-notes.md`](../rebase-notes.md).

## Findings

### The score drift was host-memory reuse during DMA

The SYCL preparation stage enqueues reference-plane copies followed by
distorted-plane copies. Because `copy_queue` is in order, completion of the
last distorted-plane event also proves completion of every earlier copy for
that frame. Before BUG-040, the ingestion path could drop the final counted
host-picture reference while that event was incomplete. The CLI picture pool
then returned the same storage to its reader, whose next `fread` overwrote
bytes still being consumed by DMA.

Disabling graph replay did not change the drift. A diagnostic checksum wait
did, because its blocking copy accidentally extended the host lifetime. That
made the checksum a useful oracle but an invalid production fix: it masked the
ownership error and added unrelated synchronisation.

### Serial and threaded ingestion have different release boundaries

The serial path may enter CUDA's host-cleanup early return in an all-backend
build. Its SYCL event wait therefore has to precede every cleanup branch.

The threaded path first gives a worker counted `ref` and `dist` references.
The worker may immediately finish and release its copies, while the caller may
also release the original references before post-batch cleanup starts. A wait
in `read_pictures_frame_cleanup_after_batch` is consequently too late. The
safe boundary is after enqueue, while the caller's original references still
pin the storage, and before both the success and enqueue-error paths unref
them. Waiting after enqueue preserves overlap between DMA and the worker's CPU
extractors.

If CUDA translation fails after a successful SYCL upload, the API returns the
pictures to its caller rather than entering ordinary cleanup. That return now
also waits for the SYCL event, so immediate caller-side reuse is safe.

### The regression is red-capable and exact-head green

The immediate predecessor `249220f11d1a299d2c774515ed032066d3427d44`
identified itself as `v3.1.0-2767-g249220f11`. Its one-worker poison probe
failed in all 20 repetitions: at least one of the 48 identical 4K frames in
each run scored below the 60 dB PSNR cap after the release callback filled the
distorted plane with `0xff`.

After rebuilding the same combined CUDA+SYCL target from
`587c3144d75b4728658d95387e133d53655a16f5`, the artifact identified itself as
`v3.1.0-2768-g587c3144d`. Twenty repetitions passed on the Intel Arc A380;
each repetition ran both `n_threads=0` and `n_threads=1`, 48 frames per path,
with all scores at the cap. The exact command was:

```sh
meson test -C build --no-rebuild --repeat 20 -j1 --print-errorlogs \
  libvmaf:test_sycl_cuda_serial_upload_lifetime
```

The production determinism harness separately exercises full VMAF plus
`integer_adm2`, 20 serial and 20 one-worker 4K runs, rather than reducing the
proof to PSNR alone.

## Alternatives explored

| Option | Correctness | Concurrency cost | Decision |
| --- | --- | --- | --- |
| Keep the checksum or wait for every SYCL queue | Hides the symptom | Serialises DMA and unrelated compute | Rejected; diagnostic-only behaviour is not a lifetime contract |
| Wait in post-batch cleanup | Too late when the worker releases first | Low | Rejected by the one-worker poison regression |
| Wait before enqueueing worker work | Safe | Removes useful CPU/DMA overlap | Rejected |
| Wait after enqueue, before caller-reference release | Safe on success and enqueue failure | Retains worker/DMA overlap | Chosen |
| Defer unref through a new completion-callback ABI | Can be safe | More state, teardown, and ABI complexity | Deferred; not needed for this bug fix |

## Open questions

- A future optimisation could attach host-reference release directly to an
  upload-completion callback, but it needs an explicit shutdown and error
  contract before replacing the bounded event wait.
- The poison regression deliberately compiles CUDA and SYCL together but does
  not require an NVIDIA device. Hosted CI still needs an Intel Level Zero GPU
  before it can make this hardware lane required rather than skip-capable.

## Related

- BUG-040: Arc A380 snapshot and production determinism investigation
- [ADR-0214: GPU parity CI gate](../adr/0214-gpu-parity-ci-gate.md)
- [`core/src/AGENTS.md`](../../core/src/AGENTS.md) upload-lifetime invariant
- [`core/test/AGENTS.md`](../../core/test/AGENTS.md) poison-test invariant
