- **CRD generation builds again after the controller-tools v0.23.0 bump.**
  `go.sum` lacked the module hash of `sigs.k8s.io/controller-tools` v0.23.0, so
  `scripts/codegen/crd_generate.py` could not build `controller-gen` and the
  `test_crd_generated_current` and `test_crd_compat` tests failed. The hash is
  added and the four CRDs under `deploy/helm/vmafx/crds/` carry the new
  generator version.
