- `test_svm_parser` Meson target now links against `thread_locale.c`,
  matching the rest of the libsvm-consuming test executables.  The
  parser pulled in `vmaf_thread_locale_push_c` /
  `vmaf_thread_locale_pop` via `svm.cpp` but the target's source list
  was missing the implementation translation unit, so `meson test -C
  core/build-cpu test_svm_parser` failed to link on a clean tree.
- `vmafx-operator` audit (`cmd/vmafx-operator/`): `VmafxNode`'s
  `probeHealthz` now drains the response body before `Close`, so the
  underlying TCP connection is returned to the keep-alive pool
  instead of being torn down on every 30-second probe (Go `net/http`
  semantics).  Without the drain, polling N nodes leaked one TCP
  connection per probe per node to the controller.
- `vmafx.dev/v1` CRD integer fields (`VmafxJob.spec.priority`,
  `VmafxNode.spec.capacity`, `VmafxNode.status.assignedJobs`,
  `VmafxModelTraining.status.currentSamples`,
  `VmafxModelTraining.spec.checkpoint.minSamples`) widened from
  Go `int` to `int32` to conform to the Kubernetes API conventions
  (OpenAPI v3 has no architecture-dependent integer; `int` round-trips
  inconsistently across 32- and 64-bit architectures).
- `vmafx.dev/v1` CRDs: documented defaults
  (`VmafxJob.spec.backend=cpu`, `VmafxJob.spec.priority=0`,
  `VmafxNode.spec.capacity=1`,
  `VmafxModelTraining.spec.checkpoint.interval=10m`,
  `VmafxModelTraining.spec.checkpoint.minSamples=1000`) are now
  enforced by `kubebuilder:default` markers on the Go types and
  `default:` keys in the CRD OpenAPI schemas.  Helm CRD copies under
  `deploy/helm/vmafx/crds/` resynced to match (per
  `cmd/vmafx-operator/AGENTS.md` invariant #2).
