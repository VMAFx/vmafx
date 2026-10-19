// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// test/e2e/controller-ha/driver/main.go — the failover scenario of ADR-2350
// on a kind cluster, run as a Job next to the chart it tests.

// Command driver serves the scoring inputs over HTTP, submits jobs to the
// chart's vmafx-controller (PostgreSQL backend, two replicas), scales the
// node pool out, and once a job runs kills its node pod and a controller pod
// while every stream is held after its first frame. It then releases the
// streams, waits for every job to finish, and checks in the database that
// each job was completed by exactly one attempt and that the killed job
// needed a second. It prints one JSON summary line and exits non-zero on
// any failure.
package main

import (
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"log/slog"
	"os"
	"time"
)

// config is the scenario's command line.
type config struct {
	namespace  string
	controller string // gRPC address of the controller Service
	nodeDeploy string // node pool Deployment
	ctrlLabel  string // label selector of the controller pods
	listen     string // address of the fixture server
	fixtures   string // URL the nodes read the fixtures from (a scoring root)
	frames     int
	jobs       int
	deadline   time.Duration
}

func parseFlags() config {
	var c config
	flag.StringVar(&c.namespace, "namespace", os.Getenv("POD_NAMESPACE"), "namespace of the release")
	flag.StringVar(&c.controller, "controller", "vmafx-controller:9090", "controller gRPC address")
	flag.StringVar(&c.nodeDeploy, "node-deployment", "vmafx-node", "node pool Deployment")
	flag.StringVar(&c.ctrlLabel, "controller-selector", "app.kubernetes.io/component=controller", "controller pod selector")
	flag.StringVar(&c.listen, "listen", ":8080", "fixture server address")
	flag.StringVar(&c.fixtures, "fixtures-url", "http://vmafx-e2e-driver:8080/fixtures", "fixture URL as the nodes reach it")
	flag.IntVar(&c.frames, "frames", 48, "frames per clip")
	flag.IntVar(&c.jobs, "jobs", 4, "jobs to submit")
	flag.DurationVar(&c.deadline, "deadline", 10*time.Minute, "time the whole scenario may take")
	flag.Parse()
	return c
}

// summary is the scenario's result line.
type summary struct {
	OK               bool              `json:"ok"`
	Jobs             int               `json:"jobs"`
	KilledController string            `json:"killed_controller"`
	KilledNode       string            `json:"killed_node"`
	KilledJob        string            `json:"killed_job"`
	Attempts         map[string]int    `json:"attempts"`
	Outcomes         map[string]string `json:"outcomes"`
	Error            string            `json:"error,omitempty"`
}

func main() {
	cfg := parseFlags()
	log := slog.New(slog.NewTextHandler(os.Stderr, nil))
	ctx, cancel := context.WithTimeout(context.Background(), cfg.deadline)
	defer cancel()
	sum, err := run(ctx, cfg, log)
	if err != nil {
		sum.Error = err.Error()
	}
	sum.OK = err == nil
	out, merr := json.Marshal(sum)
	if merr != nil {
		log.Error("encode summary", "error", merr)
		os.Exit(2)
	}
	if _, werr := fmt.Println(string(out)); werr != nil || err != nil {
		os.Exit(1)
	}
}

// run is the scenario: serve, submit, scale out, kill mid-job, release,
// wait, verify.
func run(ctx context.Context, cfg config, log *slog.Logger) (summary, error) {
	sum := summary{Jobs: cfg.jobs}
	if cfg.namespace == "" {
		return sum, errors.New("no namespace (-namespace or POD_NAMESPACE)")
	}
	fx, err := newFixtures(cfg.frames)
	if err != nil {
		return sum, err
	}
	stop, err := fx.serve(ctx, cfg.listen)
	if err != nil {
		return sum, err
	}
	defer stop()
	if err := waitServed(ctx, cfg.fixtures); err != nil {
		return sum, err
	}
	cl, err := newClients(ctx, cfg)
	if err != nil {
		return sum, err
	}
	defer cl.close()
	return failover(ctx, cfg, cl, fx, log)
}

// failover runs the jobs through the kill and checks the outcome.
func failover(ctx context.Context, cfg config, cl *clients, fx *fixtures, log *slog.Logger) (summary, error) {
	sum := summary{Jobs: cfg.jobs}
	ids, err := cl.submitAll(ctx, cfg)
	if err != nil {
		return sum, err
	}
	log.Info("submitted", "jobs", ids)
	if err := cl.scaleNodes(ctx, cfg, 2); err != nil {
		return sum, err
	}
	victim, node, err := cl.waitRunning(ctx, ids)
	if err != nil {
		return sum, err
	}
	sum.KilledJob, sum.KilledNode = victim, node
	nodeAddr, ctrl, err := cl.killMidJob(ctx, cfg, node)
	if err != nil {
		return sum, err
	}
	sum.KilledController = ctrl
	fx.release(nodeAddr)
	log.Info("killed mid-job", "job", victim, "node", node, "controller", sum.KilledController)
	if err := cl.waitFinished(ctx, ids); err != nil {
		return sum, err
	}
	sum.Attempts, sum.Outcomes, err = cl.verifyOnce(ctx, ids, victim)
	return sum, err
}
