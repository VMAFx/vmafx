---
paths:
  - scripts/ci/check-runner-available.sh
  - scripts/ci/tests/test-runner-available.sh
  - scripts/ci/test_self_hosted_runner_workflow_contract.py
  - .github/workflows/sycl-parity.yml
  - dev/docker-compose.runner.yml
  - dev/scripts/arc-render-node.sh
invariant: Fork PRs never run on self-hosted hardware; Arc container sees only Arc node; enabled lane must report `success`.
---
<!-- markdownlint-disable MD013 MD060 -->
# Self-hosted runners

## Self-hosted SYCL Arc runner invariants (ADR-1177)

Intel Arc A380 self-hosted runner executes hardware-in-the-loop SYCL parity tests
under `.github/workflows/sycl-parity.yml`. Following invariants load-bearing:

1. **Untrusted fork PR execution prohibition**: `sycl-parity.yml` must strictly enforce
   `if: github.event_name != 'pull_request' || github.event.pull_request.head.repo.full_name == github.repository`.
   Fork PRs must NEVER execute arbitrary workflows or code on self-hosted infrastructure.
2. **Device isolation**: Container passthrough (`dev/docker-compose.runner.yml`)
   restricted to `/dev/dri/renderD129` (Intel Arc A380, vendor `0x8086`, device `0x56a5`,
   PCI `03:00.0`). Host NVIDIA RTX 4090 and AMD iGPU device nodes must NOT pass into
   container under any circumstances.
3. **Container security posture**: Runner container runs as unprivileged user
   `runner` (uid 1001, gid 1001) in groups 988 (`render`) and 984 (`video`). No Docker socket
   (`/var/run/docker.sock`) mounted. Container resource limits capped at 8 CPUs and 16 GB RAM.
   Ephemeral mode (`--ephemeral`) ensures clean environment per job without state persistence.
4. **Lane-switch contract**: `required-aggregator.yml` lists `SYCL Parity (Arc A380)` as required,
   reads `vars.SYCL_ARC_RUNNER_ENABLED` (makes no runner API call — `GITHUB_TOKEN` cannot list
   self-hosted runners):
   - Lane disabled (variable unset / not `true`): absent or skipped accepted as pass.
   - Lane enabled: job MUST report `success`; absent or skipped (probe failed because
     runner unregistered, offline, or probe token rejected) = loud aggregator failure.
   Never reintroduce auto-detect probe that treats API error as "unregistered" — makes
   required check silently green.
5. **Probe token**: `check-runner-available.sh` runs runner-list query only while lane
   enabled, with `secrets.SYCL_RUNNER_PROBE_TOKEN` (fine-grained PAT, single repository,
   Administration: read-only). Do not widen workflow's `permissions:` to replace it —
   no `administration` scope there.
6. **Render node resolved, not hard-coded**: `dev/docker-compose.runner.yml` takes
   `ARC_RENDER_NODE` from `dev/scripts/arc-render-node.sh` (exactly one vendor-`0x8086` render node).
   Do not replace with bare `renderD<N>`; numbers change after PCI re-enumeration.

## Self-hosted hardware admission invariants (ADR-1319)

`sycl-arc` and `gpu-full` are different capability contracts. Arc-only
container never satisfies CUDA/HIP or combined-coverage claims. Preserve these
couplings together:

1. `sycl-parity.yml` is sole hardware `float_ssim` parity owner and probes
   `self-hosted linux x64 sycl-arc` before dispatch.
2. `tests-and-quality-gates.yml` has exactly one `gpu-full` consumer,
   `Coverage GPU`; its hosted probe checks `self-hosted linux gpu-full` before
   dispatch. Do not restore retired duplicate SYCL job.
3. required aggregator permits absent/skipped hardware checks only while
   their own switch is disabled. With switch true, only `success` passes.
4. Keep `scripts/ci/test_self_hosted_runner_workflow_contract.py` wired into
   Rule Enforcement. It executes real embedded aggregator JavaScript as
   well as checking workflow ownership and admission edges.

## Workflow coupling

| Script | Workflow lane(s) that invoke it | What couples them |
| --- | --- | --- |
| `check-runner-available.sh` | `sycl-parity.yml` (`runner-available`) and `tests-and-quality-gates.yml` (`gpu-full-runner-available`) | Reads `$RUNNER_ENABLED`; disabled means exit 0, `available=false`, no API call. Enabled means query `GET repos/<repo>/actions/runners` with existing read-only `$GH_TOKEN` and require one ONLINE runner carrying every case-insensitive label in `$RUNNER_LABELS`. custom-label-only match is insufficient: missing `linux` / `x64` can still make `runs-on` unroutable. API error, no complete match, or all complete matches offline = exit 1 with `::error::`. Never maps API error to "unregistered". Tests: `scripts/ci/tests/test-runner-available.sh` and `scripts/ci/test_self_hosted_runner_workflow_contract.py`. |
