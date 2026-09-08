# Research-2058 — One verified envtest tool version

## Defect and ownership

Scorecard 5.5.0's successful post-parser scan reports a mutable Go installer:
`setup-envtest@latest` in `.github/workflows/go-ci.yml`. The Make target also
used `@latest`, but skipped installation whenever any PATH command existed.
CI and local tests could therefore use different tool implementations.

`build-config.env` now owns `SETUP_ENVTEST_VERSION=v0.25.0` and the existing
`ENVTEST_K8S_VERSION=1.31` default. Both consumers invoke
`scripts/ci/setup-envtest.sh`. Installation uses Go's existing GOBIN / first
GOPATH-bin destination, verifies the resulting module/version build metadata,
and executes that exact path. An older PATH binary cannot replace it.
Make retains its Kubernetes-version override and prints a shell-quoted export.
Install, metadata and asset lookup failures propagate to Make and CI.

The helper serves Bash/Make and Ubuntu Go CI. It requires an absolute POSIX
Go installation path; native Windows tooling is not validated by this change.
`path` and `env` require the installed tool and asset cache, using upstream
`--installed-only` to reject missing binaries without querying the network.
Only `install` may acquire assets. Explicit `--use-env=false` prevents an
inherited environment setting from substituting an unrelated asset path.

## Version evidence

The official Go module proxy resolves current `@latest` to v0.25.0, published
2026-09-03. The submodule tag is `tools/setup-envtest/v0.25.0` at controller-runtime
commit `e8f9455e429046cea3a1033c2e8555bcdfc8b08c`.
Its module declares Go 1.26.0. The module checksum is
`h1:P/fGuSapw13MrCE45zaTc3aC3BQbkk66u7slHxApd1k=`.
The observed host installation was older v0.24.1; merely finding that executable
was insufficient. Renovate now tracks the canonical Go-module version field.
No application Go module, Kubernetes API dependency or scientific Python floor
changes in this repair.

## Alternatives

| Option | Tradeoff | Decision |
| --- | --- | --- |
| Canonical pin + shared installer + executable metadata check | Preserves installation destination and verifies the actual consumer | Chosen |
| Replace two `@latest` strings independently | Still permits drift and stale PATH binaries | Rejected |
| Let Make accept any existing setup-envtest | Hides tool-version skew | Rejected |
| Change the Kubernetes fixture generation with the tool | Adds an unrelated API compatibility change | Rejected |

This implements existing ADR-1231 ownership rather than introducing a new
version policy; no separate architectural ADR is needed.

## Verification

The real v0.25.0 tool compiles with Go 1.26.7 in a private GOBIN. Its install,
path and environment modes successfully select a private copy of the existing
Kubernetes 1.31.0 assets with network asset lookup disabled. Original and
copied etcd, kube-apiserver and kubectl hashes match. The unchanged controller
package passed, including all 14 envtest specs, with Go 1.27.1 and those
1.31.0 assets in a private CPU container. Its initial network-isolated run
failed because kube-apiserver could not infer an address without a default
route. A private `TEST_ASSET_KUBE_APISERVER` wrapper supplied only
`--advertise-address=127.0.0.1`; the retry passed. Both failed attempts, the
wrapper and successful command/log are retained. No source assertions, shared
service or original asset binaries changed. This proves the provided fixture
environment, not every Kubernetes generation.

Ten regressions execute the shared helper, actual Make recipes and extracted
CI run block. They cover exact config consumption, stale PATH/version
rejection, failed installation, failed/empty asset lookup, GOPATH ordering,
Kubernetes overrides, shell-quoted paths, missing-cache export without
download and Renovate ownership. No new
mirror tests of operator behavior are added.

```sh
python3 -m unittest discover -s scripts/ci/tests -p test_envtest_single_source.py
make setup-envtest
eval "$(make -s setup-envtest-env)"
go test ./cmd/vmafx-operator/internal/controller/... -v
```

Preserve the dependency-pinning distinctions from the original scan: SLSA
v2.1.0 requires reusable-workflow version tags for verifier identity;
`Dockerfile.ffmpeg` intentionally consumes a local image under ADR-1231;
Python package floors/local-source installs are not interchangeable with CI
tool locks. Those contracts, other dependencies and Scorecard thresholds are
unchanged. The actual post-change Pinned-Dependencies scan remains 7. It no
longer reports the literal `setup-envtest@latest` command, but does not
inventory the indirect variable-based helper install as a separate command.
Executable metadata and the consumer regressions establish the pin; the scan
count alone does not. No hosted Scorecard or complete RC1 acceptance is claimed.

## References

- req: user, “well fix” (Scorecard gaps).
- [Container/toolchain ownership](../adr/1231-base-image-single-source.md).
- [Operator test guide](../development/operator.md#controller-envtest-suite).
- [Upstream installer](https://github.com/kubernetes-sigs/controller-runtime/tree/e8f9455e429046cea3a1033c2e8555bcdfc8b08c/tools/setup-envtest).
- [Go install](https://pkg.go.dev/cmd/go#hdr-Compile_and_install_packages_and_dependencies).
- [Required SLSA tag identity](https://github.com/slsa-framework/slsa-github-generator/blob/v2.1.0/README.md#referencing-slsa-builders-and-generators).
- [Renovate Go datasource](https://docs.renovatebot.com/modules/datasource/go/).
