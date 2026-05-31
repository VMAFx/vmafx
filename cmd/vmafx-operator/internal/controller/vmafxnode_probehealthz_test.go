// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-operator/internal/controller/vmafxnode_probehealthz_test.go —
// Standalone (non-envtest) regression tests for probeHealthz.
//
// The Ginkgo suite under suite_test.go drives the reconciler against an
// envtest API server, but the body-drain regression (operator audit, this
// PR) needs to assert that the response body is fully consumed before
// Close — a property of probeHealthz alone, not of the controller-runtime
// integration. Splitting these into a plain `go test` file means they
// run without `KUBEBUILDER_ASSETS`, so CI lanes that lack envtest still
// gate the connection-pool fix.
//
// Background: the prior implementation called `defer resp.Body.Close()`
// without draining. Go's net/http documents that an unread body breaks
// HTTP/1.1 keep-alive — for N VmafxNodes polled every 30s, the
// controller leaks one TCP connection per probe. The fix drains via
// io.Copy(io.Discard, ...) before Close; this test asserts the drain
// actually happens.
//
// ADR-0714: vmafx-operator kubebuilder skeleton + CRDs.

package controller

import (
	"context"
	"io"
	"net/http"
	"strings"
	"testing"
	"time"
)

// countingBody wraps an io.Reader and records how many bytes were Read
// and whether Close was called. Close-before-EOF leaves bytesRead < total,
// which is the regression we are gating.
type countingBody struct {
	r         io.Reader
	bytesRead int
	closed    bool
}

func (c *countingBody) Read(p []byte) (int, error) {
	n, err := c.r.Read(p)
	c.bytesRead += n
	return n, err
}

func (c *countingBody) Close() error {
	c.closed = true
	return nil
}

// stubBodyRoundTripper returns a fixed response with a counting body so
// the test can assert the reconciler drained it.
type stubBodyRoundTripper struct {
	body       *countingBody
	statusCode int
}

func (s *stubBodyRoundTripper) RoundTrip(_ *http.Request) (*http.Response, error) {
	return &http.Response{
		StatusCode: s.statusCode,
		Body:       s.body,
		Header:     make(http.Header),
	}, nil
}

// TestProbeHealthzDrainsBody asserts that probeHealthz reads the entire
// response body before closing it, so the connection can be returned to
// the keep-alive pool.
func TestProbeHealthzDrainsBody(t *testing.T) {
	t.Parallel()

	const payload = "ok\n"
	body := &countingBody{r: strings.NewReader(payload)}

	r := &VmafxNodeReconciler{
		HTTPClient: &http.Client{
			Transport: &stubBodyRoundTripper{body: body, statusCode: http.StatusOK},
			Timeout:   2 * time.Second,
		},
	}

	healthy := r.probeHealthz(context.Background(), "default")
	if !healthy {
		t.Fatalf("probeHealthz: expected healthy=true on 200 OK")
	}
	if !body.closed {
		t.Errorf("probeHealthz: expected response body to be closed")
	}
	if body.bytesRead != len(payload) {
		t.Errorf("probeHealthz: expected entire body drained (%d bytes), got %d — "+
			"undrained bodies break HTTP keep-alive and leak connections",
			len(payload), body.bytesRead)
	}
}

// TestProbeHealthzNon200StillDrains asserts the drain happens on non-200
// responses too — keep-alive reuse depends on a clean body regardless of
// status code.
func TestProbeHealthzNon200StillDrains(t *testing.T) {
	t.Parallel()

	const payload = "internal server error\n"
	body := &countingBody{r: strings.NewReader(payload)}

	r := &VmafxNodeReconciler{
		HTTPClient: &http.Client{
			Transport: &stubBodyRoundTripper{
				body: body, statusCode: http.StatusInternalServerError,
			},
			Timeout: 2 * time.Second,
		},
	}

	healthy := r.probeHealthz(context.Background(), "default")
	if healthy {
		t.Errorf("probeHealthz: expected healthy=false on 500")
	}
	if !body.closed {
		t.Errorf("probeHealthz: expected response body to be closed on 500")
	}
	if body.bytesRead != len(payload) {
		t.Errorf("probeHealthz: expected entire body drained on 500 (%d bytes), got %d",
			len(payload), body.bytesRead)
	}
}

// errRoundTripper always fails the request — exercises the early-return
// path where resp is nil and no defer is registered.
type errRoundTripper struct{}

func (errRoundTripper) RoundTrip(_ *http.Request) (*http.Response, error) {
	return nil, io.ErrUnexpectedEOF
}

// TestProbeHealthzTransportErrorReturnsFalse asserts the reconciler
// returns false without panicking on a transport-level error.
func TestProbeHealthzTransportErrorReturnsFalse(t *testing.T) {
	t.Parallel()

	r := &VmafxNodeReconciler{
		HTTPClient: &http.Client{
			Transport: errRoundTripper{},
			Timeout:   2 * time.Second,
		},
	}

	if r.probeHealthz(context.Background(), "default") {
		t.Errorf("probeHealthz: expected healthy=false on transport error")
	}
}
