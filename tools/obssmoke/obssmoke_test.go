// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

package main

import (
	"errors"
	"strings"
	"testing"
)

func TestInstantiateSetsEveryVariable(t *testing.T) {
	t.Parallel()
	for in, want := range map[string]string{
		`rate(x{job=~"$job",tenant=~"${tenant}"}[$__rate_interval])`: `rate(x{job=~".*",tenant=~".*"}[10m])`,
		`increase(x[${__range}]) / $__interval_ms`:                   `increase(x[10m]) / .*`,
		`sum(up)`: `sum(up)`,
	} {
		if got := instantiate(in); got != want {
			t.Errorf("instantiate(%q) = %q, want %q", in, got, want)
		}
	}
}

// TestJudgePanels: a query without data fails unless exempted, an exempted
// query with data fails, an exemption no dashboard has fails, a query error
// fails, and a query with data or an exempted one without data passes.
func TestJudgePanels(t *testing.T) {
	key := "VMAFx Nodes and devices: panel GPU memory in use"
	if _, ok := exemptions[key]; !ok {
		t.Fatalf("test assumes the exemption %q", key)
	}
	ok := []panelResult{{key: key, data: false}, {key: "D: panel A", data: true}}
	if err := judgePanels(ok); err != nil {
		t.Errorf("judgePanels(passing) = %v", err)
	}
	for name, results := range map[string][]panelResult{
		"no data":         {{key: key}, {key: "D: panel B"}},
		"exempt has data": {{key: key, data: true}},
		"stale exemption": {{key: "D: panel A", data: true}},
		"query error":     {{key: key}, {key: "D: panel C", err: errors.New("bad expr")}},
	} {
		if err := judgePanels(results); err == nil {
			t.Errorf("%s: judgePanels passed", name)
		}
	}
}

func TestRuleProblems(t *testing.T) {
	t.Parallel()
	var g ruleGroups
	if err := ruleProblems(g); err == nil || !strings.Contains(err.Error(), "vmafx.recording") {
		t.Errorf("no groups: %v", err)
	}
	g.Data.Groups = append(g.Data.Groups,
		struct {
			Name  string `json:"name"`
			Rules []struct {
				Name      string `json:"name"`
				Health    string `json:"health"`
				LastError string `json:"lastError"`
			} `json:"rules"`
		}{Name: "vmafx.recording"},
	)
	g.Data.Groups[0].Rules = append(g.Data.Groups[0].Rules, struct {
		Name      string `json:"name"`
		Health    string `json:"health"`
		LastError string `json:"lastError"`
	}{Name: "r", Health: "ok"})
	alerts := g.Data.Groups[0]
	alerts.Name = "vmafx.alerts"
	g.Data.Groups = append(g.Data.Groups, alerts)
	if err := ruleProblems(g); err != nil {
		t.Errorf("healthy groups: %v", err)
	}
	g.Data.Groups[1].Rules = append(g.Data.Groups[1].Rules[:0:0], g.Data.Groups[1].Rules[0])
	g.Data.Groups[1].Rules[0].Health = "err"
	if err := ruleProblems(g); err == nil {
		t.Error("an unhealthy rule passed")
	}
}

// TestSendFramesRefusesUnevenFrameLists: a reference list and a distorted list
// of different lengths are an error before anything is sent (a nil stream would
// panic on the first Send), where the pairing loop would index past the shorter.
func TestSendFramesRefusesUnevenFrameLists(t *testing.T) {
	t.Parallel()
	err := sendFrames(nil, [][]byte{{1}, {2}}, [][]byte{{1}})
	if err == nil || !strings.Contains(err.Error(), "2 reference frames, 1 distorted frames") {
		t.Errorf("sendFrames with uneven lists = %v", err)
	}
}
