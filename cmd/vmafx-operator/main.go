// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-operator/main.go — vmafx-operator entry point.
//
// The operator watches VmafxJob, VmafxNode, and VmafxModelTraining CRDs and
// reconciles Pods + status subresources.  It is built with kubebuilder v4 /
// controller-runtime v0.19+ and runs as a Kubernetes Deployment.
//
// 12-factor environment variables:
//
//	VMAFX_OPERATOR_METRICS_ADDR       Prometheus metrics bind address (default ":8081").
//	VMAFX_OPERATOR_PROBE_ADDR         Health probe bind address (default ":8082").
//	VMAFX_OPERATOR_LEADER_ELECT       Enable leader election ("true"/"false", default "false").
//	VMAFX_OPERATOR_LOG_LEVEL          slog level string (default "info").
//	VMAFX_OPERATOR_WEBHOOKS_ENABLED   Enable admission webhooks ("true"/"false", default "false").
//	VMAFX_CONTROLLER_GRPC_ADDR        gRPC address of the vmafx-controller service.
//	VMAFX_CONTROLLER_HTTP_ADDR        HTTP address of the vmafx-controller service (for /healthz).
//
// CLI flags mirror and override the environment variables.
//
// ADR-0786: vmafx-operator Stage 2 — reconciler loops + webhook + per-controller RBAC.
// ADR-0714: vmafx-operator kubebuilder skeleton + CRDs.
// ADR-0709: VMAFX Phase 4b distributed platform (parent).
// ADR-0711: vmafx-controller impl (sibling service).

package main

import (
	"flag"
	"os"
	"strings"

	"go.uber.org/zap/zapcore"
	"k8s.io/apimachinery/pkg/runtime"
	utilruntime "k8s.io/apimachinery/pkg/util/runtime"
	clientgoscheme "k8s.io/client-go/kubernetes/scheme"
	ctrl "sigs.k8s.io/controller-runtime"
	"sigs.k8s.io/controller-runtime/pkg/healthz"
	"sigs.k8s.io/controller-runtime/pkg/log/zap"
	metricsserver "sigs.k8s.io/controller-runtime/pkg/metrics/server"

	vmafxv1 "github.com/VMAFx/vmafx/api/vmafx/v1"
	"github.com/VMAFx/vmafx/cmd/vmafx-operator/internal/controller"
	"github.com/VMAFx/vmafx/cmd/vmafx-operator/internal/webhook"
)

var scheme = runtime.NewScheme()

func init() {
	utilruntime.Must(clientgoscheme.AddToScheme(scheme))
	utilruntime.Must(vmafxv1.AddToScheme(scheme))
}

func envOr(key, def string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return def
}

func main() {
	metricsAddr := flag.String("metrics-bind-address",
		envOr("VMAFX_OPERATOR_METRICS_ADDR", ":8081"),
		"Address for Prometheus metrics endpoint")
	probeAddr := flag.String("health-probe-bind-address",
		envOr("VMAFX_OPERATOR_PROBE_ADDR", ":8082"),
		"Address for health probe endpoints")
	leaderElect := flag.Bool("leader-elect",
		envOr("VMAFX_OPERATOR_LEADER_ELECT", "false") == "true",
		"Enable leader election for high-availability deployments")
	logLevel := flag.String("log-level",
		envOr("VMAFX_OPERATOR_LOG_LEVEL", "info"),
		"Log level: debug|info|warn|error")
	webhooksEnabled := flag.Bool("webhooks-enabled",
		envOr("VMAFX_OPERATOR_WEBHOOKS_ENABLED", "false") == "true",
		"Enable admission webhooks (requires cert-manager or manual TLS secret)")
	flag.Parse()

	// ---------------------------------------------------------------------------
	// Logger — zap (controller-runtime standard).
	// ---------------------------------------------------------------------------
	level := zapcore.InfoLevel
	switch strings.ToLower(*logLevel) {
	case "debug":
		level = zapcore.DebugLevel
	case "warn":
		level = zapcore.WarnLevel
	case "error":
		level = zapcore.ErrorLevel
	}

	zapOpts := zap.Options{
		Development: level == zapcore.DebugLevel,
		Level:       level,
	}
	ctrl.SetLogger(zap.New(zap.UseFlagOptions(&zapOpts)))
	setupLog := ctrl.Log.WithName("setup")

	// ---------------------------------------------------------------------------
	// Manager — owns the scheme, cache, and all reconciler goroutines.
	// ---------------------------------------------------------------------------
	mgr, err := ctrl.NewManager(ctrl.GetConfigOrDie(), ctrl.Options{
		Scheme: scheme,
		Metrics: metricsserver.Options{
			BindAddress: *metricsAddr,
		},
		HealthProbeBindAddress: *probeAddr,
		LeaderElection:         *leaderElect,
		LeaderElectionID:       "vmafx-operator.vmafx.dev",
	})
	if err != nil {
		setupLog.Error(err, "Unable to start manager")
		os.Exit(1)
	}

	// ---------------------------------------------------------------------------
	// Register reconcilers.
	// ---------------------------------------------------------------------------
	if err := (&controller.VmafxJobReconciler{
		Client: mgr.GetClient(),
		Scheme: mgr.GetScheme(),
	}).SetupWithManager(mgr); err != nil {
		setupLog.Error(err, "Unable to create VmafxJob controller")
		os.Exit(1)
	}

	if err := (&controller.VmafxNodeReconciler{
		Client: mgr.GetClient(),
		Scheme: mgr.GetScheme(),
	}).SetupWithManager(mgr); err != nil {
		setupLog.Error(err, "Unable to create VmafxNode controller")
		os.Exit(1)
	}

	if err := (&controller.VmafxModelTrainingReconciler{
		Client:   mgr.GetClient(),
		Scheme:   mgr.GetScheme(),
		Recorder: mgr.GetEventRecorderFor("vmafx-model-training-controller"),
	}).SetupWithManager(mgr); err != nil {
		setupLog.Error(err, "Unable to create VmafxModelTraining controller")
		os.Exit(1)
	}

	// ---------------------------------------------------------------------------
	// Admission webhooks (opt-in via --webhooks-enabled).
	// ---------------------------------------------------------------------------
	if *webhooksEnabled {
		if err := (&webhook.VmafxJobValidator{}).SetupWebhookWithManager(mgr); err != nil {
			setupLog.Error(err, "Unable to register VmafxJob validation webhook")
			os.Exit(1)
		}
		if err := (&webhook.VmafxNodeValidator{}).SetupWebhookWithManager(mgr); err != nil {
			setupLog.Error(err, "Unable to register VmafxNode validation webhook")
			os.Exit(1)
		}
		setupLog.Info("Admission webhooks registered")
	}

	// ---------------------------------------------------------------------------
	// Health probes.
	// ---------------------------------------------------------------------------
	if err := mgr.AddHealthzCheck("healthz", healthz.Ping); err != nil {
		setupLog.Error(err, "Unable to register healthz check")
		os.Exit(1)
	}
	if err := mgr.AddReadyzCheck("readyz", healthz.Ping); err != nil {
		setupLog.Error(err, "Unable to register readyz check")
		os.Exit(1)
	}

	// ---------------------------------------------------------------------------
	// Start.
	// ---------------------------------------------------------------------------
	setupLog.Info("Starting vmafx-operator",
		"version", buildVersion,
		"metricsAddr", *metricsAddr,
		"probeAddr", *probeAddr,
		"leaderElect", *leaderElect,
	)

	if err := mgr.Start(ctrl.SetupSignalHandler()); err != nil {
		setupLog.Error(err, "Manager exited with error")
		os.Exit(1)
	}
}

// buildVersion is populated by ldflags: -ldflags "-X main.buildVersion=v1.2.3".
var buildVersion = "dev"
