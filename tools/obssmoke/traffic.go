// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

package main

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"os"
	"path"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"

	vmafxv1 "github.com/VMAFx/vmafx/gen/go"
	controllerv1 "github.com/VMAFx/vmafx/gen/go/controller"
)

// The media the smoke script writes: a 576x324 8-bit 4:2:0 pair as Y4M (for
// the file-based calls) and raw (for ScoreStream).
const (
	frameWidth    = 576
	frameHeight   = 324
	streamFrames  = 12
	rpcTimeout    = 5 * time.Minute
	completedJobs = 2
)

// settle is how long the checks wait after the traffic: every rate() window
// holds samples on both sides of it (5 s scrapes), and the capacity
// forecasts' 1-minute subqueries hold the two points predict_linear and deriv
// need.
const settle = 150 * time.Second

// generateTraffic makes every family the dashboards read move: passing and
// failing Score requests on both front doors, a stream session that
// completes and one that fails, and jobs that complete, fail and are
// cancelled.
func generateTraffic(ctx context.Context, cfg config) error {
	for _, f := range []func(context.Context, config) error{scoreHTTP, scoreGRPC, scoreStream, failedStream, controllerJobs} {
		if err := f(ctx, cfg); err != nil {
			return err
		}
	}
	return nil
}

// scoreHTTP sends two Score requests that pass and one that fails.
func scoreHTTP(ctx context.Context, cfg config) error {
	pass, err := scoreBody(cfg, "ref.y4m", "dis.y4m")
	if err != nil {
		return err
	}
	fail, err := scoreBody(cfg, "ref.y4m", "missing.y4m")
	if err != nil {
		return err
	}
	for i, body := range []string{pass, pass, fail} {
		status, raw, err := httpCall{method: http.MethodPost, url: cfg.serverHTTP + "/v1/score", body: body}.do(ctx)
		if err != nil {
			return err
		}
		if wantOK := i < 2; (status == http.StatusOK) != wantOK {
			return fmt.Errorf("POST /v1/score %s: HTTP %d %.300s", body, status, raw)
		}
	}
	return nil
}

func scoreBody(cfg config, ref, dis string) (string, error) {
	b, err := json.Marshal(map[string]string{"reference": path.Join(cfg.media, ref), "distorted": path.Join(cfg.media, dis)})
	if err != nil {
		return "", fmt.Errorf("score request body: %w", err)
	}
	return string(b), nil
}

// dial opens a plaintext gRPC connection inside the example's network.
func dial(addr string) (*grpc.ClientConn, error) {
	conn, err := grpc.NewClient(addr, grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		return nil, fmt.Errorf("dial %s: %w", addr, err)
	}
	return conn, nil
}

// scoreGRPC scores the pair once through the controller's VmafxScoring
// service, so the controller's Score families move too.
func scoreGRPC(ctx context.Context, cfg config) error {
	conn, err := dial(cfg.controllerGRPC)
	if err != nil {
		return err
	}
	defer closeLogged(conn, "gRPC connection")
	ctx, cancel := context.WithTimeout(ctx, rpcTimeout)
	defer cancel()
	_, err = vmafxv1.NewVmafxScoringClient(conn).Score(ctx, &vmafxv1.ScoreRequest{
		Reference: path.Join(cfg.media, "ref.y4m"), Distorted: path.Join(cfg.media, "dis.y4m"),
	})
	if err != nil {
		return fmt.Errorf("controller Score: %w", err)
	}
	return nil
}

// scoreStream runs one ScoreStream session of streamFrames frames on the
// server and reads its aggregate.
func scoreStream(ctx context.Context, cfg config) error {
	ref, dis, err := readFrames(cfg)
	if err != nil {
		return err
	}
	conn, err := dial(cfg.serverGRPC)
	if err != nil {
		return err
	}
	defer closeLogged(conn, "gRPC connection")
	ctx, cancel := context.WithTimeout(ctx, rpcTimeout)
	defer cancel()
	stream, err := vmafxv1.NewVmafxScoringClient(conn).ScoreStream(ctx)
	if err != nil {
		return fmt.Errorf("ScoreStream: %w", err)
	}
	if err := sendFrames(stream, ref, dis); err != nil {
		return err
	}
	return drainStream(stream)
}

// waitScrapes lets Prometheus scrape the traffic's effects several times.
func waitScrapes(ctx context.Context, _ config) error {
	select {
	case <-ctx.Done():
		return ctx.Err()
	case <-time.After(settle):
		return nil
	}
}

// failedStream sends a frame of the wrong size, which the server refuses:
// the session ends with outcome "failed".
func failedStream(ctx context.Context, cfg config) error {
	conn, err := dial(cfg.serverGRPC)
	if err != nil {
		return err
	}
	defer closeLogged(conn, "gRPC connection")
	ctx, cancel := context.WithTimeout(ctx, rpcTimeout)
	defer cancel()
	stream, err := vmafxv1.NewVmafxScoringClient(conn).ScoreStream(ctx)
	if err != nil {
		return fmt.Errorf("ScoreStream: %w", err)
	}
	short := [][]byte{make([]byte, 16)}
	if err := sendFrames(stream, short, short); err != nil {
		return nil // the server may close the stream before the client half-closes
	}
	if err := drainStream(stream); err == nil {
		return errors.New("ScoreStream accepted a frame of the wrong size")
	}
	return nil
}

// readFrames reads streamFrames frames of the raw pair.
func readFrames(cfg config) ([][]byte, [][]byte, error) {
	size := frameWidth * frameHeight * 3 / 2
	read := func(name string) ([][]byte, error) {
		raw, err := os.ReadFile(path.Join(cfg.media, name)) // #nosec G304 -- the smoke media
		if err != nil {
			return nil, err
		}
		if len(raw) < size*streamFrames {
			return nil, fmt.Errorf("%s holds %d bytes, want %d frames of %d", name, len(raw), streamFrames, size)
		}
		var frames [][]byte
		for i := range streamFrames {
			frames = append(frames, raw[i*size:(i+1)*size])
		}
		return frames, nil
	}
	ref, err := read("ref.yuv")
	if err != nil {
		return nil, nil, err
	}
	dis, err := read("dis.yuv")
	return ref, dis, err
}

type scoreStreamClient = grpc.BidiStreamingClient[vmafxv1.ScoreStreamRequest, vmafxv1.ScoreStreamResponse]

func sendFrames(stream scoreStreamClient, ref, dis [][]byte) error {
	if len(dis) != len(ref) {
		return fmt.Errorf("ScoreStream: %d reference frames, %d distorted frames", len(ref), len(dis))
	}
	cfg := &vmafxv1.ScoreStreamRequest{Payload: &vmafxv1.ScoreStreamRequest_Config{Config: &vmafxv1.StreamConfig{
		Width: frameWidth, Height: frameHeight, PixelFormat: vmafxv1.PixelFormat_PIXEL_FORMAT_YUV420P, FrameCountHint: streamFrames,
	}}}
	if err := stream.Send(cfg); err != nil {
		return fmt.Errorf("ScoreStream config: %w", err)
	}
	for i := range ref {
		pair := &vmafxv1.ScoreStreamRequest{Payload: &vmafxv1.ScoreStreamRequest_FramePair{FramePair: &vmafxv1.FramePair{
			FrameIndex: uint32(i), RawReference: ref[i], RawDistorted: dis[i], // #nosec G115 G602 -- i < streamFrames; len(dis) == len(ref) is checked above
		}}}
		if err := stream.Send(pair); err != nil {
			return fmt.Errorf("ScoreStream frame %d: %w", i, err)
		}
	}
	if err := stream.CloseSend(); err != nil {
		return fmt.Errorf("ScoreStream close: %w", err)
	}
	return nil
}

// drainStream reads the per-frame scores until the aggregate and EOF.
func drainStream(stream scoreStreamClient) error {
	aggregate := false
	for range streamFrames + 2 {
		msg, err := stream.Recv()
		if errors.Is(err, io.EOF) {
			break
		}
		if err != nil {
			return fmt.Errorf("ScoreStream receive: %w", err)
		}
		aggregate = aggregate || msg.GetAggregate() != nil
	}
	if !aggregate {
		return errors.New("ScoreStream ended without an aggregate score")
	}
	return nil
}

// controllerJobs submits jobs that complete, one that fails (its distorted
// input does not exist) and one it cancels, and waits until all are done.
func controllerJobs(ctx context.Context, cfg config) error {
	conn, err := dial(cfg.controllerGRPC)
	if err != nil {
		return err
	}
	defer closeLogged(conn, "gRPC connection")
	client := controllerv1.NewVmafxControllerClient(conn)
	var ids []string
	for i := range completedJobs + 2 {
		dis := "dis.y4m"
		if i == completedJobs {
			dis = "missing.y4m"
		}
		id, err := submit(ctx, client, cfg, dis)
		if err != nil {
			return err
		}
		ids = append(ids, id)
	}
	if err := cancelJob(ctx, client, ids[len(ids)-1]); err != nil {
		return err
	}
	return waitJobs(ctx, client, ids)
}

func submit(ctx context.Context, client controllerv1.VmafxControllerClient, cfg config, dis string) (string, error) {
	ctx, cancel := context.WithTimeout(ctx, requestTimeout)
	defer cancel()
	resp, err := client.SubmitJob(ctx, &controllerv1.SubmitJobRequest{Scoring: &controllerv1.ScoringParams{
		Reference: path.Join(cfg.media, "ref.y4m"), Distorted: path.Join(cfg.media, dis),
	}})
	if err != nil {
		return "", fmt.Errorf("SubmitJob: %w", err)
	}
	return resp.GetJobId(), nil
}

func cancelJob(ctx context.Context, client controllerv1.VmafxControllerClient, id string) error {
	ctx, cancel := context.WithTimeout(ctx, requestTimeout)
	defer cancel()
	if _, err := client.CancelJob(ctx, &controllerv1.CancelJobRequest{JobId: id}); err != nil {
		return fmt.Errorf("CancelJob %s: %w", id, err)
	}
	return nil
}

// waitJobs waits until every job reached a final status.
func waitJobs(ctx context.Context, client controllerv1.VmafxControllerClient, ids []string) error {
	final := map[controllerv1.JobStatus]bool{
		controllerv1.JobStatus_COMPLETED: true, controllerv1.JobStatus_FAILED: true, controllerv1.JobStatus_CANCELLED: true,
	}
	for _, id := range ids {
		err := poll(ctx, func(ctx context.Context) (bool, error) {
			rctx, cancel := context.WithTimeout(ctx, requestTimeout)
			defer cancel()
			job, err := client.GetJob(rctx, &controllerv1.GetJobRequest{JobId: id})
			if err != nil {
				return false, err
			}
			slog.Debug("job", "id", id, "status", job.GetStatus())
			return final[job.GetStatus()], fmt.Errorf("job %s is %s", id, job.GetStatus())
		})
		if err != nil {
			return err
		}
	}
	return nil
}
