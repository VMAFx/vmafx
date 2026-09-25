<!-- markdownlint-disable MD013 MD060 -->
# ADR-1319: Admit self-hosted GPU jobs through a live fail-closed probe

- **Status**: Accepted
- **Date**: 2026-09-25
- **Deciders**: Lusoris
- **Tags**: `ci`, `gpu`, `security`, `self-hosted-runner`, `fork-local`

## Context

At reviewed collector commit `a7f77d66997a1a718e7f32156ceb11b3af7786dc`,
`tests-and-quality-gates.yml` had two jobs targeting
`[self-hosted, linux, gpu-full]`. The live repository and organisation runner
APIs returned zero registered runners, and the repository variables API returned
zero variables. The local workstation also had no installed runner service,
runner environment, supervisor state, runner process or runner container.

An unset `GPU_COVERAGE_ENABLED` variable happened to skip both jobs, but the
variable was the only admission check. Setting it to `true` without first
provisioning the exact label set would submit the jobs to a queue that no runner
could consume. The required-check aggregator then accepted absent or skipped
hardware checks under its generic path-filter semantics. The second `gpu-full`
consumer also duplicated the `float_ssim` ownership that
[`sycl-parity.yml`](../../.github/workflows/sycl-parity.yml) and
[ADR-1177](1177-sycl-arc-self-hosted-runner.md) had already assigned to the
isolated `sycl-arc` lane.

The capabilities are not interchangeable. The Arc-only runner design exposes
one Intel render node and deliberately cannot execute CUDA or HIP. No repository
change can truthfully turn it into the multi-vendor `gpu-full` host. Separately,
no current job executes a tiny-AI ONNX model on two execution providers, so the
documented CPU/CUDA variance remains workstation evidence rather than CI proof.

## Decision

We will keep `gpu-full` and `sycl-arc` as distinct capability contracts. The
`sycl-parity.yml` workflow is the sole owner of hardware `float_ssim` parity;
the obsolete `SYCL float_ssim Parity` job is removed from
`tests-and-quality-gates.yml` and from the required-check list.

Every remaining self-hosted hardware job is admitted by two independent facts:

1. its repository lane switch is exactly `true`; and
2. a hosted probe finds an online runner carrying **every** label in the job's
   `runs-on` set.

The probe runs before the self-hosted job. A disabled lane returns
`available=false` without querying GitHub and cleanly skips hardware dispatch.
An enabled lane with a rejected API call, no complete label match or only
offline matches fails loudly. The dependent hardware job is therefore skipped
before GitHub tries to route it. The required-check aggregator permits
absence/skip only while that lane's switch is disabled; while enabled, the
hardware check must report `success`.

This decision changes repository admission only. It does not register, relabel,
start or configure an external runner, and it does not set either repository
variable. Missing `gpu-full` capacity, missing live `sycl-arc` execution and
missing tiny-AI cross-device parity remain explicit open state.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Relabel the Arc A380 runner `gpu-full` | No workflow edit | Advertises CUDA and multi-vendor capability the isolated container does not have; a queued CUDA build would fail or hang | False capability claims are worse than an explicit unavailable lane |
| Retarget both old jobs to `sycl-arc` | Uses the intended Arc host | `Coverage GPU` enables CUDA and expects a multi-capability toolchain; narrowing it in place would change the coverage contract | Keep the full-coverage contract honest and independently provisionable |
| Delete `Coverage GPU` until hardware exists | No impossible dispatch | Erases the desired coverage recipe and makes later provisioning a code change | Preserve the dormant contract behind safe admission |
| Keep the variable-only guard | Smallest textual change | A stale `true` value can queue forever; generic aggregator skip semantics conceal probe failure | Does not solve the observed failure mode |

## Consequences

- **Positive**: no repository workflow can dispatch `Coverage GPU` merely
  because a variable is set; incomplete default/custom labels are rejected;
  duplicate SYCL parity ownership is gone; enabled hardware lanes fail closed.
- **Negative**: the hosted probe consumes a short hosted job and needs the
  existing read-only runner-list token whenever a lane is enabled.
- **Neutral / follow-ups**: operators still need to provision and validate the
  two distinct runner capabilities. Tiny-AI cross-device parity needs a
  separate design and executable hardware before its numeric bounds can become
  gates.

## References

- [Research: self-hosted GPU runner admission](../research/gpu-runner-admission-2026-09-25.md)
- [ADR-1177](1177-sycl-arc-self-hosted-runner.md)
- [GitHub workflow syntax: `jobs.<job_id>.if`](https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-syntax#jobsjob_idif)
- [GitHub REST API: self-hosted runners](https://docs.github.com/en/rest/actions/self-hosted-runners)
- Source: `req` — “oh of course all bugs.md's in this local repo should of course be fully fixed”.
