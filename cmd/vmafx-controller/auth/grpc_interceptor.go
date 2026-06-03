// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/auth/grpc_interceptor.go — gRPC unary + stream
// interceptors that enforce JWT authentication on the controller's gRPC
// services.
//
// Design
// ======
// The interceptors extract the JWT from gRPC metadata key "authorization"
// (lowercase, per gRPC HTTP/2 header semantics).  On success, Claims are
// stored in the context exactly as the HTTP middleware does — code shared
// between HTTP and gRPC handlers calls TenantIDFromCtx / ClaimsFromCtx
// without knowing which transport was used.
//
// ADR-0794: multi-tenant auth gateway.

package auth

import (
	"context"
	"fmt"
	"strings"

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/metadata"
	"google.golang.org/grpc/status"
)

// GRPCUnaryInterceptor returns a gRPC server unary interceptor that enforces
// JWT authentication.  Call it with grpc.UnaryInterceptor(...) when creating
// the gRPC server.
func (m *Middleware) GRPCUnaryInterceptor() grpc.UnaryServerInterceptor {
	return func(
		ctx context.Context,
		req any,
		info *grpc.UnaryServerInfo,
		handler grpc.UnaryHandler,
	) (any, error) {
		method := ""
		if info != nil {
			method = info.FullMethod
		}
		ctx, err := m.authenticateGRPC(ctx, method)
		if err != nil {
			return nil, err
		}
		return handler(ctx, req)
	}
}

// GRPCStreamInterceptor returns a gRPC server stream interceptor that enforces
// JWT authentication.  Call it with grpc.StreamInterceptor(...) when creating
// the gRPC server.
func (m *Middleware) GRPCStreamInterceptor() grpc.StreamServerInterceptor {
	return func(
		srv any,
		ss grpc.ServerStream,
		info *grpc.StreamServerInfo,
		handler grpc.StreamHandler,
	) error {
		ctx, err := m.authenticateGRPC(ss.Context(), info.FullMethod)
		if err != nil {
			return err
		}
		return handler(srv, &wrappedServerStream{ServerStream: ss, ctx: ctx})
	}
}

// authenticateGRPC extracts and verifies the JWT from gRPC metadata.
// Returns an enriched context on success, or a gRPC status error on failure.
func (m *Middleware) authenticateGRPC(ctx context.Context, method string) (context.Context, error) {
	if m.cfg.Disabled {
		return withClaims(ctx, Claims{
			Subject:  "dev",
			TenantID: "dev",
			Roles:    []string{RoleAdmin},
		}), nil
	}

	md, ok := metadata.FromIncomingContext(ctx)
	if !ok {
		return ctx, status.Errorf(codes.Unauthenticated, "missing request metadata")
	}

	vals := md.Get("authorization")
	if len(vals) == 0 {
		return ctx, status.Errorf(codes.Unauthenticated, "missing authorization metadata")
	}

	authHeader := vals[0]
	const prefix = "Bearer "
	if !strings.HasPrefix(authHeader, prefix) {
		return ctx, status.Errorf(codes.Unauthenticated, "authorization must be Bearer token")
	}
	token := strings.TrimPrefix(authHeader, prefix)

	rawClaims, err := verifyJWT(token, m.cache, m.cfg.Issuer, m.cfg.Audience)
	if err != nil {
		m.log.Warn("grpc auth: rejected", "method", method, "error", err)
		return ctx, status.Errorf(codes.Unauthenticated, "invalid token: %v", err)
	}

	tenantID, ok2 := extractStringClaim(rawClaims, m.cfg.tenantClaim())
	if !ok2 || tenantID == "" {
		return ctx, status.Errorf(codes.Unauthenticated,
			"jwt claim %q is required", m.cfg.tenantClaim())
	}

	sub, _ := extractStringClaim(rawClaims, "sub")
	roles := extractStringSliceClaim(rawClaims, m.cfg.rolesClaim())
	claims := Claims{Subject: sub, TenantID: tenantID, Roles: roles}

	return withClaims(ctx, claims), nil
}

// RequireGRPCRole returns a unary interceptor that enforces a role constraint
// after the auth interceptor has run.  Pair with GRPCUnaryInterceptor in a
// grpc.ChainUnaryInterceptor(...) call.
func RequireGRPCRole(roles ...string) grpc.UnaryServerInterceptor {
	return func(
		ctx context.Context,
		req any,
		_ *grpc.UnaryServerInfo,
		handler grpc.UnaryHandler,
	) (any, error) {
		c, ok := ClaimsFromCtx(ctx)
		if !ok {
			return nil, status.Errorf(codes.Unauthenticated, "unauthenticated")
		}
		if !c.HasRole(roles...) {
			return nil, status.Errorf(codes.PermissionDenied,
				"role required: %s", strings.Join(roles, " | "))
		}
		return handler(ctx, req)
	}
}

// wrappedServerStream replaces the stream's Context with an enriched one.
type wrappedServerStream struct {
	grpc.ServerStream
	ctx context.Context
}

func (w *wrappedServerStream) Context() context.Context { return w.ctx }

// ---------------------------------------------------------------------------
// Tenant-scoping helpers for gRPC handlers
// ---------------------------------------------------------------------------

// AssertTenantOwns verifies that the caller's tenant_id matches the supplied
// jobTenantID.  Returns a gRPC PermissionDenied error if not.
//
// Usage in a gRPC handler:
//
//	if err := auth.AssertTenantOwns(ctx, job.TenantID); err != nil {
//	    return nil, err
//	}
func AssertTenantOwns(ctx context.Context, resourceTenantID string) error {
	tid := TenantIDFromCtx(ctx)
	if tid == "" {
		return status.Errorf(codes.Unauthenticated, "tenant_id not in context")
	}
	if tid != resourceTenantID {
		return status.Errorf(codes.PermissionDenied,
			"resource belongs to tenant %q, caller is tenant %q", resourceTenantID, tid)
	}
	return nil
}

// AssertHTTPTenantOwns is the HTTP analogue of AssertTenantOwns.
func AssertHTTPTenantOwns(ctx context.Context, resourceTenantID string) error {
	tid := TenantIDFromCtx(ctx)
	if tid == "" {
		return fmt.Errorf("auth: tenant_id not in context")
	}
	if tid != resourceTenantID {
		return fmt.Errorf("auth: resource belongs to tenant %q, caller is %q",
			resourceTenantID, tid)
	}
	return nil
}
