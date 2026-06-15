// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT
//
// cmd/vmafx-node/online_feedback.go — Go-side client that emits
// (features, true_score) pairs to the co-located Python sidecar over a
// Unix domain socket.
//
// Wire protocol: newline-delimited JSON.
//
//	→ {"job_id": "...", "features": [...], "true_score": 42.7}
//	← {"ok": true, "step": 1234, "trained": false}
//	← {"ok": false, "error": "message"}
//
// The client is intentionally fire-and-forget from the scoring path:
// FeedbackClient.Send() is non-blocking — it enqueues into a bounded
// ring buffer (default 1000 entries).  A background goroutine drains
// the buffer and delivers to the sidecar.  When the sidecar is down,
// the buffer absorbs bursts up to its capacity; entries beyond that are
// silently dropped and counted via a Prometheus counter.
//
// The socket path defaults to /tmp/vmafx-sidecar.sock and is overridden
// by VMAFX_SIDECAR_SOCKET.  The vmafx-node and sidecar containers share
// an emptyDir volume mounted at /tmp.
//
// ADR-0781: sidecar online training — SGD + EMA + replay buffer.
package main

import (
	"bufio"
	"context"
	"encoding/json"
	"fmt"
	"log/slog"
	"net"
	"os"
	"sync"
	"sync/atomic"
	"time"
)

const (
	// feedbackSocketEnv is the env var that overrides the default socket path.
	feedbackSocketEnv = "VMAFX_SIDECAR_SOCKET"

	// feedbackSocketDefault is the default Unix socket path.
	// Must match VMAFX_SIDECAR_SOCKET in online_trainer.py.
	feedbackSocketDefault = "/tmp/vmafx-sidecar.sock"

	// feedbackQueueCap is the maximum number of enqueued feedback messages.
	// When full, new messages are dropped and the drop counter increments.
	feedbackQueueCap = 1000

	// feedbackDialTimeout is how long the background drainer waits for a
	// connection to the sidecar before retrying.
	feedbackDialTimeout = 5 * time.Second

	// feedbackWriteTimeout is the per-message write deadline on the socket.
	feedbackWriteTimeout = 2 * time.Second

	// feedbackRetryBase is the initial backoff after the first failed dial.
	// The interval doubles on each consecutive failure up to feedbackRetryMax
	// (ADR-1049 — replaces the old fixed feedbackRetryInterval constant).
	feedbackRetryBase = 2 * time.Second

	// feedbackRetryMax caps the exponential backoff to avoid log noise during
	// extended sidecar outages (e.g. rolling restarts).
	feedbackRetryMax = 2 * time.Minute
)

// FeedbackMessage is the JSON envelope sent to the sidecar.
type FeedbackMessage struct {
	JobID     string    `json:"job_id"`
	Features  []float32 `json:"features"`
	TrueScore float32   `json:"true_score"`
}

// feedbackAck is the JSON envelope received from the sidecar.
type feedbackAck struct {
	OK         bool    `json:"ok"`
	Step       int64   `json:"step"`
	Trained    bool    `json:"trained"`
	Loss       float64 `json:"loss,omitempty"`
	Checkpoint string  `json:"checkpoint,omitempty"`
	Error      string  `json:"error,omitempty"`
}

// FeedbackClient is a non-blocking Unix-socket client for the online
// training sidecar.  It is safe for concurrent use by multiple goroutines.
//
// Lifecycle (ADR-1119): construction (NewFeedbackClient) does NOT spawn the
// drainer goroutine — call Start() to launch it (wired to an fx OnStart hook)
// and Close() to stop and await it (wired to an fx OnStop hook). The drainer is
// bound to an internal, Close-owned context rather than a caller-supplied one,
// so its lifetime is governed entirely by the Start/Close pair and no goroutine
// leaks past Close().
type FeedbackClient struct {
	socketPath string
	queue      chan *FeedbackMessage
	log        *slog.Logger

	// drainCtx / cancel own the drainer goroutine's lifetime. cancel is wired
	// in Start() and fired by Close() to stop the drainer after the current
	// in-flight message completes.
	drainCtx context.Context
	cancel   context.CancelFunc

	// startOnce / closeOnce make Start and Close idempotent: Start launches the
	// drainer at most once, Close stops it at most once.
	startOnce sync.Once
	closeOnce sync.Once
	started   atomic.Bool

	// done is closed when the drainer goroutine has returned.  Close blocks
	// on this so callers see a synchronous shutdown.
	done chan struct{}

	// Metrics — updated atomically so they can be read without locking.
	dropped   atomic.Int64
	delivered atomic.Int64
}

// NewFeedbackClient creates a FeedbackClient. It does NOT start the background
// drainer — call Start() (typically from an fx OnStart hook) to launch it. The
// queue is live immediately, so Send() may be called before Start(); enqueued
// messages drain once the drainer runs.
func NewFeedbackClient(log *slog.Logger) *FeedbackClient {
	path := os.Getenv(feedbackSocketEnv)
	if path == "" {
		path = feedbackSocketDefault
	}
	// Guard against a nil logger so callers (and tests) cannot accidentally
	// trigger a panic deep inside the drainer.
	if log == nil {
		log = slog.Default()
	}
	drainCtx, cancel := context.WithCancel(context.Background())
	return &FeedbackClient{
		socketPath: path,
		queue:      make(chan *FeedbackMessage, feedbackQueueCap),
		log:        log,
		drainCtx:   drainCtx,
		cancel:     cancel,
		done:       make(chan struct{}),
	}
}

// Start launches the background drainer goroutine. It is safe to call multiple
// times; only the first call starts the drainer. Calling Start after Close is a
// no-op (the internal context is already cancelled, so a drainer would exit
// immediately — but it is not launched to avoid closing fc.done twice).
func (fc *FeedbackClient) Start() {
	fc.startOnce.Do(func() {
		// If Close already cancelled the context (Start-after-Close), do not
		// launch a goroutine; close done so Close's wait remains correct.
		if fc.drainCtx.Err() != nil {
			close(fc.done)
			return
		}
		fc.started.Store(true)
		go func() {
			defer close(fc.done)
			fc.drainLoop(fc.drainCtx)
		}()
	})
}

// Close stops the background drainer goroutine and waits for it to return.
// It is safe to call multiple times; subsequent calls are no-ops.
//
// Close is correct whether or not Start was ever called: if the drainer was
// never started, Close cancels the context and returns without blocking; if it
// was started, Close cancels and awaits the goroutine. Close does not flush
// in-flight queued messages — any messages still in fc.queue at shutdown are
// discarded (consistent with the fire-and-forget telemetry contract documented
// in the package header).
func (fc *FeedbackClient) Close() {
	fc.cancel()
	fc.closeOnce.Do(func() {
		if fc.started.Load() {
			<-fc.done
		}
	})
}

// Send enqueues a feedback message for async delivery.
// It never blocks: if the queue is full the message is dropped and the
// drop counter is incremented.  Returns true when the message was enqueued,
// false when it was dropped.
func (fc *FeedbackClient) Send(msg *FeedbackMessage) bool {
	select {
	case fc.queue <- msg:
		return true
	default:
		fc.dropped.Add(1)
		fc.log.Debug("sidecar feedback queue full — message dropped",
			slog.String("job_id", msg.JobID))
		return false
	}
}

// Dropped returns the total number of messages dropped due to queue overflow
// or a disconnected sidecar that is not recovering fast enough.
func (fc *FeedbackClient) Dropped() int64 { return fc.dropped.Load() }

// Delivered returns the total number of messages successfully sent to the sidecar.
func (fc *FeedbackClient) Delivered() int64 { return fc.delivered.Load() }

// drainLoop connects to the sidecar and forwards queued messages.
// On connection failure it waits with exponential backoff (feedbackRetryBase …
// feedbackRetryMax) before retrying.  A successful connection resets the
// backoff to the base interval (ADR-1049).
// It exits when ctx is cancelled.
func (fc *FeedbackClient) drainLoop(ctx context.Context) {
	backoff := feedbackRetryBase
	for {
		if ctx.Err() != nil {
			return
		}
		conn, err := fc.dial(ctx)
		if err != nil {
			if ctx.Err() != nil {
				return
			}
			fc.log.Warn("sidecar not reachable — retrying",
				slog.String("socket", fc.socketPath),
				slog.String("err", err.Error()),
				slog.Duration("retry_in", backoff),
			)
			select {
			case <-ctx.Done():
				return
			case <-time.After(backoff):
			}
			// Double backoff, cap at feedbackRetryMax.
			backoff *= 2
			if backoff > feedbackRetryMax {
				backoff = feedbackRetryMax
			}
			continue
		}
		// Successful connection — reset backoff.
		backoff = feedbackRetryBase
		fc.log.Info("sidecar connected", slog.String("socket", fc.socketPath))
		if err := fc.pump(ctx, conn); err != nil {
			if ctx.Err() != nil {
				_ = conn.Close()
				return
			}
			fc.log.Warn("sidecar connection lost — reconnecting",
				slog.String("err", err.Error()))
		}
		_ = conn.Close()
	}
}

// dial opens a Unix domain socket connection to the sidecar.
func (fc *FeedbackClient) dial(ctx context.Context) (net.Conn, error) {
	dialer := net.Dialer{Timeout: feedbackDialTimeout}
	return dialer.DialContext(ctx, "unix", fc.socketPath)
}

// pump reads messages from the queue and writes them to conn.
// Returns a non-nil error when the connection breaks or ctx is cancelled.
func (fc *FeedbackClient) pump(ctx context.Context, conn net.Conn) error {
	reader := bufio.NewReader(conn)
	for {
		select {
		case <-ctx.Done():
			return nil
		case msg := <-fc.queue:
			if err := fc.sendOne(conn, reader, msg); err != nil {
				// Re-enqueue on write error so the message is not lost
				// (best-effort; if the queue is now full it is dropped).
				select {
				case fc.queue <- msg:
				default:
					fc.dropped.Add(1)
				}
				return fmt.Errorf("send: %w", err)
			}
			fc.delivered.Add(1)
		}
	}
}

// sendOne serialises msg as JSON, writes it to conn, and reads the ACK.
func (fc *FeedbackClient) sendOne(
	conn net.Conn,
	reader *bufio.Reader,
	msg *FeedbackMessage,
) error {
	payload, err := json.Marshal(msg)
	if err != nil {
		return fmt.Errorf("marshal: %w", err)
	}
	payload = append(payload, '\n')

	if err := conn.SetWriteDeadline(time.Now().Add(feedbackWriteTimeout)); err != nil {
		return fmt.Errorf("set write deadline: %w", err)
	}
	if _, err := conn.Write(payload); err != nil {
		return fmt.Errorf("write: %w", err)
	}

	// Read ACK (optional — sidecar always sends one per message).
	if err := conn.SetReadDeadline(time.Now().Add(feedbackWriteTimeout)); err != nil {
		return fmt.Errorf("set read deadline: %w", err)
	}
	line, err := reader.ReadBytes('\n')
	if err != nil {
		return fmt.Errorf("read ack: %w", err)
	}

	var ack feedbackAck
	if jsonErr := json.Unmarshal(line, &ack); jsonErr != nil {
		// Non-fatal: ACK parse failure does not break the connection.
		fc.log.Debug("sidecar ACK parse error",
			slog.String("raw", string(line)),
			slog.String("err", jsonErr.Error()))
		return nil
	}
	if !ack.OK {
		fc.log.Warn("sidecar rejected message",
			slog.String("job_id", msg.JobID),
			slog.String("error", ack.Error))
	} else if ack.Checkpoint != "" {
		fc.log.Info("sidecar checkpoint exported",
			slog.String("path", ack.Checkpoint),
			slog.Int64("step", ack.Step))
	}
	return nil
}
