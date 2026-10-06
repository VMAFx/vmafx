// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-server/logfields_test.go — POST /v1/score logs carry the shared
// structured-log field set (issue #1251).

//go:build cgo

package main

import (
	"bytes"
	"encoding/json"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/prometheus/client_golang/prometheus"

	"github.com/VMAFx/vmafx/pkg/libvmaf"
	"github.com/VMAFx/vmafx/pkg/observability"
)

func TestHTTPScoreLogsCarrySharedFields(t *testing.T) {
	stub := writeVmafStub(t, vmafGoldenJSON)
	scorer, err := libvmaf.New(stub, writeModelFile(t))
	if err != nil {
		t.Fatalf("libvmaf.New: %v", err)
	}
	var buf bytes.Buffer
	log := slog.New(slog.NewJSONHandler(&buf, nil))
	reg := prometheus.NewRegistry()
	hs := newHTTPServer(scorer, observability.NewMetrics(reg), reg, log, nil)

	req := httptest.NewRequest(http.MethodPost, "/v1/score",
		strings.NewReader(`{"reference":"/r.yuv","distorted":"/d.yuv","model":"vmaf_v0.6.1"}`))
	rr := httptest.NewRecorder()
	hs.handleScore(rr, req)
	if rr.Code != http.StatusOK {
		t.Fatalf("POST /v1/score = %d (%s), want 200", rr.Code, rr.Body.String())
	}

	var line map[string]any
	for _, l := range strings.Split(strings.TrimSpace(buf.String()), "\n") {
		var m map[string]any
		if err := json.Unmarshal([]byte(l), &m); err == nil && m["msg"] == "http Score completed" {
			line = m
		}
	}
	if line == nil {
		t.Fatalf("no 'http Score completed' line in %q", buf.String())
	}
	for key, want := range map[string]any{
		observability.FieldRoute: "POST /v1/score",
		observability.FieldModel: "vmaf_v0.6.1",
	} {
		if line[key] != want {
			t.Errorf("%s = %v, want %v", key, line[key], want)
		}
	}
	for _, key := range []string{observability.FieldRequestID, observability.FieldDurationS} {
		if _, ok := line[key]; !ok {
			t.Errorf("log line lacks %q: %v", key, line)
		}
	}
}
