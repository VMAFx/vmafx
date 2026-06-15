// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-operator/main.go — vmafx-operator entry point (golusoris fx).
//
// The operator watches VmafxJob, VmafxNode, and VmafxModelTraining CRDs and
// reconciles Pods + status subresources.  It is built with kubebuilder v4 /
// controller-runtime v0.24+ and runs as a Kubernetes Deployment.
//
// ADR-1119 Phase 1: this binary is composed with the golusoris fx framework.
// golusoris.Core supplies config (koanf) + structured slog; otel.Module wires
// OpenTelemetry; operator.Module (golusoris/k8s/operator) resolves the
// rest.Config, builds the controller-runtime manager, registers the default
// healthz/readyz "ping" probes, supports leader election, and runs the manager
// under the fx lifecycle.  (The v0.4.0 tag does not call ctrl.SetLogger —
// golusoris#227 is untagged — so setupCtrlLogger bridges it here.)  fx owns signal
// handling and the run loop; the binary no longer calls
// ctrl.SetupSignalHandler() or mgr.Start() itself.
//
// 12-factor environment variables (VMAFX_ prefix, koanf "operator" subtree;
// every leaf is a config.Options CompoundKey so the leaf underscores survive
// the env→koanf transform):
//
//	VMAFX_OPERATOR_METRICS_ADDR         Prometheus metrics bind address (golusoris default ":8080").
//	VMAFX_OPERATOR_HEALTH_PROBE_ADDR    Health probe bind address (golusoris default ":8081").
//	VMAFX_OPERATOR_LEADER_ELECTION      Enable leader election ("true"/"false", default "false").
//	VMAFX_OPERATOR_LEADER_ELECTION_ID   Lease name (default "vmafx-operator.vmafx.dev").
//	VMAFX_OPERATOR_WEBHOOK_PORT         Admission-webhook port; 0 disables webhooks (default 0).
//	VMAFX_OPERATOR_WEBHOOK_HOST         Admission-webhook bind host (default all interfaces).
//	VMAFX_LOG_LEVEL                     slog level: debug|info|warn|error (golusoris log module).
//	VMAFX_CONTROLLER_GRPC_ADDR          gRPC address of the vmafx-controller service.
//	VMAFX_CONTROLLER_HTTP_ADDR          HTTP address of the vmafx-controller service (for /healthz).
//
// Migration note: VMAFX_OPERATOR_PROBE_ADDR → VMAFX_OPERATOR_HEALTH_PROBE_ADDR,
// VMAFX_OPERATOR_LEADER_ELECT → VMAFX_OPERATOR_LEADER_ELECTION, and the boolean
// VMAFX_OPERATOR_WEBHOOKS_ENABLED is replaced by the integer
// VMAFX_OPERATOR_WEBHOOK_PORT (0 = disabled).  See docs/development/operator.md.
//
// ADR-0786: vmafx-operator Stage 2 — reconciler loops + webhook + per-controller RBAC.
// ADR-0714: vmafx-operator kubebuilder skeleton + CRDs.
// ADR-0709: VMAFX Phase 4b distributed platform (parent).
// ADR-0711: vmafx-controller impl (sibling service).

package main

import (
	"log/slog"
	"os"

	"github.com/go-logr/logr"
	"github.com/golusoris/golusoris/config"
	"github.com/golusoris/golusoris/k8s/operator"
	"go.uber.org/fx"
	ctrl "sigs.k8s.io/controller-runtime"
	"sigs.k8s.io/controller-runtime/pkg/manager"

	vmafxv1 "github.com/VMAFx/vmafx/api/vmafx/v1"
	"github.com/VMAFx/vmafx/cmd/vmafx-operator/internal/controller"
	"github.com/VMAFx/vmafx/cmd/vmafx-operator/internal/webhook"
	"github.com/VMAFx/vmafx/internal/app/bootstrap"
)

// defaultLeaderElectionID is the controller-runtime lease name used when leader
// election is enabled and no explicit ID is configured — a stable
// vmafx-specific lease (golusoris does not default LeaderElectionID).
const defaultLeaderElectionID = "vmafx-operator.vmafx.dev"

// operatorEnvOptions returns the config.Options that pin the VMAFX_ env
// contract for this binary.
//
// golusoris's env transform strips the prefix, lowercases, and splits EVERY
// underscore on the delimiter, so VMAFX_OPERATOR_METRICS_ADDR would otherwise
// map to "operator.metrics.addr" — but operator.Options reads the compound leaf
// key "operator.metrics_addr". Declaring each operator leaf as a CompoundKey
// keeps its underscores intact through the transform.
func operatorEnvOptions() config.Options {
	return config.Options{
		EnvPrefix: "VMAFX_",
		Delimiter: ".",
		CompoundKeys: []string{
			"operator.metrics_addr",
			"operator.health_probe_addr",
			"operator.leader_election",
			"operator.leader_election_id",
			"operator.graceful_shutdown",
			"operator.webhook_port",
			"operator.webhook_host",
		},
	}
}

// withOperatorDefaults backfills the leader-election lease name. The metrics /
// health-probe bind addresses use golusoris operator.Module's native defaults
// (:8080 / :8081). The lease name is functionally required — controller-runtime
// leader election fails with an empty LeaderElectionID and golusoris does not
// default it — so we supply a stable vmafx-specific lease.
func withOperatorDefaults(o operator.Options) operator.Options {
	if o.LeaderElectionID == "" {
		o.LeaderElectionID = defaultLeaderElectionID
	}
	return o
}

// registerReconcilers wires every Stage 2 reconciler against the manager. Only
// the Setup mechanism changed in the fx migration; the Reconcile logic is
// untouched (ADR-1119). The embedded client.Client field name is "Client".
func registerReconcilers(mgr manager.Manager) error {
	if err := (&controller.VmafxJobReconciler{
		Client: mgr.GetClient(),
		Scheme: mgr.GetScheme(),
	}).SetupWithManager(mgr); err != nil {
		return err
	}
	if err := (&controller.VmafxNodeReconciler{
		Client: mgr.GetClient(),
		Scheme: mgr.GetScheme(),
	}).SetupWithManager(mgr); err != nil {
		return err
	}
	if err := (&controller.VmafxModelTrainingReconciler{
		Client:   mgr.GetClient(),
		Scheme:   mgr.GetScheme(),
		Recorder: mgr.GetEventRecorderFor("vmafx-model-training-controller"),
	}).SetupWithManager(mgr); err != nil {
		return err
	}
	return nil
}

// registerWebhooks wires the admission webhooks when enabled via config
// (VMAFX_OPERATOR_WEBHOOK_PORT > 0). NOTE: golusoris v0.4.0's operator.Options
// does not yet expose a webhook bind port — the WebhookPort/WebhookHost fields
// are merged to golusoris main but untagged (golusoris#227) — so the webhook
// server uses controller-runtime's default bind (:9443) and the configured port
// acts as a feature gate only. Once the next golusoris tag lands the field,
// thread it through operator.Options instead.
func registerWebhooks(cfg *config.Config, mgr manager.Manager) error {
	if cfg.Int("operator.webhook_port") <= 0 {
		return nil
	}
	if err := (&webhook.VmafxJobValidator{}).SetupWebhookWithManager(mgr); err != nil {
		return err
	}
	if err := (&webhook.VmafxNodeValidator{}).SetupWebhookWithManager(mgr); err != nil {
		return err
	}
	return nil
}

// setupCtrlLogger bridges controller-runtime's global logr sink onto the
// golusoris *slog.Logger. golusoris v0.4.0's operator.Module does not call
// ctrl.SetLogger (the bridge is merged to golusoris main but untagged —
// golusoris#227), so without this shim controller-runtime's own logs bypass the
// application log stream and its OTel correlation. Remove once the framework
// calls SetLogger itself.
func setupCtrlLogger(l *slog.Logger) {
	ctrl.SetLogger(logr.FromSlogHandler(l.Handler()))
}

// app builds the fx application graph. It is a package-level function so tests
// can fx.ValidateApp the exact same composition the binary runs.
func app() *fx.App {
	return fx.New(options()...)
}

// options is the ordered fx option list for the operator. Shared with the
// validation test.
func options() []fx.Option {
	return []fx.Option{
		bootstrap.Base, // golusoris.Core + otel.Module + version
		fx.Replace(operatorEnvOptions()),
		operator.Module,
		operator.ProvideScheme(vmafxv1.AddToScheme),
		fx.Decorate(withOperatorDefaults),
		bootstrap.FxLogger(),
		fx.Invoke(setupCtrlLogger),
		fx.Invoke(registerReconcilers),
		fx.Invoke(registerWebhooks),
	}
}

func main() {
	// golusoris#234: the v0.4.0 log module reads bare LOG_LEVEL/LOG_FORMAT and
	// ignores the VMAFX_ prefix (the config-prefixed read is merged to golusoris
	// main but untagged). Bridge the prefixed vars before fx.New; remove once
	// the carrying golusoris tag lands.
	if v := os.Getenv("VMAFX_LOG_LEVEL"); v != "" && os.Getenv("LOG_LEVEL") == "" {
		_ = os.Setenv("LOG_LEVEL", v)
	}
	if v := os.Getenv("VMAFX_LOG_FORMAT"); v != "" && os.Getenv("LOG_FORMAT") == "" {
		_ = os.Setenv("LOG_FORMAT", v)
	}
	app().Run()
}
