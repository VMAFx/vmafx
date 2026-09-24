# Research-2084: dev-MCP resilience restoration

- **Status**: Complete
- **Workstream**: BUG-048 A8; no ADR (bug restoration)
- **Last updated**: 2026-09-24

## Question

Did the dev-MCP GPU visibility probes, CUDA-aware Compose readiness check, and
libvmaf output opener still carry the resilience behavior introduced by
`4f807c746`, or had the repository-layout reconciliation removed it while its
documentation and operational assumptions survived?

## Sources

- `4f807c746a55a081783c57f1b219807c309e78f7`, the producer commit
  (`fix(dev): entrypoint probes, env propagation, cold-start CUDA healthcheck
  (#1562)`).
- `384d97d037f1537c934ff67c51ff07e19c3bda6b`, the later repository-layout
  reconciliation that reset the affected blobs.
- Current `dev/scripts/dev-mcp-entrypoint.sh`, `dev/docker-compose.yml`, and
  `core/src/libvmaf.c` at exact base `4e6916d16ac57647105d14a47a6680117d6b5738`.
- Live `sycl-ls` and `rocminfo` output from the existing healthy
  `vmaf-dev-mcp` container, plus host `rocminfo` output.
- The BUG-048 A8 row in
  `.workingdir/evidence/silent-reverts-2026-09-18.md`.

## Findings

The three regressions were independently present.

1. The SYCL probe searched only for the loose token pair
   `level_zero.*gpu`. A real OpenCL-only record such as
   `[opencl:gpu][opencl:1]` was missed. The HIP probe searched for
   `Agent.*GPU|gfx[0-9]+`; current `rocminfo` exposes a GPU through a full
   `Device Type: GPU` record, while an initialization diagnostic mentioning
   `gfx1036` could satisfy the loose token search without any usable agent.
2. Compose admitted the service after `vmaf --version` and allowed only 20
   seconds for startup. That proves the CLI is installed, but says nothing
   about the NVIDIA driver on a container where `/dev/nvidia0` is exposed.
   The historical fix checked that the path was a character device; that is
   still not a driver-readiness query.
3. `output_file_open()` attempted `_open()` / `open()` once and returned
   `-EINTR`. A signal arriving during output creation therefore failed an
   otherwise valid `vmaf_write_output()` request. The producer retried once;
   the layout reconciliation restored its parent implementation.

The historical blob ancestry confirms a stale-content clobber rather than a
newly designed behavior change. The producer's `dev/docker-compose.yml` blob
differs from both its parent and the layout commit, while the layout commit's
`core/src/libvmaf.c` is byte-identical to the producer's parent at the old
`libvmaf/src/libvmaf.c` path.

## Decision

Restore the intended behavior against today's tree, tightening it where live
runtime evidence falsifies the historical approximation:

- SYCL accepts only a leading, bracketed `level_zero:gpu` or `opencl:gpu`
  runtime record. HIP accepts only a complete `Name: gfx...` or
  `Device Type: GPU` line. Diagnostic prose cannot produce success.
- The health helper always checks `vmaf --version`. When the NVIDIA device is
  exposed it requires `nvidia-smi --query-gpu=index` to succeed, and Compose
  provides a 45-second cold-start period. CPU, SYCL, and HIP-only hosts do not
  acquire an NVIDIA dependency.
- The output opener retries exactly once when the first call fails with
  `EINTR`, on both POSIX and Windows, then preserves the existing `-errno`
  contract.

No ADR is needed. These are bounded repairs to previously shipped error and
readiness behavior; they add no public API, backend, or policy choice.

## Alternatives considered

| Alternative | Result |
| --- | --- |
| Restore the historical regexes literally | Rejected: live OpenCL-only output still fails, and a diagnostic containing `gfx1036` still false-passes. |
| Treat `/dev/nvidia0` being a character device as ready | Rejected: device-node creation can precede a responsive driver. |
| Run `nvidia-smi` on every host | Rejected: it would make non-NVIDIA developer machines permanently unhealthy. |
| Probe the MCP Unix socket | Rejected: ADR-0641's active service transport is stdio and creates no socket by default. |
| Retry output open without a bound | Rejected: an unbounded loop violates the bounded-control-flow standard and can hide persistent signal pressure. |
| Retry once only on `EINTR` | Chosen: it restores the producer's narrow transient-failure behavior without masking any other error. |

## Executable evidence

Tests were added before production changes.

- `scripts/ci/tests/test-dev-mcp-entrypoint-probe.sh` failed three cases on
  the exact base: OpenCL-only SYCL was missed, the HIP `Device Type` record
  was missed, and diagnostic prose containing `gfx1036` false-passed. It now
  passes all 13 cases while retaining the existing no-`eval` injection tests.
- `dev/scripts/test-dev-mcp-healthcheck.sh` initially failed because the helper
  did not exist. It now models a non-NVIDIA host, a present-but-unready NVIDIA
  device, a ready device, and a broken CLI; it also pins Compose's helper
  command plus 45-second start period.
- `core/test/test_output_open_eintr.c` uses GNU ld fault injection against the
  actual `open64` symbol selected by the project's large-file flags. Before
  the repair, the injected first-call `EINTR` produced `could not open file`
  and failed the test. After the repair, the write succeeds and the control
  proves exactly two calls occurred. The target is Linux/static-only with LTO
  disabled so the linker seam remains observable.

No Netflix golden assertion or score path changes. Output serialization begins
only after the same file descriptor is successfully opened.

## Related

- BUG-048 A8
- [ADR-0540](../adr/0540-dev-container-ffmpeg-av1-and-hwaccel-encoders.md)
- [ADR-0641](../adr/0641-dev-container-encoder-probe-hardening.md)
- [dev-MCP operator guide](../development/dev-mcp.md)
