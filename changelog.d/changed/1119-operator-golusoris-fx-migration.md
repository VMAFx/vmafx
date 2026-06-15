- **changed(operator):** `vmafx-operator` is now composed with the golusoris
  fx framework (ADR-1119 Phase 1). The hand-rolled
  `ctrl.NewManager(...) + mgr.Start(ctrl.SetupSignalHandler())` entry point is
  replaced by `fx.New(...).Run()` over golusoris's `k8s/operator` module: fx
  owns signal handling and the run loop, golusoris `log.Module` supplies the
  structured slog logger (controller-runtime logs are bridged onto it by
  `operator.Module`), and config is read from the `operator.*` koanf subtree
  under the `VMAFX_` env prefix. Reconcile logic for `VmafxJob`, `VmafxNode`,
  and `VmafxModelTraining` is unchanged. **Env-var renames**:
  `VMAFX_OPERATOR_PROBE_ADDR` → `VMAFX_OPERATOR_HEALTH_PROBE_ADDR`,
  `VMAFX_OPERATOR_LEADER_ELECT` → `VMAFX_OPERATOR_LEADER_ELECTION`,
  `VMAFX_OPERATOR_LOG_LEVEL` → `VMAFX_LOG_LEVEL`; the boolean
  `VMAFX_OPERATOR_WEBHOOKS_ENABLED` is replaced by the integer
  `VMAFX_OPERATOR_WEBHOOK_PORT` (0 disables webhooks). CLI flags are dropped in
  favour of the 12-factor env contract. (ADR-1119)
