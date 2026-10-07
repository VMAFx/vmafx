// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-node/controller_provider.go — fx provider of the controller
// client (ADR-0713 node loop, ADR-1119 composition).
//
// The provider is constructed after the Executor and before the golusoris
// *grpc.Server (see nodeLifecycleOptions), so fx's reverse-order stop runs:
// gRPC GracefulStop -> controller client drain -> FeedbackClient drainer stop
// -> scorer Close. The client therefore finishes and reports its running jobs
// while the executor and scorer still exist.

//go:build cgo

package main

import (
	"context"
	"errors"
	"fmt"
	"log/slog"

	"go.uber.org/fx"
	googlegrpc "google.golang.org/grpc"

	"github.com/golusoris/golusoris/core/config"
	grpcmod "github.com/golusoris/golusoris/grpc"

	controllerv1 "github.com/VMAFx/vmafx/gen/go/controller"
	"github.com/VMAFx/vmafx/pkg/libvmaf"
)

// backendVendors maps every backend the node can advertise to the GPU vendor
// the controller records for it. "auto" is not listed: the scheduler matches a
// job's backend against the node's list, and a node must name the backend it
// actually runs.
var backendVendors = map[string]string{
	"cpu":   "cpu",
	"cuda":  "nvidia",
	"hip":   "amd",
	"sycl":  "intel",
	"metal": "apple",
}

// controllerClientParams groups the provider's dependencies.
type controllerClientParams struct {
	fx.In
	LC          fx.Lifecycle
	Config      *config.Config
	ConnFactory *grpcmod.ConnFactory
	Scorer      *libvmaf.Scorer
	Executor    *Executor
	Metrics     *nodeMetrics
	Log         *slog.Logger
}

// provideControllerClient builds the controller client when
// VMAFX_CONTROLLER_ADDR is set and binds it to the fx lifecycle. Without an
// address it returns nil and says so in the log: the node then serves direct
// VmafxScoring calls only. A configuration it cannot honour is a startup
// error.
func provideControllerClient(p controllerClientParams) (*controllerClient, error) {
	cc, err := loadControllerConfig(p.Config)
	if err != nil {
		return nil, err
	}
	if !cc.Enabled() {
		p.Log.Info("controller client disabled: VMAFX_CONTROLLER_ADDR is unset; the node serves direct VmafxScoring calls only")
		return nil, nil
	}
	if p.Scorer == nil {
		return nil, errors.New("VMAFX_CONTROLLER_ADDR is set but no vmaf scorer is available " +
			"(check VMAFX_VMAF_BINARY): a node that cannot score must not pull jobs")
	}
	capability, err := nodeCapability(p.Executor.backend, cc.Slots)
	if err != nil {
		return nil, err
	}
	conn, err := dialController(p.ConnFactory, cc)
	if err != nil {
		return nil, err
	}
	client := newControllerClient(cc, controllerv1.NewVmafxControllerClient(conn), p.Executor, capability, p.Log)
	client.metrics = p.Metrics
	p.Metrics.setSlots(cc.Slots)
	p.LC.Append(fx.Hook{
		OnStart: func(_ context.Context) error {
			client.start()
			return nil
		},
		OnStop: func(ctx context.Context) error {
			client.stop(ctx)
			return conn.Close()
		},
	})
	return client, nil
}

// nodeCapability is what RegisterNode and PullWork announce: the configured
// backend, its vendor and the slot count.
func nodeCapability(backend string, slots int) (*controllerv1.NodeCapability, error) {
	vendor, ok := backendVendors[backend]
	if !ok {
		return nil, fmt.Errorf("VMAFX_BACKEND=%q cannot be advertised to the controller; use one of cpu, cuda, hip, sycl, metal", backend)
	}
	return &controllerv1.NodeCapability{
		GpuVendor: vendor,
		Backends:  []string{backend},
		// #nosec G115 -- slots is validated to lie in [1, maxNodeSlots] (64).
		Concurrency: int32(slots),
	}, nil
}

// dialController opens the controller connection through the golusoris
// ConnFactory (OTel client handler, cmd/AGENTS.md invariant 3). TLS replaces
// the factory's plaintext default when configured; the bearer token rides as
// per-RPC credentials (pkg/controllerclient, shared with the operator).
func dialController(cf *grpcmod.ConnFactory, cc controllerConfig) (*googlegrpc.ClientConn, error) {
	opts, err := cc.Creds.DialOptions()
	if err != nil {
		return nil, err
	}
	// grpc.NewClient behind Dial connects lazily; the deadline bounds the call
	// itself, each RPC carries its own (controller.rpc_timeout).
	dialCtx, cancel := context.WithTimeout(context.Background(), cc.RPCTimeout)
	defer cancel()
	conn, err := cf.Dial(dialCtx, cc.Addr, opts...)
	if err != nil {
		return nil, fmt.Errorf("controller client: %w", err)
	}
	return conn, nil
}
