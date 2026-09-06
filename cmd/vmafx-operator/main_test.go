// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-operator/main_test.go — fx graph validation for the operator
// composition (ADR-1119 Phase 1).
//
// These tests assert the fx dependency graph is internally consistent — every
// constructor's inputs are provided and there are no cycles — WITHOUT starting
// a controller-runtime manager (which would need a live API server / envtest).
// fx.ValidateApp resolves the graph but does not invoke constructors, so
// operator.Module's newManager (which calls ctrl.GetConfig + manager.New) never
// runs here. The full start/stop path is exercised by the controller envtest
// suite under internal/controller/.

package main

import (
	"testing"

	"github.com/golusoris/golusoris/config"
	"github.com/golusoris/golusoris/k8s/operator"
	"github.com/golusoris/golusoris/otel"
	"go.uber.org/fx"

	"github.com/VMAFx/vmafx/internal/app/bootstrap"
	"github.com/VMAFx/vmafx/internal/oteltest"
	buildversion "github.com/VMAFx/vmafx/pkg/version"
)

func TestVersionRequest(t *testing.T) {
	t.Parallel()
	if !isVersionRequest([]string{"vmafx-operator", "--version"}) {
		t.Fatal("--version must select the non-blocking version path")
	}
	if isVersionRequest([]string{"vmafx-operator"}) {
		t.Fatal("normal startup must not select the version path")
	}
	if isVersionRequest([]string{"vmafx-operator", "--version", "extra"}) {
		t.Fatal("version path must reject extra arguments")
	}
}

// TestOptionsGraphValidates asserts the production fx option list resolves into
// a consistent dependency graph: golusoris.Core (config + slog + clock + id),
// otel.Module, operator.Module (manager.Manager + Options), the CRD scheme
// adder, and both fx.Invoke wiring functions (reconcilers + webhooks) all have
// their dependencies satisfied.
func TestOptionsGraphValidates(t *testing.T) {
	t.Parallel()
	if err := fx.ValidateApp(options()...); err != nil {
		t.Fatalf("operator fx graph does not validate: %v", err)
	}
}

// TestEnvOptionsContract pins the VMAFX_ env contract: the prefix, delimiter,
// and the compound-key set that keeps operator.Options' leaf underscores intact
// through golusoris's env→koanf transform. If golusoris adds an operator option
// (a new koanf leaf with an underscore), this test reminds us to register it as
// a compound key here too, otherwise its env var silently won't bind.
func TestEnvOptionsContract(t *testing.T) {
	t.Parallel()
	opts := operatorEnvOptions()
	if opts.EnvPrefix != "VMAFX_" {
		t.Fatalf("EnvPrefix = %q, want %q", opts.EnvPrefix, "VMAFX_")
	}
	if opts.Delimiter != "." {
		t.Fatalf("Delimiter = %q, want %q", opts.Delimiter, ".")
	}
	wantCompound := map[string]bool{
		"operator.metrics_addr":       true,
		"operator.health_probe_addr":  true,
		"operator.leader_election":    true,
		"operator.leader_election_id": true,
		"operator.graceful_shutdown":  true,
		"operator.webhook_port":       true,
		"operator.webhook_host":       true,
	}
	got := make(map[string]bool, len(opts.CompoundKeys))
	for _, k := range opts.CompoundKeys {
		got[k] = true
	}
	for k := range wantCompound {
		if !got[k] {
			t.Errorf("missing compound key %q (its VMAFX_ env var would not bind)", k)
		}
	}
	for k := range got {
		if !wantCompound[k] {
			t.Errorf("unexpected compound key %q", k)
		}
	}
}

// TestEnvOptionsBindOperatorKeys is an end-to-end check that the VMAFX_ env
// vars actually resolve onto operator.Options through a real config.Config
// built with operatorEnvOptions(). This is the regression guard for the env
// gotcha: without the compound keys, VMAFX_OPERATOR_METRICS_ADDR would map to
// "operator.metrics.addr" and operator.Options.MetricsAddr would stay empty.
func TestEnvOptionsBindOperatorKeys(t *testing.T) {
	t.Setenv("VMAFX_OPERATOR_METRICS_ADDR", ":19090")
	t.Setenv("VMAFX_OPERATOR_HEALTH_PROBE_ADDR", ":19091")
	t.Setenv("VMAFX_OPERATOR_LEADER_ELECTION", "true")
	t.Setenv("VMAFX_OPERATOR_LEADER_ELECTION_ID", "test-lease")
	t.Setenv("VMAFX_OPERATOR_WEBHOOK_PORT", "9443")

	cfg, err := config.New(operatorEnvOptions())
	if err != nil {
		t.Fatalf("config.New: %v", err)
	}
	var opts operator.Options
	if err := cfg.Unmarshal("operator", &opts); err != nil {
		t.Fatalf("unmarshal operator options: %v", err)
	}
	if opts.MetricsAddr != ":19090" {
		t.Errorf("MetricsAddr = %q, want %q", opts.MetricsAddr, ":19090")
	}
	if opts.HealthProbeAddr != ":19091" {
		t.Errorf("HealthProbeAddr = %q, want %q", opts.HealthProbeAddr, ":19091")
	}
	if !opts.LeaderElection {
		t.Errorf("LeaderElection = false, want true")
	}
	if opts.LeaderElectionID != "test-lease" {
		t.Errorf("LeaderElectionID = %q, want %q", opts.LeaderElectionID, "test-lease")
	}
	// golusoris v0.5.0's operator.Options carries WebhookPort (golusoris#227),
	// so the compound key binds straight onto the field through the env
	// transform — registerWebhooks gates on opts.WebhookPort directly.
	if opts.WebhookPort != 9443 {
		t.Errorf("opts.WebhookPort = %d, want %d", opts.WebhookPort, 9443)
	}
}

// TestWithOperatorDefaults verifies the app-level default backfill supplies a
// stable leader-election lease name when none is configured, passes the
// metrics / health-probe addresses through untouched (golusoris operator.Module
// owns their defaults), and preserves any explicitly-configured value.
func TestWithOperatorDefaults(t *testing.T) {
	t.Parallel()
	// Empty lease ID gets the stable vmafx default; ports pass through unchanged.
	got := withOperatorDefaults(operator.Options{MetricsAddr: ":8080", HealthProbeAddr: ":8081"})
	if got.LeaderElectionID != defaultLeaderElectionID {
		t.Errorf("LeaderElectionID = %q, want %q", got.LeaderElectionID, defaultLeaderElectionID)
	}
	if got.MetricsAddr != ":8080" || got.HealthProbeAddr != ":8081" {
		t.Errorf("ports must pass through untouched, got %q / %q", got.MetricsAddr, got.HealthProbeAddr)
	}
	// Explicit values survive.
	explicit := withOperatorDefaults(operator.Options{
		MetricsAddr:      ":7000",
		HealthProbeAddr:  ":7001",
		LeaderElectionID: "custom",
	})
	if explicit.MetricsAddr != ":7000" || explicit.HealthProbeAddr != ":7001" || explicit.LeaderElectionID != "custom" {
		t.Errorf("explicit values not preserved: %+v", explicit)
	}
}

// TestOTelWiredThroughBootstrap proves the operator's composition inherits
// OpenTelemetry from bootstrap.Base (ADR-0782 / ADR-1119) with the operator's
// own VMAFX_ env contract: without an OTLP endpoint golusoris's otel.Module
// yields no-op providers, and the resource identity is service.name
// "vmafx-operator" (derived from the binary) with service.version from
// pkg/version. operator.Module is deliberately left out — its manager
// constructor needs a cluster; TestOptionsGraphValidates covers that Base is
// part of the production option list.
func TestOTelWiredThroughBootstrap(t *testing.T) {
	oteltest.NoopEnv(t)
	t.Setenv("VMAFX_LOG_LEVEL", "error")
	t.Setenv("OTEL_SERVICE_NAME", "")
	t.Setenv("VMAFX_OTEL_SERVICE_NAME", "")
	t.Setenv("VMAFX_OTEL_SERVICE_VERSION", "")

	var (
		providers *otel.Providers
		opts      otel.Options
	)
	app := fx.New(
		bootstrap.Base,
		fx.Replace(operatorEnvOptions()),
		fx.NopLogger,
		fx.Populate(&providers, &opts),
	)
	if err := app.Err(); err != nil {
		t.Fatalf("bootstrap graph with operator env options: %v", err)
	}
	if providers == nil || providers.Tracer != nil || providers.Meter != nil || providers.Logger != nil {
		t.Fatalf("expected no-op OTel providers without an endpoint, got %+v", providers)
	}
	if opts.Service.Name != "vmafx-operator" {
		t.Errorf("service.name = %q, want vmafx-operator", opts.Service.Name)
	}
	if opts.Service.Version != buildversion.Version() {
		t.Errorf("service.version = %q, want %q", opts.Service.Version, buildversion.Version())
	}
}
