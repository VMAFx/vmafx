// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/wire_test.go — gRPC WIRE round-trip test for the
// controllerv1 protobuf types (ADR-1119).
//
// REGRESSION GUARD: the controllerv1 *.pb.go files were once hand-written and
// did NOT implement proto.Message (no protoimpl/ProtoReflect), so every
// VmafxController RPC failed to marshal at runtime even though the enum-only
// unit tests passed. This test stands up a real grpc.Server over an in-process
// bufconn pipe and exercises SubmitJob + StreamJobs end-to-end, so a future
// regression to non-marshalable types fails loudly here instead of in
// production. It deliberately uses a minimal stub server (no queue/scheduler
// deps) — the point under test is the protobuf wire codec, not business logic.

//go:build cgo

package main

import (
	"context"
	"io"
	"net"
	"testing"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/test/bufconn"

	controllerv1 "github.com/VMAFx/vmafx/gen/go/controller"
)

// wireStubServer is a minimal VmafxController that echoes its inputs back so the
// test can assert the protobuf types survive a real marshal → wire → unmarshal
// round-trip. It is NOT the production server.
type wireStubServer struct {
	controllerv1.UnimplementedVmafxControllerServer
}

func (wireStubServer) SubmitJob(_ context.Context, req *controllerv1.SubmitJobRequest) (*controllerv1.SubmitJobResponse, error) {
	// Echo a deterministic id derived from the request so the test can verify
	// the request fields actually crossed the wire intact.
	return &controllerv1.SubmitJobResponse{JobId: "job-" + req.GetScoring().GetReference()}, nil
}

func (wireStubServer) GetJob(_ context.Context, req *controllerv1.GetJobRequest) (*controllerv1.Job, error) {
	return &controllerv1.Job{
		Id:             req.GetJobId(),
		Status:         controllerv1.JobStatus_COMPLETED,
		PartialResults: []float64{1.5, 2.5, 3.5},
		Scoring:        &controllerv1.ScoringParams{Reference: "ref.yuv", Distorted: "dis.yuv"},
	}, nil
}

func (wireStubServer) StreamJobs(req *controllerv1.StreamJobsRequest, stream grpc.ServerStreamingServer[controllerv1.Job]) error {
	// Emit one Job per requested status filter so the stream + the repeated
	// enum + the nested message all round-trip.
	for _, st := range req.GetStatusFilter() {
		if err := stream.Send(&controllerv1.Job{Id: "j", Status: st}); err != nil {
			return err
		}
	}
	return nil
}

func newWireTestConn(t *testing.T) (controllerv1.VmafxControllerClient, func()) {
	t.Helper()
	lis := bufconn.Listen(1 << 20)
	srv := grpc.NewServer()
	controllerv1.RegisterVmafxControllerServer(srv, wireStubServer{})
	go func() { _ = srv.Serve(lis) }()

	dialer := grpc.WithContextDialer(func(_ context.Context, _ string) (net.Conn, error) {
		return lis.DialContext(context.Background())
	})
	conn, err := grpc.NewClient("passthrough:///bufnet", dialer, grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		srv.Stop()
		t.Fatalf("grpc.NewClient: %v", err)
	}
	cleanup := func() {
		_ = conn.Close()
		srv.Stop()
	}
	return controllerv1.NewVmafxControllerClient(conn), cleanup
}

// TestControllerProtoMarshalsOverWire is the core regression guard: if the
// controllerv1 types ever stop implementing proto.Message, SubmitJob fails to
// marshal and this test errors.
func TestControllerProtoMarshalsOverWire(t *testing.T) {
	client, cleanup := newWireTestConn(t)
	defer cleanup()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	resp, err := client.SubmitJob(ctx, &controllerv1.SubmitJobRequest{
		Scoring: &controllerv1.ScoringParams{Reference: "r.yuv", Distorted: "d.yuv", Model: "vmaf_v0.6.1", Backend: "cpu"},
	})
	if err != nil {
		t.Fatalf("SubmitJob over the wire failed (proto not marshalable?): %v", err)
	}
	if resp.GetJobId() != "job-r.yuv" {
		t.Fatalf("SubmitJob round-trip mismatch: job_id=%q, want %q", resp.GetJobId(), "job-r.yuv")
	}
}

// TestControllerJobAndEnumRoundTrip exercises the nested message + the JobStatus
// enum + a repeated double field across the wire.
func TestControllerJobAndEnumRoundTrip(t *testing.T) {
	client, cleanup := newWireTestConn(t)
	defer cleanup()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	job, err := client.GetJob(ctx, &controllerv1.GetJobRequest{JobId: "abc"})
	if err != nil {
		t.Fatalf("GetJob over the wire failed: %v", err)
	}
	if job.GetId() != "abc" {
		t.Errorf("job id = %q, want abc", job.GetId())
	}
	if job.GetStatus() != controllerv1.JobStatus_COMPLETED {
		t.Errorf("job status = %v, want COMPLETED", job.GetStatus())
	}
	if got := job.GetPartialResults(); len(got) != 3 || got[2] != 3.5 {
		t.Errorf("partial_results = %v, want [1.5 2.5 3.5]", got)
	}
	if job.GetScoring().GetReference() != "ref.yuv" {
		t.Errorf("nested scoring.reference = %q, want ref.yuv", job.GetScoring().GetReference())
	}
}

// TestControllerStreamRoundTrip exercises the server-streaming RPC + a repeated
// JobStatus filter, all of which marshal through the generated stream codec.
func TestControllerStreamRoundTrip(t *testing.T) {
	client, cleanup := newWireTestConn(t)
	defer cleanup()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	stream, err := client.StreamJobs(ctx, &controllerv1.StreamJobsRequest{
		StatusFilter: []controllerv1.JobStatus{controllerv1.JobStatus_RUNNING, controllerv1.JobStatus_PENDING},
	})
	if err != nil {
		t.Fatalf("StreamJobs over the wire failed: %v", err)
	}
	var got []controllerv1.JobStatus
	for {
		job, err := stream.Recv()
		if err == io.EOF {
			break
		}
		if err != nil {
			t.Fatalf("stream Recv: %v", err)
		}
		got = append(got, job.GetStatus())
	}
	if len(got) != 2 || got[0] != controllerv1.JobStatus_RUNNING || got[1] != controllerv1.JobStatus_PENDING {
		t.Errorf("streamed statuses = %v, want [RUNNING PENDING]", got)
	}
}
