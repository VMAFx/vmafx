// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

// Command obssmoke is the smoke test of the Compose observability example
// (deploy/compose/observability, ADR-2349). It runs inside the example's
// network, sends traffic to every VMAFx component (Score requests that pass
// and fail, a ScoreStream session, controller jobs that complete, fail and are
// cancelled), then checks that Prometheus scrapes every component and loaded
// the rendered rules without errors, that every Prometheus query of every
// shipped dashboard returns data, that Grafana provisioned the dashboards and
// data sources, and that the traces reached Tempo. A query that cannot return
// data in this stack is listed in exemptions.go with its reason; an exempted
// query that does return data fails too, so the list cannot go stale.
//
// scripts/ci/observability-compose-smoke.sh starts the stack and runs it:
//
//	docker compose --profile smoke run --rm smoke
package main

import (
	"context"
	"flag"
	"fmt"
	"log/slog"
	"os"
	"time"
)

// config names the services of the example, as seen from its network.
type config struct {
	serverHTTP, serverGRPC, controllerGRPC string
	prometheus, grafana, tempo, loki       string
	grafanaUser, grafanaPassword           string
	media, dashboards                      string
	expectLogs                             bool
}

func parseFlags() (config, time.Duration) {
	var c config
	flag.StringVar(&c.serverHTTP, "server-http", "http://vmafx-server:8080", "vmafx-server HTTP base URL")
	flag.StringVar(&c.serverGRPC, "server-grpc", "vmafx-server:9090", "vmafx-server gRPC address")
	flag.StringVar(&c.controllerGRPC, "controller-grpc", "vmafx-controller:9090", "vmafx-controller gRPC address")
	flag.StringVar(&c.prometheus, "prometheus", "http://prometheus:9090", "Prometheus base URL")
	flag.StringVar(&c.grafana, "grafana", "http://grafana:3000", "Grafana base URL")
	flag.StringVar(&c.tempo, "tempo", "http://tempo:3200", "Tempo base URL")
	flag.StringVar(&c.loki, "loki", "http://loki:3100", "Loki base URL")
	flag.StringVar(&c.grafanaUser, "grafana-user", "admin", "Grafana user")
	flag.StringVar(&c.grafanaPassword, "grafana-password", os.Getenv("GRAFANA_ADMIN_PASSWORD"), "Grafana password")
	flag.StringVar(&c.media, "media", "/media", "the media directory the components share, holding ref.y4m, dis.y4m, ref.yuv, dis.yuv")
	flag.StringVar(&c.dashboards, "dashboards", "/src/deploy/grafana/dashboards", "the generated dashboards")
	flag.BoolVar(&c.expectLogs, "expect-logs", false, "require VMAFx log records in Loki")
	timeout := flag.Duration("timeout", 15*time.Minute, "overall deadline")
	flag.Parse()
	if c.grafanaPassword == "" {
		c.grafanaPassword = "admin"
	}
	return c, *timeout
}

func main() {
	cfg, timeout := parseFlags()
	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	err := run(ctx, cfg)
	cancel()
	if err != nil {
		fmt.Fprintln(os.Stderr, "obssmoke:", err)
		os.Exit(1)
	}
}

// step is one check; a required step that fails ends the run.
type step struct {
	name     string
	required bool
	fn       func(context.Context, config) error
}

func run(ctx context.Context, cfg config) error {
	steps := []step{
		{"every component is scraped", true, waitTargetsUp},
		{"traffic", true, generateTraffic},
		{"scrapes after the traffic", true, waitScrapes},
		{"rules loaded and healthy", false, checkRules},
		{"every dashboard query returns data", false, checkPanels},
		{"Grafana provisioned the dashboards and data sources", false, checkGrafana},
		{"traces reached Tempo", false, checkTraces},
		{"logs reached Loki", false, checkLogs},
	}
	var failed []string
	for _, s := range steps {
		start := time.Now()
		err := s.fn(ctx, cfg)
		if err == nil {
			slog.Info("PASS", "step", s.name, "took", time.Since(start).Round(time.Millisecond))
			continue
		}
		slog.Error("FAIL", "step", s.name, "err", err)
		failed = append(failed, s.name)
		if s.required {
			break
		}
	}
	if len(failed) > 0 {
		return fmt.Errorf("%d step(s) failed: %v", len(failed), failed)
	}
	return nil
}
