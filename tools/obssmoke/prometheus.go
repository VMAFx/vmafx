// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

package main

import (
	"context"
	"fmt"
	"net/url"
)

// promResult is the part of a Prometheus query answer the checks read.
type promResult struct {
	Status string `json:"status"`
	Error  string `json:"error"`
	Data   struct {
		ResultType string `json:"resultType"`
		Result     []any  `json:"result"`
	} `json:"data"`
}

// promSeries runs an instant query and returns how many series (or one for
// a scalar) it returned.
func promSeries(ctx context.Context, base, expr string) (int, error) {
	var r promResult
	u := base + "/api/v1/query?query=" + url.QueryEscape(expr)
	if err := getJSON(ctx, httpCall{url: u}, &r); err != nil {
		return 0, err
	}
	if r.Status != "success" {
		return 0, fmt.Errorf("query %q: %s", expr, r.Error)
	}
	if r.Data.ResultType == "scalar" || r.Data.ResultType == "string" {
		return 1, nil
	}
	return len(r.Data.Result), nil
}

// components are the scrape jobs of the example's VMAFx components.
var components = []string{"vmafx-server", "vmafx-controller", "vmafx-node"}

// waitTargetsUp waits until Prometheus scrapes every component and each
// serves vmafx_build_info.
func waitTargetsUp(ctx context.Context, cfg config) error {
	for _, job := range components {
		expr := fmt.Sprintf(`up{job=%q} == 1 and on (job, instance) vmafx_build_info`, job)
		err := poll(ctx, func(ctx context.Context) (bool, error) {
			n, err := promSeries(ctx, cfg.prometheus, expr)
			if err == nil && n == 0 {
				err = fmt.Errorf("%s: not up or no vmafx_build_info", job)
			}
			return n > 0, err
		})
		if err != nil {
			return fmt.Errorf("%s: %w", job, err)
		}
	}
	return nil
}

// ruleGroups is the part of /api/v1/rules the rule check reads.
type ruleGroups struct {
	Data struct {
		Groups []struct {
			Name  string `json:"name"`
			Rules []struct {
				Name      string `json:"name"`
				Health    string `json:"health"`
				LastError string `json:"lastError"`
			} `json:"rules"`
		} `json:"groups"`
	} `json:"data"`
}

// checkRules requires both generated groups, every rule evaluated without
// an error.
func checkRules(ctx context.Context, cfg config) error {
	var g ruleGroups
	err := poll(ctx, func(ctx context.Context) (bool, error) {
		if err := getJSON(ctx, httpCall{url: cfg.prometheus + "/api/v1/rules"}, &g); err != nil {
			return false, err
		}
		return ruleProblems(g) == nil, ruleProblems(g)
	})
	if err != nil {
		return err
	}
	return nil
}

// ruleProblems reports a missing group or a rule that is not healthy.
func ruleProblems(g ruleGroups) error {
	seen := map[string]int{}
	for _, grp := range g.Data.Groups {
		seen[grp.Name] = len(grp.Rules)
		for _, r := range grp.Rules {
			if r.Health != "ok" {
				return fmt.Errorf("rule %s of %s: health %q %s", r.Name, grp.Name, r.Health, r.LastError)
			}
		}
	}
	for _, want := range []string{"vmafx.recording", "vmafx.alerts"} {
		if seen[want] == 0 {
			return fmt.Errorf("rule group %s missing or empty (loaded: %v)", want, seen)
		}
	}
	return nil
}
