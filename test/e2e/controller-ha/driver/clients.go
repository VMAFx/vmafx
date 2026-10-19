// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// test/e2e/controller-ha/driver/clients.go — the controller, Kubernetes and
// database clients of the failover scenario.

package main

import (
	"context"
	"errors"
	"fmt"
	"os"
	"time"

	"github.com/jackc/pgx/v5/pgxpool"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/status"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/client-go/kubernetes"
	"k8s.io/client-go/rest"

	controllerv1 "github.com/VMAFx/vmafx/gen/go/controller"
)

// Bounds of the scenario's waits and retries (HISS-02).
const (
	callTimeout = 10 * time.Second
	pollEvery   = 2 * time.Second
	maxPolls    = 600
	maxRetries  = 30
)

// clients are the scenario's connections.
type clients struct {
	conn *grpc.ClientConn
	ctrl controllerv1.VmafxControllerClient
	kube kubernetes.Interface
	db   *pgxpool.Pool
	ns   string
}

// newClients connects to the controller Service, the API server (in-cluster
// service account) and the database (VMAFX_DB_DSN).
func newClients(ctx context.Context, cfg config) (*clients, error) {
	conn, err := grpc.NewClient("dns:///"+cfg.controller,
		grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		return nil, fmt.Errorf("dial controller: %w", err)
	}
	rc, err := rest.InClusterConfig()
	if err != nil {
		return nil, errors.Join(fmt.Errorf("in-cluster config: %w", err), conn.Close())
	}
	kube, err := kubernetes.NewForConfig(rc)
	if err != nil {
		return nil, errors.Join(fmt.Errorf("kubernetes client: %w", err), conn.Close())
	}
	db, err := pgxpool.New(ctx, os.Getenv("VMAFX_DB_DSN"))
	if err != nil {
		return nil, errors.Join(fmt.Errorf("database: %w", err), conn.Close())
	}
	return &clients{conn: conn, ctrl: controllerv1.NewVmafxControllerClient(conn), kube: kube, db: db, ns: cfg.namespace}, nil
}

func (c *clients) close() {
	c.db.Close()
	if err := c.conn.Close(); err != nil {
		fmt.Fprintf(os.Stderr, "close controller connection: %v\n", err)
	}
}

// call runs one controller RPC with a timeout, retrying while the controller
// is unreachable (a replica that just died, a new one starting).
func call[T any](ctx context.Context, fn func(context.Context) (T, error)) (T, error) {
	var (
		out T
		err error
	)
	for range maxRetries {
		cctx, cancel := context.WithTimeout(ctx, callTimeout)
		out, err = fn(cctx)
		cancel()
		if code := status.Code(err); code != codes.Unavailable && code != codes.DeadlineExceeded {
			return out, err
		}
		if werr := wait(ctx, pollEvery); werr != nil {
			return out, errors.Join(err, werr)
		}
	}
	return out, fmt.Errorf("controller unreachable after %d tries: %w", maxRetries, err)
}

// wait sleeps d or until ctx ends.
func wait(ctx context.Context, d time.Duration) error {
	t := time.NewTimer(d)
	defer t.Stop()
	select {
	case <-ctx.Done():
		return fmt.Errorf("scenario deadline: %w", ctx.Err())
	case <-t.C:
		return nil
	}
}

// submitAll submits cfg.jobs scoring jobs and returns their IDs.
func (c *clients) submitAll(ctx context.Context, cfg config) ([]string, error) {
	ids := make([]string, 0, cfg.jobs)
	for range cfg.jobs {
		resp, err := call(ctx, func(cctx context.Context) (*controllerv1.SubmitJobResponse, error) {
			return c.ctrl.SubmitJob(cctx, &controllerv1.SubmitJobRequest{Scoring: &controllerv1.ScoringParams{
				Reference: cfg.fixtures + "/ref.y4m", Distorted: cfg.fixtures + "/dis.y4m", Backend: "cpu",
			}})
		})
		if err != nil {
			return nil, fmt.Errorf("submit: %w", err)
		}
		ids = append(ids, resp.GetJobId())
	}
	return ids, nil
}

// scaleNodes sets the node pool's replica count.
func (c *clients) scaleNodes(ctx context.Context, cfg config, replicas int32) error {
	cctx, cancel := context.WithTimeout(ctx, callTimeout)
	defer cancel()
	scale, err := c.kube.AppsV1().Deployments(c.ns).GetScale(cctx, cfg.nodeDeploy, metav1.GetOptions{})
	if err != nil {
		return fmt.Errorf("read node pool scale: %w", err)
	}
	scale.Spec.Replicas = replicas
	if _, err := c.kube.AppsV1().Deployments(c.ns).UpdateScale(cctx, cfg.nodeDeploy, scale, metav1.UpdateOptions{}); err != nil {
		return fmt.Errorf("scale node pool to %d: %w", replicas, err)
	}
	return nil
}
