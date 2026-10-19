// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/scoring_scope.go — the scoring roots of each tenant
// (ADR-1577).
//
//	VMAFX_SCORING_ROOTS -> scoring.roots  Comma-separated roots for every caller when no tenant
//	                                      registry is configured; "{tenant}" in an entry becomes
//	                                      the caller's tenant ID (/data/{tenant}).
//
// With a tenant registry (VMAFX_AUTH_TENANTS_SOURCE) each VmafxTenant carries
// its own spec.scoring.roots, and VMAFX_SCORING_ROOTS is refused like the
// global identity-provider settings (ADR-1519). A tenant without roots may
// score nothing: the controller refuses every path it names (deny by
// default), and the node refuses a job whose roots are empty.
//
// Score and POST /v1/score read the files on the controller, so their inputs
// are resolved (symlinks followed) and the real paths scored. SubmitJob's
// inputs are read by a node, so the controller checks them lexically and
// PullWork hands the roots to the node, which resolves them where the files
// are.

//go:build cgo

package main

import (
	"errors"
	"fmt"
	"strings"

	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"

	"github.com/golusoris/golusoris/core/config"

	"github.com/VMAFx/vmafx/cmd/vmafx-controller/auth"
	"github.com/VMAFx/vmafx/pkg/scoringscope"
)

// tenantPlaceholder in a VMAFX_SCORING_ROOTS entry is replaced by the
// caller's tenant ID.
const tenantPlaceholder = "{tenant}"

// scoringScopes returns the scoring roots of a tenant.
type scoringScopes struct {
	registry *auth.TenantRegistry
	template []string
}

// provideScoringScopes reads scoring.roots and pairs it with the tenant
// registry (nil without one). Setting both is refused.
func provideScoringScopes(cfg *config.Config, reg *auth.TenantRegistry) (*scoringScopes, error) {
	template := splitRoots(cfg.Get("scoring.roots"))
	if reg != nil && len(template) > 0 {
		return nil, errors.New("VMAFX_SCORING_ROOTS is not used with a tenant registry: set spec.scoring.roots on each VmafxTenant")
	}
	// Validate the template once with a sample tenant so a malformed entry
	// stops the controller at startup, not at the first request.
	if _, err := scoringscope.Parse(expandRoots(template, "tenant")); err != nil {
		return nil, fmt.Errorf("VMAFX_SCORING_ROOTS: %w", err)
	}
	return &scoringScopes{registry: reg, template: template}, nil
}

// splitRoots splits a comma-separated list and drops empty entries.
func splitRoots(raw string) []string {
	var out []string
	for r := range strings.SplitSeq(raw, ",") {
		if r = strings.TrimSpace(r); r != "" {
			out = append(out, r)
		}
	}
	return out
}

// expandRoots replaces the tenant placeholder in every entry.
func expandRoots(template []string, tenantID string) []string {
	out := make([]string, len(template))
	for i, r := range template {
		out[i] = strings.ReplaceAll(r, tenantPlaceholder, tenantID)
	}
	return out
}

// For returns the roots of tenantID. A tenant the registry does not hold has
// none. Without a registry the tenant ID comes from a token claim unchecked,
// so an ID that could change a root's shape (a "/", a ":", "." or "..") is
// refused rather than substituted.
func (s *scoringScopes) For(tenantID string) (scoringscope.Roots, error) {
	if s.registry != nil {
		roots, _ := s.registry.ScoringRoots(tenantID)
		return roots, nil
	}
	if strings.ContainsAny(tenantID, "/\\:\x00") || tenantID == "." || tenantID == ".." {
		return scoringscope.Roots{}, fmt.Errorf("tenant ID %q cannot name a scoring root", tenantID)
	}
	return scoringscope.Parse(expandRoots(s.template, tenantID))
}

// resolveInputs resolves both inputs of a request scored on the controller
// and returns the paths to score. A refusal is PermissionDenied and names
// the input, not another tenant.
func (s *scoringScopes) resolveInputs(tenantID, ref, dis string) (string, string, error) {
	roots, err := s.For(tenantID)
	if err != nil {
		return "", "", status.Errorf(codes.PermissionDenied, "scoring roots: %v", err)
	}
	refPath, err := roots.Resolve(ref)
	if err != nil {
		return "", "", status.Error(codes.PermissionDenied, err.Error())
	}
	disPath, err := roots.Resolve(dis)
	if err != nil {
		return "", "", status.Error(codes.PermissionDenied, err.Error())
	}
	return refPath, disPath, nil
}

// checkInputs checks both inputs of a job a node will read, lexically.
func (s *scoringScopes) checkInputs(tenantID, ref, dis string) error {
	roots, err := s.For(tenantID)
	if err != nil {
		return status.Errorf(codes.PermissionDenied, "scoring roots: %v", err)
	}
	for _, in := range []string{ref, dis} {
		if err := roots.Check(in); err != nil {
			return status.Error(codes.PermissionDenied, err.Error())
		}
	}
	return nil
}

// rootsFor returns the configured roots of tenantID for a PullWork answer.
func (s *scoringScopes) rootsFor(tenantID string) ([]string, error) {
	roots, err := s.For(tenantID)
	if err != nil {
		return nil, err
	}
	return roots.Strings(), nil
}
