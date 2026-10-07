// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/scoring_scope_test.go — a writer scores only inputs
// under its tenant's scoring roots (ADR-1577).
//
// Positive: inputs under the tenant's root are admitted (Score resolves them
// to real paths, SubmitJob keeps them, PullWork hands the roots to the node).
// Negative: another tenant's directory, a ".." traversal and a symlink out of
// the root are refused with PermissionDenied (403 over HTTP), and a tenant
// without roots scores nothing. Boundary: the configuration refuses roots
// next to a tenant registry, a malformed root and a tenant ID that would
// reshape a root.

//go:build cgo

package main

import (
	"bytes"
	"context"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"testing"
	"time"

	"github.com/golusoris/golusoris/core/config"
	"github.com/prometheus/client_golang/prometheus"
	"google.golang.org/grpc/codes"

	"github.com/VMAFx/vmafx/cmd/vmafx-controller/auth"
	vmafxv1 "github.com/VMAFx/vmafx/gen/go"
	controllerv1 "github.com/VMAFx/vmafx/gen/go/controller"
	"github.com/VMAFx/vmafx/internal/oteltest"
)

// allowAllScopes admits every local path, for tests about other behaviour.
func allowAllScopes() *scoringScopes {
	return &scoringScopes{template: []string{"/"}}
}

// mediaTree builds <dir>/acme/ref.y4m, <dir>/rival/secret.y4m and the link
// <dir>/acme/steal.y4m -> ../rival/secret.y4m, and returns dir.
func mediaTree(t *testing.T) string {
	t.Helper()
	dir, err := filepath.EvalSymlinks(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	for _, p := range []string{"acme/ref.y4m", "rival/secret.y4m"} {
		full := filepath.Join(dir, p)
		if err := os.MkdirAll(filepath.Dir(full), 0o700); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(full, []byte("YUV4MPEG2"), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	if err := os.Symlink(filepath.Join(dir, "rival", "secret.y4m"), filepath.Join(dir, "acme", "steal.y4m")); err != nil {
		t.Fatal(err)
	}
	return dir
}

// scoringInputFiles creates two empty inputs for handler tests whose scorer
// is a stub; Score resolves its inputs, so they must exist (ADR-1577).
func scoringInputFiles(t *testing.T) (string, string) {
	t.Helper()
	dir := t.TempDir()
	ref, dis := filepath.Join(dir, "ref.yuv"), filepath.Join(dir, "dis.yuv")
	for _, p := range []string{ref, dis} {
		if err := os.WriteFile(p, nil, 0o600); err != nil {
			t.Fatal(err)
		}
	}
	return ref, dis
}

// tenantScopes gives every tenant <dir>/<tenant>.
func tenantScopes(dir string) *scoringScopes {
	return &scoringScopes{template: []string{filepath.Join(dir, tenantPlaceholder)}}
}

func TestScoringScopesConfiguration(t *testing.T) {
	cfgWith := func(roots string) *config.Config {
		t.Helper()
		t.Setenv("VMAFX_SCORING_ROOTS", roots)
		cfg, err := config.New(controllerEnvOptions())
		if err != nil {
			t.Fatalf("config.New: %v", err)
		}
		return cfg
	}
	s, err := provideScoringScopes(cfgWith("/data/{tenant}, s3:bucket/{tenant}"), nil)
	if err != nil {
		t.Fatalf("valid roots refused: %v", err)
	}
	roots, err := s.For("acme")
	if err != nil || !slices.Equal(roots.Strings(), []string{"/data/acme", "s3:bucket/acme"}) {
		t.Fatalf("roots of acme = %v, %v", roots.Strings(), err)
	}
	for _, id := range []string{"../rival", "a/b", "x:y", ".."} {
		if _, err := s.For(id); err == nil {
			t.Errorf("tenant ID %q was substituted into a root", id)
		}
	}
	if _, err := provideScoringScopes(cfgWith("relative/{tenant}"), nil); err == nil {
		t.Error("a relative root was accepted")
	}
	reg, err := auth.NewTenantRegistry(time.Minute, nil)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := provideScoringScopes(cfgWith("/data"), reg); err == nil {
		t.Error("VMAFX_SCORING_ROOTS was accepted next to a tenant registry")
	}
	if s, err := provideScoringScopes(cfgWith(""), nil); err != nil {
		t.Errorf("no roots refused at startup: %v", err)
	} else if roots, _ := s.For("acme"); !roots.Empty() {
		t.Errorf("no configuration gave acme roots %v, want none (deny by default)", roots.Strings())
	}
}

func TestScoreAdmitsOnlyTheTenantsRoot(t *testing.T) {
	dir := mediaTree(t)
	s := &scoringServer{scopes: tenantScopes(dir), metrics: oteltest.Metrics(t, prometheus.NewRegistry()), log: slog.New(slog.DiscardHandler)}
	ctx := tenantCtx("acme")
	ref, dis, err := s.scopedInputs(ctx, filepath.Join(dir, "acme", "ref.y4m"), filepath.Join(dir, "acme", "ref.y4m"))
	if err != nil || ref != filepath.Join(dir, "acme", "ref.y4m") || dis != ref {
		t.Fatalf("own input: %q %q %v", ref, dis, err)
	}
	refused := map[string]string{
		"other tenant": filepath.Join(dir, "rival", "secret.y4m"),
		"traversal":    filepath.Join(dir, "acme") + "/../rival/secret.y4m",
		"symlink out":  filepath.Join(dir, "acme", "steal.y4m"),
		"relative":     "acme/ref.y4m",
	}
	for name, in := range refused {
		if _, _, err := s.scopedInputs(ctx, filepath.Join(dir, "acme", "ref.y4m"), in); codeOf(err) != codes.PermissionDenied {
			t.Errorf("%s (%s): err %v, want PermissionDenied", name, in, err)
		}
	}
	// Over the handler: refused before the scorer (nil here) is touched.
	resp, err := s.Score(ctx, &vmafxv1.ScoreRequest{Reference: refused["symlink out"], Distorted: refused["symlink out"]})
	if codeOf(err) != codes.PermissionDenied || resp != nil {
		t.Fatalf("Score of a symlink escape: %v, %v; want PermissionDenied", resp, err)
	}
}

func TestSubmitJobAdmitsOnlyTheTenantsRoot(t *testing.T) {
	dir := mediaTree(t)
	f := newGRPCFixture(t)
	f.srv.scopes = tenantScopes(dir)
	submit := func(ref string) error {
		_, err := f.srv.SubmitJob(tenantCtx("acme"), &controllerv1.SubmitJobRequest{
			Scoring: &controllerv1.ScoringParams{Reference: ref, Distorted: filepath.Join(dir, "acme", "ref.y4m")},
		})
		return err
	}
	if err := submit(filepath.Join(dir, "acme", "ref.y4m")); err != nil {
		t.Fatalf("own input refused: %v", err)
	}
	for _, in := range []string{filepath.Join(dir, "rival", "secret.y4m"), filepath.Join(dir, "acme") + "/../rival/x.y4m"} {
		if err := submit(in); codeOf(err) != codes.PermissionDenied {
			t.Errorf("SubmitJob(%s): err %v, want PermissionDenied", in, err)
		}
	}
	if _, err := f.srv.SubmitJob(tenantCtx("nobody"), &controllerv1.SubmitJobRequest{
		Scoring: &controllerv1.ScoringParams{Reference: "/x", Distorted: "/y"},
	}); codeOf(err) != codes.PermissionDenied {
		t.Errorf("tenant outside every root: err %v, want PermissionDenied", err)
	}
}

func TestPullWorkHandsTheRootsToTheNode(t *testing.T) {
	dir := mediaTree(t)
	f := newGRPCFixture(t)
	f.srv.scopes = tenantScopes(dir)
	ctx := tenantCtx("acme")
	in := filepath.Join(dir, "acme", "ref.y4m")
	if _, err := f.srv.SubmitJob(ctx, &controllerv1.SubmitJobRequest{
		Scoring: &controllerv1.ScoringParams{Reference: in, Distorted: in},
	}); err != nil {
		t.Fatalf("SubmitJob: %v", err)
	}
	reg, err := f.srv.RegisterNode(ctx, &controllerv1.RegisterNodeRequest{Name: "n", Capability: &controllerv1.NodeCapability{Backends: []string{"cpu"}}})
	if err != nil {
		t.Fatal(err)
	}
	resp, err := f.srv.PullWork(ctx, &controllerv1.PullWorkRequest{
		NodeId: reg.GetNodeId(), SessionToken: reg.GetSessionToken(),
		Capability: &controllerv1.NodeCapability{Backends: []string{"cpu"}},
	})
	if err != nil || resp.GetJob() == nil {
		t.Fatalf("PullWork: %v %v", resp, err)
	}
	if got := resp.GetJob().GetScoringRoots(); !slices.Equal(got, []string{filepath.Join(dir, "acme")}) {
		t.Fatalf("scoring_roots = %q, want acme's root", got)
	}
	job, err := f.srv.GetJob(ctx, &controllerv1.GetJobRequest{JobId: resp.GetJob().GetId()})
	if err != nil || len(job.GetScoringRoots()) != 0 {
		t.Fatalf("GetJob carries scoring_roots %q (%v); only PullWork answers do", job.GetScoringRoots(), err)
	}
}

func TestHTTPScoreRefusesWithForbidden(t *testing.T) {
	dir := mediaTree(t)
	h := &httpServer{scopes: tenantScopes(dir), metrics: oteltest.Metrics(t, prometheus.NewRegistry()), log: slog.New(slog.DiscardHandler)}
	body := `{"reference":"` + filepath.Join(dir, "acme", "steal.y4m") + `","distorted":"` + filepath.Join(dir, "acme", "ref.y4m") + `"}`
	req := httptest.NewRequestWithContext(auth.ContextWithClaims(context.Background(), auth.Claims{TenantID: "acme"}),
		http.MethodPost, "/v1/score", bytes.NewBufferString(body))
	rec := httptest.NewRecorder()
	h.handleScore(rec, req)
	if rec.Code != http.StatusForbidden || !strings.Contains(rec.Body.String(), "outside the tenant's scoring roots") {
		t.Fatalf("POST /v1/score with a symlink escape: %d %s, want 403", rec.Code, rec.Body.String())
	}
}
