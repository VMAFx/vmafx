- vmafx-operator envtest suite (`cmd/vmafx-operator/internal/controller`):
  PRs #330, #341, and #362 all noted the suite panicking in `BeforeSuite` with
  a nil-pointer deref from `controlplane.(*APIServer).Stop` because the
  kubebuilder envtest control-plane binaries (etcd + kube-apiserver + kubectl)
  were not on `PATH`. Fixed three ways: (1) new `make setup-envtest` target
  installs `sigs.k8s.io/controller-runtime/tools/setup-envtest@latest` and
  downloads the v1.31 control-plane bundle; (2) `.github/workflows/go-ci.yml`
  installs setup-envtest + exports `KUBEBUILDER_ASSETS` before `go test ./...`
  so the operator suite runs for real in CI; (3) `suite_test.go` now
  `t.Skip()`s with an actionable message when `KUBEBUILDER_ASSETS` is unset
  (and `AfterSuite` guards the nil `testEnv` deref) as defense in depth for
  ad-hoc `go test` invocations from a fresh checkout. Operator suite goes from
  hard-fail to 3/3 green locally with the new target.
