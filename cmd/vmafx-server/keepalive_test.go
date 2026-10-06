// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

//go:build cgo

package main

import (
	"context"
	"errors"
	"net"
	"testing"
	"time"

	"go.uber.org/fx"
	"go.uber.org/fx/fxtest"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/health"
	healthpb "google.golang.org/grpc/health/grpc_health_v1"
	"google.golang.org/grpc/keepalive"
	"google.golang.org/grpc/status"
)

// TestScoringKeepaliveOutlastsFrameworkGrace: an app keepalive option placed
// after the framework's (as golusoris appends app options) replaces it, so a
// stream that outlives the framework's connection age plus grace still
// completes. Scaled down: the framework-like option here rotates after 300 ms
// with a 100 ms grace (the cut lands about 1.5 s in, after the GOAWAY ping)
// and the stream is held 3 s; without scoringKeepalive() the server cuts it.
func TestScoringKeepaliveOutlastsFrameworkGrace(t *testing.T) {
	framework := grpc.KeepaliveParams(keepalive.ServerParameters{
		MaxConnectionAge: 300 * time.Millisecond, MaxConnectionAgeGrace: 100 * time.Millisecond,
	})
	if err := watchFor(t, streamHold, framework, scoringKeepalive()); err != nil {
		t.Fatalf("a 3 s stream was cut with scoringKeepalive() after the framework option: %v", err)
	}
	if err := watchFor(t, streamHold, framework); err == nil {
		t.Fatal("the framework-like keepalive did not cut the stream: the test cannot see the defect")
	}
	if grpcConnectionAgeGrace < scoreTimeoutBound {
		t.Errorf("grace %v is shorter than a vmaf run may take (%v)", grpcConnectionAgeGrace, scoreTimeoutBound)
	}
}

// streamHold outlasts the scaled framework age, grace and GOAWAY ping.
const streamHold = 3 * time.Second

// scoreTimeoutBound mirrors pkg/libvmaf's scoreTimeout (30 minutes).
const scoreTimeoutBound = 30 * time.Minute

// watchFor holds a server stream open for `hold` on a server built with
// `opts`; it returns the stream's error, nil when it lasted.
func watchFor(t *testing.T, hold time.Duration, opts ...grpc.ServerOption) error {
	t.Helper()
	lis, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	srv := grpc.NewServer(opts...)
	healthpb.RegisterHealthServer(srv, health.NewServer())
	go func() { _ = srv.Serve(lis) }()
	defer srv.Stop()
	conn, err := grpc.NewClient(lis.Addr().String(), grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	defer func() { _ = conn.Close() }()
	ctx, cancel := context.WithTimeout(context.Background(), hold)
	defer cancel()
	stream, err := healthpb.NewHealthClient(conn).Watch(ctx, &healthpb.HealthCheckRequest{})
	if err != nil {
		return err
	}
	for range maxWatchMessages {
		if _, err := stream.Recv(); err != nil {
			// The client's own deadline ending the stream means it was held for
			// the whole period; grpc's timer may fire before ctx.Err() is set.
			if ctx.Err() != nil || status.Code(err) == codes.DeadlineExceeded {
				return nil
			}
			return err
		}
	}
	return errors.New("the health stream sent more messages than a watch ever does")
}

// maxWatchMessages bounds the receive loop (HISS-02); a health watch sends one.
const maxWatchMessages = 1000

// appServerOptions are the app-supplied gRPC server options of the graph.
type appServerOptions struct {
	fx.In
	Options []grpc.ServerOption `group:"grpc.serveropts"`
}

// TestProductionGraphCarriesTheKeepalive: the production graph hands the
// framework at least one app server option (scoringKeepalive is the only one);
// dropping it from hardeningOptions() empties the group.
func TestProductionGraphCarriesTheKeepalive(t *testing.T) {
	writeVmafStubForApp(t)
	var got appServerOptions
	app := fxtest.New(t, productionGraph(), fx.Populate(&got))
	defer app.RequireStart().RequireStop()
	if len(got.Options) == 0 {
		t.Fatal("no app gRPC server option in the production graph: the keepalive grace is missing")
	}
}
