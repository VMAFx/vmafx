// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// test/e2e/controller-ha/driver/scenario.go — the failover steps: wait for a
// running job, kill its node and a controller, wait, check exactly once.

package main

import (
	"context"
	"errors"
	"fmt"
	"math"
	"os"

	"github.com/jackc/pgx/v5"

	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"

	controllerv1 "github.com/VMAFx/vmafx/gen/go/controller"
)

// getJob reads one job through the controller.
func (c *clients) getJob(ctx context.Context, id string) (*controllerv1.Job, error) {
	return call(ctx, func(cctx context.Context) (*controllerv1.Job, error) {
		return c.ctrl.GetJob(cctx, &controllerv1.GetJobRequest{JobId: id})
	})
}

// waitRunning waits until one of ids runs and returns it and its node. A
// job that ends before any ran fails the scenario: every stream is held, so
// no job can complete before the kill.
func (c *clients) waitRunning(ctx context.Context, ids []string) (string, string, error) {
	for range maxPolls {
		for _, id := range ids {
			j, err := c.getJob(ctx, id)
			if err != nil {
				return "", "", fmt.Errorf("read job %s: %w", id, err)
			}
			switch {
			case j.GetStatus() == controllerv1.JobStatus_RUNNING && j.GetAssignedNode() != "":
				return id, j.GetAssignedNode(), nil
			case terminal(j.GetStatus()):
				return "", "", fmt.Errorf("job %s ended %s before the kill: %s", id, j.GetStatus(), j.GetError())
			default:
			}
		}
		if err := wait(ctx, pollEvery); err != nil {
			return "", "", err
		}
	}
	return "", "", fmt.Errorf("no job started within %d polls", maxPolls)
}

// killMidJob deletes the node pod named node (the node ID is its pod name)
// and one controller pod, at once, and returns the node pod's address and
// the controller pod's name.
func (c *clients) killMidJob(ctx context.Context, cfg config, node string) (string, string, error) {
	cctx, cancel := context.WithTimeout(ctx, callTimeout)
	defer cancel()
	pods, err := c.kube.CoreV1().Pods(c.ns).List(cctx, metav1.ListOptions{LabelSelector: cfg.ctrlLabel})
	if err != nil || len(pods.Items) < 2 {
		return "", "", fmt.Errorf("controller pods (need 2): %d found, %w", len(pods.Items), err)
	}
	nodePod, err := c.kube.CoreV1().Pods(c.ns).Get(cctx, node, metav1.GetOptions{})
	if err != nil || nodePod.Status.PodIP == "" {
		return "", "", fmt.Errorf("address of node pod %s: %q, %w", node, nodePod.Status.PodIP, err)
	}
	now := int64(0)
	victim := pods.Items[0].Name
	for _, name := range []string{node, victim} {
		if err := c.kube.CoreV1().Pods(c.ns).Delete(cctx, name, metav1.DeleteOptions{GracePeriodSeconds: &now}); err != nil {
			return "", "", fmt.Errorf("delete pod %s: %w", name, err)
		}
	}
	return nodePod.Status.PodIP, victim, nil
}

// waitFinished waits until every job completed and its score is finite; a
// failed or cancelled job fails the scenario.
func (c *clients) waitFinished(ctx context.Context, ids []string) error {
	for range maxPolls {
		done, err := c.countCompleted(ctx, ids)
		if err != nil || done == len(ids) {
			return err
		}
		if err := wait(ctx, pollEvery); err != nil {
			return fmt.Errorf("%d of %d jobs completed: %w", done, len(ids), err)
		}
	}
	return fmt.Errorf("jobs did not finish within %d polls", maxPolls)
}

// countCompleted counts the completed jobs of ids and refuses any other end.
func (c *clients) countCompleted(ctx context.Context, ids []string) (int, error) {
	done := 0
	for _, id := range ids {
		j, err := c.getJob(ctx, id)
		if err != nil {
			return 0, fmt.Errorf("read job %s: %w", id, err)
		}
		switch j.GetStatus() {
		case controllerv1.JobStatus_COMPLETED:
			if s := j.GetFinalScore(); math.IsNaN(s) || math.IsInf(s, 0) {
				return 0, fmt.Errorf("job %s completed with score %v", id, s)
			}
			done++
		case controllerv1.JobStatus_FAILED, controllerv1.JobStatus_CANCELLED:
			return 0, fmt.Errorf("job %s ended %s: %s", id, j.GetStatus(), j.GetError())
		default:
		}
	}
	return done, nil
}

// terminal reports whether a job status is final.
func terminal(s controllerv1.JobStatus) bool {
	return s == controllerv1.JobStatus_COMPLETED || s == controllerv1.JobStatus_FAILED ||
		s == controllerv1.JobStatus_CANCELLED
}

// attemptsQuery lists each job's attempts and their outcomes; maintenance
// mode lets the scenario's role read every tenant's rows (row-level security).
const attemptsQuery = `SELECT job_id::text, count(*), count(*) FILTER (WHERE outcome = 'completed'),
	string_agg(coalesce(outcome, 'open'), ',' ORDER BY attempt)
	FROM job_attempts WHERE job_id::text = ANY($1) GROUP BY job_id`

// verifyOnce checks in the database that every job was completed by exactly
// one attempt and that the killed job needed more than one.
func (c *clients) verifyOnce(ctx context.Context, ids []string, killed string) (map[string]int, map[string]string, error) {
	tx, err := c.db.Begin(ctx)
	if err != nil {
		return nil, nil, fmt.Errorf("database: %w", err)
	}
	defer func() {
		if rerr := tx.Rollback(ctx); rerr != nil && !errors.Is(rerr, pgx.ErrTxClosed) {
			fmt.Fprintf(os.Stderr, "roll back the read-only transaction: %v\n", rerr)
		}
	}()
	if _, err := tx.Exec(ctx, "SELECT set_config('vmafx.maintenance', 'on', true)"); err != nil {
		return nil, nil, fmt.Errorf("maintenance mode: %w", err)
	}
	rows, err := tx.Query(ctx, attemptsQuery, ids)
	if err != nil {
		return nil, nil, fmt.Errorf("read attempts: %w", err)
	}
	defer rows.Close()
	attempts, outcomes := map[string]int{}, map[string]string{}
	for rows.Next() {
		var (
			id, outs        string
			total, finished int
		)
		if err := rows.Scan(&id, &total, &finished, &outs); err != nil {
			return nil, nil, fmt.Errorf("scan attempts: %w", err)
		}
		attempts[id], outcomes[id] = total, outs
		if finished != 1 {
			return attempts, outcomes, fmt.Errorf("job %s has %d completed attempts (%s), want exactly 1", id, finished, outs)
		}
	}
	if err := rows.Err(); err != nil {
		return nil, nil, fmt.Errorf("read attempts: %w", err)
	}
	return attempts, outcomes, checkKilled(ids, killed, attempts)
}

// checkKilled refuses a run where a job has no attempt row or where the
// killed job finished on its first attempt (the kill did not hit it).
func checkKilled(ids []string, killed string, attempts map[string]int) error {
	for _, id := range ids {
		if attempts[id] == 0 {
			return fmt.Errorf("job %s has no attempt", id)
		}
	}
	if attempts[killed] < 2 {
		return fmt.Errorf("job %s, killed mid-run, finished in %d attempt(s): the kill did not interrupt it", killed, attempts[killed])
	}
	return nil
}
