// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

package main

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strings"

	"github.com/VMAFx/vmafx/pkg/observability/obsgen"
)

// checkGrafana requires every generated dashboard and the three linked data
// sources in Grafana.
func checkGrafana(ctx context.Context, cfg config) error {
	auth := httpCall{user: cfg.grafanaUser, password: cfg.grafanaPassword}
	if err := waitReady(ctx, cfg.grafana+"/api/health"); err != nil {
		return fmt.Errorf("grafana: %w", err)
	}
	var found []struct {
		UID string `json:"uid"`
	}
	auth.url = cfg.grafana + "/api/search?type=dash-db&limit=5000"
	if err := getJSON(ctx, auth, &found); err != nil {
		return err
	}
	have := map[string]bool{}
	for _, d := range found {
		have[d.UID] = true
	}
	want, err := dashboardUIDs(cfg.dashboards)
	if err != nil {
		return err
	}
	for _, uid := range want {
		if !have[uid] {
			return fmt.Errorf("dashboard %s not provisioned", uid)
		}
	}
	for _, uid := range []string{obsgen.PrometheusUID, obsgen.TempoUID, obsgen.LokiUID} {
		auth.url = cfg.grafana + "/api/datasources/uid/" + uid
		var ds map[string]any
		if err := getJSON(ctx, auth, &ds); err != nil {
			return fmt.Errorf("data source %s: %w", uid, err)
		}
	}
	return nil
}

// dashboardUIDs reads the uid of every dashboard file.
func dashboardUIDs(dir string) ([]string, error) {
	files, err := filepath.Glob(filepath.Join(dir, "*.json"))
	if err != nil {
		return nil, err
	}
	var out []string
	for _, f := range files {
		raw, err := os.ReadFile(f) // #nosec G304 -- a dashboard of the mounted repository
		if err != nil {
			return nil, err
		}
		uid, ok := jsonUID(raw)
		if !ok {
			return nil, fmt.Errorf("%s: no uid", f)
		}
		out = append(out, uid)
	}
	return out, nil
}

// jsonUID finds the dashboard's top-level "uid" without decoding panels.
func jsonUID(raw []byte) (string, bool) {
	var d struct {
		UID string `json:"uid"`
	}
	if err := json.Unmarshal(raw, &d); err != nil || d.UID == "" {
		return "", false
	}
	return d.UID, true
}

// waitReady polls a readiness URL until it answers 200.
func waitReady(ctx context.Context, u string) error {
	return poll(ctx, func(ctx context.Context) (bool, error) {
		status, raw, err := httpCall{method: http.MethodGet, url: u}.do(ctx)
		if err == nil && status != http.StatusOK {
			err = fmt.Errorf("HTTP %d %s", status, strings.TrimSpace(string(raw)))
		}
		return err == nil, err
	})
}

// checkTraces requires traces of the controller and the server in Tempo.
func checkTraces(ctx context.Context, cfg config) error {
	for _, svc := range []string{"vmafx-controller", "vmafx-server"} {
		u := cfg.tempo + "/api/search?limit=5&tags=" + url.QueryEscape("service.name="+svc)
		err := poll(ctx, func(ctx context.Context) (bool, error) {
			var r struct {
				Traces []any `json:"traces"`
			}
			if err := getJSON(ctx, httpCall{url: u}, &r); err != nil {
				return false, err
			}
			return len(r.Traces) > 0, fmt.Errorf("no trace of %s yet", svc)
		})
		if err != nil {
			return fmt.Errorf("%s: %w", svc, err)
		}
	}
	return nil
}

// checkLogs requires Loki to be ready and, with -expect-logs, log records of
// a VMAFx component.
func checkLogs(ctx context.Context, cfg config) error {
	if err := waitReady(ctx, cfg.loki+"/ready"); err != nil {
		return fmt.Errorf("loki: %w", err)
	}
	if !cfg.expectLogs {
		return nil
	}
	q := url.QueryEscape(`{service_name=~"vmafx-.+"}`)
	return poll(ctx, func(ctx context.Context) (bool, error) {
		var r struct {
			Data struct {
				Result []any `json:"result"`
			} `json:"data"`
		}
		if err := getJSON(ctx, httpCall{url: cfg.loki + "/loki/api/v1/query_range?limit=5&query=" + q}, &r); err != nil {
			return false, err
		}
		return len(r.Data.Result) > 0, fmt.Errorf("no VMAFx log stream yet")
	})
}
