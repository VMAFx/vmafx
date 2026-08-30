<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1087: Extend test coverage for pkg/storage and cmd/vmafx-node/bpf

- **Status**: Accepted
- **Date**: 2026-06-06
- **Deciders**: Lusoris
- **Tags**: `test`, `storage`, `ebpf`, `coverage`

## Context

`pkg/storage` shipped at 52.6% statement coverage. The uncovered branches
included all of `unmount()` (0%), the timeout and context-cancel exit paths
of `waitForPath()` and `waitForHTTP()`, the `killProcess()` path for a real
running process, the rclone-binary-missing start-failure paths of both
`Prepare()` methods, and the `localPath()` url.Parse error branch.

`cmd/vmafx-node/bpf` shipped at 33.0% statement coverage. The remaining gaps
(`closeLinks` with a populated slice, `Stop` with a non-nil `ringbuf.Reader`,
`writeMountPrefix`, and the full `drainLoop` body) all require live kernel BPF
objects — either a real `ebpf.Map` to back `ringbuf.NewReader`, or a
`link.Link` implementation (the interface carries an unexported `isLink()`
method that prevents external mocking).

## Decision

We add `pkg/storage/coverage_test.go` covering the branches reachable without
a real rclone binary or FUSE kernel module: timeout and context-cancel exits of
`waitForPath()` and `waitForHTTP()`, a 5xx-returning mock HTTP server to verify
the poll loop continues, `killProcess()` with a real `sleep 60` subprocess,
three `unmount()` scenarios via fake shell scripts on a controlled `PATH`, the
rclone-binary start-failure paths of both `HTTPServeStorage.Prepare()` and
`FUSEMountStorage.Prepare()`, and the `localPath()` url.Parse error branch.

For `cmd/vmafx-node/bpf` we add `coverage_test.go` with prefix-normalisation
assertions for `New()` and boundary-condition cases for `IsBypassFD()`. The
remaining kernel-gated branches (closeLinks populated, drainLoop ring-buffer
paths, writeMountPrefix) are documented as intentionally deferred to the
integration path that requires CAP_BPF.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Interface-wrap `ringbuf.Reader` to allow mocking | Would unlock drainLoop branch coverage | Changes production types; adds indirection for a rarely-exercised path | Disproportionate churn for marginal gain |
| Generate real BPF objects in CI | Full coverage of kernel paths | Requires CAP_BPF, Linux 5.15+, clang, libbpf-dev; unavailable in standard CI containers | Infrastructure cost exceeds value |
| Skip bpf coverage entirely | No work | Leaves 0% on `closeLinks` body and `drainLoop` ring-buffer paths | Partial improvement plus documentation is strictly better |

## Consequences

- **Positive**: `pkg/storage` coverage rises from 52.6% to 77.9%; all
  non-kernel-gated branches are now exercised.
- **Negative**: The `unmount()` tests cannot run in parallel (they use
  `t.Setenv` to override PATH); three tests are sequential.
- **Neutral / follow-ups**: The kernel-gated bpf branches remain at their
  prior coverage levels; a future integration-test environment with CAP_BPF
  can pick up the remainder without changing production code.

## References

- ADR-0719: vmafx-node rclone integration.
- ADR-0779: rclone FUSE bypass eBPF program.
- Source: routine agent task — `DRAFT-ONLY — coverage-pkg-storage`.
