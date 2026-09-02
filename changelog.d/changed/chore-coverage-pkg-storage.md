### Chore

- Extend test coverage for `pkg/storage`: add `coverage_test.go` covering
  `unmount()` (0% → 100%), `waitForPath()` timeout and context-cancel exits
  (80% → 100%), `waitForHTTP()` timeout, context-cancel, and 5xx-loop branches
  (73.3% → 93.3%), `killProcess()` with a real subprocess (25% → 87.5%),
  both `Prepare()` start-failure paths, and the `localPath()` url.Parse error
  branch.  Overall `pkg/storage` statement coverage rises from 52.6% to 77.9%.
  Add `cmd/vmafx-node/bpf/coverage_test.go` with `New()` prefix-normalisation
  and `IsBypassFD()` boundary assertions; remaining kernel-gated bpf branches
  are documented as deferred to a CAP_BPF integration environment (ADR-1087).
