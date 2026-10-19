// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/auth/tenants.go — the tenant registry: per-tenant OIDC
// providers, suspension and role whitelists from VmafxTenant resources.
//
// Design
// ======
// With a registry configured, a token is accepted only when it resolves to
// exactly one configured, enabled tenant:
//
//  1. The token's (unverified) "iss" selects the tenants whose oidc.issuer it
//     names. No such tenant: refused.
//  2. Each candidate's JWKS endpoint verifies the signature (once per
//     endpoint), the registered claims are checked against the candidate's
//     own issuer and audience, then its tenantClaim is read. The candidate whose tenantClaim value equals its
//     tenantId is the match, so one tenant's identity provider can never mint
//     a token for another tenant. No match, or more than one: refused.
//  3. A matched tenant with enabled=false is refused with PermissionDenied.
//  4. Roles: the vmafx roles of the rolesClaim, minus those not in
//     rbac.allowedRoles. A token naming no vmafx role gets rbac.defaultRole.
//
// The set is swapped atomically on reload; a request reads one snapshot and
// uses it for every step, so a reload cannot split a decision. A snapshot
// older than the staleness bound refuses every token (the controller can no
// longer tell whether a tenant was suspended).
//
// ADR-0794: multi-tenant auth gateway. ADR-1519: tenant registry.

package auth

import (
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"net"
	"net/url"
	"regexp"
	"slices"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	vmafxv1 "github.com/VMAFx/vmafx/api/vmafx/v1"
	"github.com/VMAFx/vmafx/pkg/scoringscope"
)

// The spec of a VmafxTenant resource is the generated Go type of
// api/vmafx/v1 (api/vmafx-platform.toml, ADR-2350 D13), the type the CRD
// deploy/helm/vmafx/crds/vmafx.dev_vmafxtenants.yaml is generated from; the
// auth package names it and its parts with these aliases. Omitted fields take
// the CRD's defaults, applied by buildTenant.
type (
	// TenantSpec is the spec of one VmafxTenant resource.
	TenantSpec = vmafxv1.VmafxTenantSpec
	// TenantOIDC is the identity provider of one tenant.
	TenantOIDC = vmafxv1.VmafxTenantOIDC
	// TenantRBAC is the role policy of one tenant.
	TenantRBAC = vmafxv1.VmafxTenantRBAC
	// TenantScoring limits the inputs the tenant's callers may score
	// (ADR-1577): local directories and remote prefixes (pkg/scoringscope).
	// No roots, the default, means the tenant may score nothing.
	TenantScoring = vmafxv1.VmafxTenantScoring
)

// NamedTenantSpec is a spec with the name of where it came from (a resource
// name or a file position), for error messages. Err is set when the source
// could not decode the resource; the registry refuses such an entry like an
// invalid spec, so one broken resource does not fail the whole source.
type NamedTenantSpec struct {
	Source string
	Spec   TenantSpec
	Err    error
}

// tenantIDPattern is the CRD's tenantId pattern.
var tenantIDPattern = regexp.MustCompile(`^[a-z0-9][a-z0-9\-]{0,62}[a-z0-9]$`)

// tenant is one validated tenant with its defaults applied.
type tenant struct {
	id          string
	source      string
	enabled     bool
	issuer      string
	audience    string
	tenantClaim string
	rolesClaim  string
	defaultRole string
	allowed     []string
	roots       scoringscope.Roots
	cache       *jwksCache
}

// tenantSet is one immutable snapshot of the registry.
type tenantSet struct {
	byIssuer map[string][]*tenant
	byID     map[string]*tenant
	count    int
	loadedAt time.Time
}

// TenantRegistry holds the configured tenants. It is safe for concurrent use.
type TenantRegistry struct {
	state      atomic.Pointer[tenantSet]
	staleAfter time.Duration
	log        *slog.Logger
	now        func() time.Time

	mu     sync.Mutex
	caches map[string]*jwksCache // JWKS endpoint → cache, kept across reloads
}

// NewTenantRegistry returns an empty registry that refuses every token until
// the first Load. A snapshot older than staleAfter refuses every token.
func NewTenantRegistry(staleAfter time.Duration, log *slog.Logger) (*TenantRegistry, error) {
	if staleAfter <= 0 {
		return nil, fmt.Errorf("auth: tenant staleness bound must be positive, got %s", staleAfter)
	}
	if log == nil {
		log = slog.Default()
	}
	return &TenantRegistry{
		staleAfter: staleAfter,
		log:        log,
		now:        time.Now,
		caches:     make(map[string]*jwksCache),
	}, nil
}

// SetClockForTest replaces the registry's clock. Test-only.
func (r *TenantRegistry) SetClockForTest(now func() time.Time) { r.now = now }

// Load replaces the tenant set with specs, which must all be valid: any
// invalid spec or duplicate tenantId fails the whole load and keeps the
// previous set. The controller calls it at startup, so a misconfigured
// tenant stops the controller from starting.
func (r *TenantRegistry) Load(specs []NamedTenantSpec) error {
	tenants, errs := r.buildTenants(specs)
	if len(errs) > 0 {
		return fmt.Errorf("auth: tenant configuration rejected: %w", errors.Join(errs...))
	}
	for _, t := range tenants {
		if t.audience == "" {
			r.log.Warn("tenant has no oidc.audience: tokens its provider issues for other APIs are accepted",
				"tenant", t.id, "source", t.source)
		}
	}
	r.publish(tenants)
	return nil
}

// Reload replaces the tenant set with the valid specs and drops the rest: an
// invalid spec, and every copy of a duplicated tenantId, is left out (its
// tokens are refused) and returned as an error. A running controller calls
// it on refresh, so one broken tenant does not lock out the others.
func (r *TenantRegistry) Reload(specs []NamedTenantSpec) []error {
	tenants, errs := r.buildTenants(specs)
	for _, err := range errs {
		r.log.Error("tenant refused; its tokens are rejected until it is fixed", "error", err)
	}
	r.publish(tenants)
	return errs
}

// MarkRefreshFailed records a failed refresh: the set is kept, and it ages
// towards the staleness bound.
func (r *TenantRegistry) MarkRefreshFailed(err error) {
	set := r.state.Load()
	age := time.Duration(0)
	if set != nil {
		age = r.now().Sub(set.loadedAt)
	}
	r.log.Warn("tenant refresh failed; keeping the last tenant set", "error", err,
		"age", age, "refused_after", r.staleAfter)
}

// ScoringRoots returns the scoring roots of tenantID in the current set; ok is
// false for a tenant the set does not hold (ADR-1577).
func (r *TenantRegistry) ScoringRoots(tenantID string) (scoringscope.Roots, bool) {
	set := r.state.Load()
	if set == nil {
		return scoringscope.Roots{}, false
	}
	t, ok := set.byID[tenantID]
	if !ok {
		return scoringscope.Roots{}, false
	}
	return t.roots, true
}

// Count returns the number of tenants in the current set.
func (r *TenantRegistry) Count() int {
	if set := r.state.Load(); set != nil {
		return set.count
	}
	return 0
}

// publish swaps in a new snapshot built from tenants and forgets the JWKS
// caches no tenant names any more.
func (r *TenantRegistry) publish(tenants []*tenant) {
	byIssuer := make(map[string][]*tenant, len(tenants))
	byID := make(map[string]*tenant, len(tenants))
	referenced := make(map[*jwksCache]bool, len(tenants))
	for _, t := range tenants {
		byIssuer[t.issuer] = append(byIssuer[t.issuer], t)
		byID[t.id] = t
		referenced[t.cache] = true
	}
	r.state.Store(&tenantSet{byIssuer: byIssuer, byID: byID, count: len(tenants), loadedAt: r.now()})
	r.mu.Lock()
	for endpoint, c := range r.caches {
		if !referenced[c] {
			delete(r.caches, endpoint)
		}
	}
	r.mu.Unlock()
	r.log.Info("tenant set loaded", "tenants", len(tenants))
}

// buildTenants validates every spec and drops duplicated tenantIds.
func (r *TenantRegistry) buildTenants(specs []NamedTenantSpec) ([]*tenant, []error) {
	var errs []error
	seen := make(map[string][]string, len(specs))
	built := make([]*tenant, 0, len(specs))
	for _, ns := range specs {
		t, err := r.buildTenant(ns)
		if err != nil {
			errs = append(errs, err)
			continue
		}
		seen[t.id] = append(seen[t.id], t.source)
		built = append(built, t)
	}
	out := built[:0]
	for _, t := range built {
		if srcs := seen[t.id]; len(srcs) > 1 {
			errs = append(errs, fmt.Errorf("tenant %q (%s): tenantId also configured by %s",
				t.id, t.source, strings.Join(srcs, ", ")))
			continue
		}
		out = append(out, t)
	}
	return out, errs
}

// buildTenant validates one spec and applies the CRD defaults.
func (r *TenantRegistry) buildTenant(ns NamedTenantSpec) (*tenant, error) {
	s := ns.Spec
	fail := func(format string, args ...any) (*tenant, error) {
		return nil, fmt.Errorf("tenant %q (%s): %s", s.TenantID, ns.Source, fmt.Sprintf(format, args...))
	}
	if ns.Err != nil {
		return fail("%v", ns.Err)
	}
	if !tenantIDPattern.MatchString(s.TenantID) {
		return fail("tenantId must match %s", tenantIDPattern)
	}
	if err := checkIssuer(s.OIDC.Issuer); err != nil {
		return fail("oidc.issuer: %v", err)
	}
	if err := checkJWKSEndpoint(s.OIDC.JWKSEndpoint); err != nil {
		return fail("oidc.jwksEndpoint: %v", err)
	}
	defaultRole, allowed, err := tenantRoles(s.RBAC)
	if err != nil {
		return fail("rbac: %v", err)
	}
	roots, err := tenantScoringRoots(s.Scoring)
	if err != nil {
		return fail("scoring: %v", err)
	}
	return &tenant{
		id:          s.TenantID,
		source:      ns.Source,
		enabled:     s.Enabled == nil || *s.Enabled,
		issuer:      s.OIDC.Issuer,
		audience:    s.OIDC.Audience,
		tenantClaim: orDefault(s.OIDC.TenantClaim, "tid"),
		rolesClaim:  orDefault(s.OIDC.RolesClaim, "vmafx_roles"),
		defaultRole: defaultRole,
		allowed:     allowed,
		roots:       roots,
		cache:       r.cacheFor(s.OIDC.JWKSEndpoint),
	}, nil
}

// tenantScoringRoots validates the tenant's scoring roots; none is valid and
// admits no input.
func tenantScoringRoots(s *TenantScoring) (scoringscope.Roots, error) {
	if s == nil {
		return scoringscope.Parse(nil)
	}
	return scoringscope.Parse(s.Roots)
}

// tenantRoles applies the CRD defaults to rbac and validates it: every role
// known, and the default role among the allowed ones (otherwise the default
// would be stripped again and the field would silently mean nothing).
func tenantRoles(rbac *TenantRBAC) (string, []string, error) {
	defaultRole, allowed := RoleReader, []string{RoleReader, RoleWriter}
	if rbac != nil {
		defaultRole = orDefault(rbac.DefaultRole, defaultRole)
		if rbac.AllowedRoles != nil {
			allowed = slices.Clone(rbac.AllowedRoles)
		}
	}
	if len(allowed) == 0 {
		return "", nil, errors.New("allowedRoles is empty")
	}
	for _, role := range append([]string{defaultRole}, allowed...) {
		if !IsKnownRole(role) {
			return "", nil, fmt.Errorf("unknown role %q", role)
		}
	}
	if !slices.Contains(allowed, defaultRole) {
		return "", nil, fmt.Errorf("defaultRole %q is not in allowedRoles %v", defaultRole, allowed)
	}
	if defaultRole == RoleNode {
		// A token without a role claim must never act as a compute node: the
		// node role is granted by the token alone (ADR-1563).
		return "", nil, fmt.Errorf("defaultRole %q is refused: a compute node needs the role in its token", RoleNode)
	}
	return defaultRole, allowed, nil
}

// checkIssuer requires an absolute http(s) URL with a host.
func checkIssuer(raw string) error {
	u, err := url.Parse(raw)
	if err != nil || (u.Scheme != "https" && u.Scheme != "http") || u.Host == "" {
		return fmt.Errorf("%q is not an absolute http(s) URL", raw)
	}
	return nil
}

// checkJWKSEndpoint requires an https URL, or http on a loopback host only:
// the keys fetched there decide which tokens are genuine, so they must not
// travel over an unauthenticated network.
func checkJWKSEndpoint(raw string) error {
	if err := checkIssuer(raw); err != nil {
		return err
	}
	u, _ := url.Parse(raw) // parsed above
	if u.Scheme == "https" || isLoopbackHost(u.Hostname()) {
		return nil
	}
	return fmt.Errorf("%q must use https (plain http is accepted for loopback hosts only)", raw)
}

// isLoopbackHost reports whether host is localhost or a loopback IP.
func isLoopbackHost(host string) bool {
	if host == "localhost" {
		return true
	}
	ip := net.ParseIP(host)
	return ip != nil && ip.IsLoopback()
}

// orDefault returns v, or def when v is empty.
func orDefault(v, def string) string {
	if v == "" {
		return def
	}
	return v
}

// cacheFor returns the JWKS cache of endpoint, shared by every tenant that
// names it and kept across reloads so a reload does not refetch keys.
func (r *TenantRegistry) cacheFor(endpoint string) *jwksCache {
	r.mu.Lock()
	defer r.mu.Unlock()
	c, ok := r.caches[endpoint]
	if !ok {
		c = newJWKSCache(endpoint, r.log)
		r.caches[endpoint] = c
	}
	return c
}

// Errors of Resolve, distinguished by the transports' status mapping.
var (
	// errTenantDisabled: the token belongs to a suspended tenant (403).
	errTenantDisabled = errors.New("tenant is suspended")
	// errTenantsStale: the tenant set is older than the staleness bound (503).
	errTenantsStale = errors.New("tenant configuration is stale")
)

// Resolve verifies token against the configured tenants and returns the
// caller's claims. See the package comment of this file for the steps.
func (r *TenantRegistry) Resolve(token string) (Claims, error) {
	set := r.state.Load()
	if set == nil || r.now().Sub(set.loadedAt) > r.staleAfter {
		return Claims{}, errTenantsStale
	}
	tok, err := parseJWT(token)
	if err != nil {
		return Claims{}, err
	}
	iss, _ := extractStringClaim(tok.claims, "iss")
	candidates := set.byIssuer[iss]
	if len(candidates) == 0 {
		return Claims{}, fmt.Errorf("jwt: issuer %q is not configured for any tenant", iss)
	}
	t, raw, err := matchTenant(tok, candidates)
	if err != nil {
		return Claims{}, err
	}
	if !t.enabled {
		return Claims{}, fmt.Errorf("%w: %q", errTenantDisabled, t.id)
	}
	sub, _ := extractStringClaim(raw, "sub")
	return Claims{Subject: sub, TenantID: t.id, Roles: t.roles(raw)}, nil
}

// matchTenant returns the one candidate that verifies tok and whose
// tenantClaim value is its own tenantId. The signature is checked once per
// JWKS cache, not once per candidate: tenants of one identity provider share
// a cache, and a forged token must not cost one RSA verification per tenant.
func matchTenant(tok parsedJWT, candidates []*tenant) (*tenant, map[string]json.RawMessage, error) {
	signatures := make(map[*jwksCache]error, len(candidates))
	var (
		match   *tenant
		lastErr error
	)
	for _, t := range candidates {
		if err := verifiedOnce(tok, t.cache, signatures); err != nil {
			lastErr = err
			continue
		}
		if err := validateJWTClaims(tok.payload, t.issuer, t.audience); err != nil {
			lastErr = err
			continue
		}
		if tid, ok := extractStringClaim(tok.claims, t.tenantClaim); !ok || tid != t.id {
			continue
		}
		if match != nil {
			return nil, nil, fmt.Errorf("jwt: token matches tenants %q and %q", match.id, t.id)
		}
		match = t
	}
	if match == nil && lastErr != nil {
		return nil, nil, lastErr
	}
	if match == nil {
		return nil, nil, errors.New("jwt: token does not belong to a configured tenant of its issuer")
	}
	return match, tok.claims, nil
}

// verifiedOnce verifies tok's signature against cache, remembering the
// outcome per cache in seen.
func verifiedOnce(tok parsedJWT, cache *jwksCache, seen map[*jwksCache]error) error {
	err, done := seen[cache]
	if !done {
		err = verifyJWTSignature(tok.parts, tok.header, cache)
		seen[cache] = err
	}
	return err
}

// roles returns the token's vmafx roles that the tenant allows, or the
// tenant's default role when the token names no vmafx role at all.
func (t *tenant) roles(raw map[string]json.RawMessage) []string {
	var named []string
	for _, role := range extractStringSliceClaim(raw, t.rolesClaim) {
		if IsKnownRole(role) {
			named = append(named, role)
		}
	}
	if len(named) == 0 {
		return []string{t.defaultRole}
	}
	kept := make([]string, 0, len(named))
	for _, role := range named {
		if slices.Contains(t.allowed, role) {
			kept = append(kept, role)
		}
	}
	return kept
}
