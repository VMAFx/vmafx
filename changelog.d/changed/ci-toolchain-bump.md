- CI toolchain brought to current: `kind` v0.23.0 → v0.33.0, `kubectl` v1.30.2 →
  v1.37.0 (1.30 is EOL), `helm` v3.15.2 → **v4.2.4**, `kuttl` v0.20.0 → v0.26.0,
  `gosec` v2.27.1 → v2.29.0, `ENVTEST_K8S_VERSION` 1.31 → 1.34, and `shfmt`
  v3.9.0 → v3.13.1 in the setup scripts (which had drifted from the v3.13.1
  already pinned in `.pre-commit-config.yaml`).
- Two CI-vs-container version skews closed: the DNN build leg pinned
  `ORT_VERSION: "1.22.0"` while the container ships 1.29.0 and `ai/` floors at
  ≥1.27.0, so CI was validating against an ONNX Runtime three minors behind what
  actually runs; and the Jimver CUDA installer pinned 13.2.0 against containers
  on 13.3.1.
- **Helm 4 is a major bump.** `--wait` semantics, OCI defaults and the removal of
  the v2 compatibility shims all change; the e2e chart templates and
  `kind-cluster.sh` are exercised by the `e2e-k8s` workflow, which is the gate
  that will surface any incompatibility.
