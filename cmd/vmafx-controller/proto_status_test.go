// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/proto_status_test.go — table tests for protoStatusToQueue,
// covering all branches that were previously at 33% coverage.

//go:build cgo

package main

import (
	"testing"

	"github.com/VMAFx/vmafx/cmd/vmafx-controller/queue"
	controllerv1 "github.com/VMAFx/vmafx/gen/go/controller"
)

func TestProtoStatusToQueue_AllBranches(t *testing.T) {
	t.Parallel()
	cases := []struct {
		in   controllerv1.JobStatus
		want string
	}{
		{controllerv1.JobStatus_RUNNING, queue.StatusRunning},
		{controllerv1.JobStatus_COMPLETED, queue.StatusCompleted},
		{controllerv1.JobStatus_FAILED, queue.StatusFailed},
		{controllerv1.JobStatus_CANCELLED, queue.StatusCancelled},
		// PENDING and any unknown value fall through to the default branch.
		{controllerv1.JobStatus_PENDING, queue.StatusPending},
		// Unrecognised enum value (e.g. a future addition) hits the default.
		{controllerv1.JobStatus(9999), queue.StatusPending},
	}
	for _, tc := range cases {
		got := protoStatusToQueue(tc.in)
		if got != tc.want {
			t.Errorf("protoStatusToQueue(%v) = %q, want %q", tc.in, got, tc.want)
		}
	}
}
