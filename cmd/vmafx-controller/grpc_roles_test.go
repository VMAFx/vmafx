// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/grpc_roles_test.go — per-RPC role enforcement on the
// production gRPC server (ADR-1518, node row ADR-1563).
//
// TestEveryServedRPCHasARolePolicy ties controllerMethodRoles to the methods
// the production server actually serves, in both directions.
//
// TestGRPCRolesEnforcedPerRPC boots the production fx graph with auth enabled
// against a test identity provider and calls every RPC over the wire with a
// token per role. The expected minimum role of each RPC is written out here,
// independently of controllerMethodRoles, from the role table of ADR-0794,
// ADR-1563 (the node API needs vmafx:node, and only vmafx:node) and
// docs/server/auth.md: a refused call must fail with the policy's
// PermissionDenied before its handler runs, an admitted one must reach the
// handler (whatever the handler then answers to an empty request).

//go:build cgo

package main

import (
	"context"
	"errors"
	"io"
	"sort"
	"strings"
	"testing"
	"time"

	"go.uber.org/fx"
	"go.uber.org/fx/fxtest"
	googlegrpc "google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/metadata"
	"google.golang.org/grpc/status"

	"github.com/VMAFx/vmafx/cmd/vmafx-controller/auth"
	"github.com/VMAFx/vmafx/cmd/vmafx-controller/auth/authtest"
	vmafxv1 "github.com/VMAFx/vmafx/gen/go"
	controllerv1 "github.com/VMAFx/vmafx/gen/go/controller"
)

// TestEveryServedRPCHasARolePolicy fails when the server serves a method the
// policy does not list (it would be refused for everyone) or the policy lists a
// method the server does not serve (a stale or misspelt entry).
func TestEveryServedRPCHasARolePolicy(t *testing.T) {
	writeControllerEnv(t)
	var srv *googlegrpc.Server
	app := fxtest.New(t, productionGraph(), fx.Populate(&srv))
	app.RequireStart()
	defer app.RequireStop()

	var served []string
	for svc, info := range srv.GetServiceInfo() {
		for _, m := range info.Methods {
			served = append(served, "/"+svc+"/"+m.Name)
		}
	}
	policy := controllerMethodRoles()
	for _, method := range served {
		if _, ok := policy[method]; !ok {
			t.Errorf("served method %s has no entry in controllerMethodRoles", method)
		}
	}
	if len(policy) != len(served) {
		sort.Strings(served)
		t.Errorf("controllerMethodRoles has %d entries, the server serves %d methods: %v",
			len(policy), len(served), served)
	}
}

// rpcCall issues one RPC with an empty request and returns its status error.
type rpcCall func(ctx context.Context, cc *googlegrpc.ClientConn) error

// ignoreEOF maps the clean end of a stream to success.
func ignoreEOF(err error) error {
	if errors.Is(err, io.EOF) {
		return nil
	}
	return err
}

// unary adapts a unary client call to rpcCall.
func unary[R any](call func(ctx context.Context, cc *googlegrpc.ClientConn) (R, error)) rpcCall {
	return func(ctx context.Context, cc *googlegrpc.ClientConn) error {
		_, err := call(ctx, cc)
		return err
	}
}

func controllerClient(cc *googlegrpc.ClientConn) controllerv1.VmafxControllerClient {
	return controllerv1.NewVmafxControllerClient(cc)
}

func scoringClient(cc *googlegrpc.ClientConn) vmafxv1.VmafxScoringClient {
	return vmafxv1.NewVmafxScoringClient(cc)
}

func callStreamJobs(ctx context.Context, cc *googlegrpc.ClientConn) error {
	s, err := controllerClient(cc).StreamJobs(ctx, &controllerv1.StreamJobsRequest{})
	if err != nil {
		return err
	}
	_, err = s.Recv()
	return ignoreEOF(err)
}

func callScoreStream(ctx context.Context, cc *googlegrpc.ClientConn) error {
	s, err := scoringClient(cc).ScoreStream(ctx)
	if err != nil {
		return err
	}
	if err = s.CloseSend(); err != nil {
		return err
	}
	_, err = s.Recv()
	return ignoreEOF(err)
}

// Role classes of docs/server/auth.md's role table. The node API is its own
// class: no other role reaches it, and vmafx:node reaches nothing else
// (ADR-1563).
const (
	minReader = "reader"
	minWriter = "writer"
	nodeAPI   = "node"
)

// controllerRPCs lists every RPC of the controller with its minimum role.
var controllerRPCs = []struct {
	name    string
	minRole string
	call    rpcCall
}{
	{"Health", minReader, unary(func(ctx context.Context, cc *googlegrpc.ClientConn) (*vmafxv1.HealthResponse, error) {
		return scoringClient(cc).Health(ctx, &vmafxv1.HealthRequest{})
	})},
	{"Score", minWriter, unary(func(ctx context.Context, cc *googlegrpc.ClientConn) (*vmafxv1.ScoreResponse, error) {
		return scoringClient(cc).Score(ctx, &vmafxv1.ScoreRequest{})
	})},
	{"ScoreStream", minWriter, callScoreStream},
	{"GetJob", minReader, unary(func(ctx context.Context, cc *googlegrpc.ClientConn) (*controllerv1.Job, error) {
		return controllerClient(cc).GetJob(ctx, &controllerv1.GetJobRequest{})
	})},
	{"StreamJobs", minReader, callStreamJobs},
	{"SubmitJob", minWriter, unary(func(ctx context.Context, cc *googlegrpc.ClientConn) (*controllerv1.SubmitJobResponse, error) {
		return controllerClient(cc).SubmitJob(ctx, &controllerv1.SubmitJobRequest{})
	})},
	{"CancelJob", minWriter, unary(func(ctx context.Context, cc *googlegrpc.ClientConn) (*controllerv1.CancelJobResponse, error) {
		return controllerClient(cc).CancelJob(ctx, &controllerv1.CancelJobRequest{})
	})},
	{"RegisterNode", nodeAPI, unary(func(ctx context.Context, cc *googlegrpc.ClientConn) (*controllerv1.RegisterNodeResponse, error) {
		return controllerClient(cc).RegisterNode(ctx, &controllerv1.RegisterNodeRequest{})
	})},
	{"Heartbeat", nodeAPI, unary(func(ctx context.Context, cc *googlegrpc.ClientConn) (*controllerv1.HeartbeatResponse, error) {
		return controllerClient(cc).Heartbeat(ctx, &controllerv1.HeartbeatRequest{})
	})},
	{"PullWork", nodeAPI, unary(func(ctx context.Context, cc *googlegrpc.ClientConn) (*controllerv1.PullWorkResponse, error) {
		return controllerClient(cc).PullWork(ctx, &controllerv1.PullWorkRequest{})
	})},
	{"ReportResult", nodeAPI, unary(func(ctx context.Context, cc *googlegrpc.ClientConn) (*controllerv1.ReportResultResponse, error) {
		return controllerClient(cc).ReportResult(ctx, &controllerv1.ReportResultRequest{})
	})},
}

// callerRoles are the role claims each RPC is called with, and the minimum
// role classes each one satisfies.
var callerRoles = []struct {
	name      string
	claims    map[string]any
	satisfies []string
}{
	{"no roles claim", map[string]any{"tid": "acme"}, nil},
	{"unknown role", map[string]any{"tid": "acme", "vmafx_roles": []string{"vmafx:root"}}, nil},
	{auth.RoleReader, map[string]any{"tid": "acme", "vmafx_roles": []string{auth.RoleReader}},
		[]string{minReader}},
	{auth.RoleWriter, map[string]any{"tid": "acme", "vmafx_roles": []string{auth.RoleWriter}},
		[]string{minReader, minWriter}},
	{"vmafx:admin", map[string]any{"tid": "acme", "vmafx_roles": []string{"vmafx:admin"}},
		[]string{minReader, minWriter}},
	{"vmafx:node", map[string]any{"tid": "acme", "vmafx_roles": []string{"vmafx:node"}},
		[]string{nodeAPI}},
	{"vmafx:reader and vmafx:node", map[string]any{"tid": "acme", "vmafx_roles": []string{"vmafx:reader", "vmafx:node"}},
		[]string{minReader, nodeAPI}},
}

// startAuthEnabledController boots the production graph with auth enabled
// against iss and returns a client connection to its gRPC listener.
func startAuthEnabledController(t *testing.T, iss *authtest.Issuer) *googlegrpc.ClientConn {
	t.Helper()
	writeControllerEnv(t)
	t.Setenv("VMAFX_AUTH_DISABLED", "false")
	t.Setenv("VMAFX_JWKS_ENDPOINT", iss.URL())
	t.Setenv("VMAFX_AUTH_ISSUER", iss.URL())

	app, addr := startController(t, "VMAFX_GRPC_LISTEN")
	t.Cleanup(app.RequireStop)
	return dialInsecure(t, addr)
}

// dialInsecure returns a plaintext client connection to addr, closed when the
// test ends.
func dialInsecure(t *testing.T, addr string) *googlegrpc.ClientConn {
	t.Helper()
	cc, err := googlegrpc.NewClient(addr, googlegrpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		t.Fatalf("dial gRPC: %v", err)
	}
	t.Cleanup(func() {
		if cerr := cc.Close(); cerr != nil {
			t.Errorf("close gRPC client: %v", cerr)
		}
	})
	return cc
}

// isRoleRefusal reports whether err is the role policy's refusal.
func isRoleRefusal(err error) bool {
	return status.Code(err) == codes.PermissionDenied && strings.Contains(err.Error(), "role required")
}

// checkRPCAsRole calls rpc with token and checks the outcome against admitted.
func checkRPCAsRole(t *testing.T, cc *googlegrpc.ClientConn, token, what string, call rpcCall, admitted bool) {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	ctx = metadata.AppendToOutgoingContext(ctx, "authorization", "Bearer "+token)
	err := call(ctx, cc)
	if status.Code(err) == codes.Unauthenticated {
		t.Errorf("%s: valid token rejected as unauthenticated: %v", what, err)
		return
	}
	if admitted && isRoleRefusal(err) {
		t.Errorf("%s: refused by the role policy, want admitted: %v", what, err)
	}
	if !admitted && !isRoleRefusal(err) {
		t.Errorf("%s: err = %v, want PermissionDenied role required", what, err)
	}
}

func TestGRPCRolesEnforcedPerRPC(t *testing.T) {
	iss := authtest.NewIssuer(t)
	cc := startAuthEnabledController(t, iss)
	for _, caller := range callerRoles {
		token := iss.Token(t, caller.claims)
		for _, rpc := range controllerRPCs {
			admitted := false
			for _, class := range caller.satisfies {
				admitted = admitted || class == rpc.minRole
			}
			checkRPCAsRole(t, cc, token, rpc.name+" as "+caller.name, rpc.call, admitted)
		}
	}
}
