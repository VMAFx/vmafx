// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// test/e2e/controller-ha/driver/fixtures.go — the inputs the nodes stream
// from the driver, held back until the kill.

package main

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"net"
	"net/http"
	"os"
	"strconv"
	"sync"
	"time"
)

// The clip geometry: the smallest the default model accepts (cambi needs
// one axis of 216, speed_chroma a 4:2:0 luma of 160; see
// test/e2e/fixtures/gen-tiny-yuv.sh).
const (
	fixtureWidth  = 216
	fixtureHeight = 160
	frameBytes    = fixtureWidth * fixtureHeight * 3 / 2
	// gateTimeout bounds how long a held stream waits for the kill.
	gateTimeout = 5 * time.Minute
)

// fixtures serves a reference and a distorted Y4M clip under /fixtures/.
// Until release is called, a GET sends the header and the first frame and
// then holds the rest back, so every first attempt is mid-job when the
// scenario kills its node. release lets every stream run to the end except
// those of the killed node's address: a pod deleted with grace period 0 is
// gone from the API before the kubelet has stopped its container, so a
// released stream could still complete there. HEAD answers at once.
type fixtures struct {
	files map[string][]byte
	head  int // bytes sent before the gate: header and first frame
	gate  chan struct{}
	once  sync.Once
	dead  string // address whose streams stay held, set before gate closes
}

func newFixtures(frames int) (*fixtures, error) {
	if frames < 2 {
		return nil, fmt.Errorf("fixtures need at least 2 frames, got %d", frames)
	}
	ref, head := clip(frames, false)
	dis, _ := clip(frames, true)
	return &fixtures{
		files: map[string][]byte{"/fixtures/ref.y4m": ref, "/fixtures/dis.y4m": dis},
		head:  head,
		gate:  make(chan struct{}),
	}, nil
}

// release lets every held stream, and every later one, run to the end,
// except those from the address dead (empty: none).
func (f *fixtures) release(dead string) {
	f.once.Do(func() {
		f.dead = dead
		close(f.gate)
	})
}

// clip builds a textured 4:2:0 clip; the distorted one adds a fixed
// pattern of small errors. It returns the clip and the length of its
// header plus first frame.
func clip(frames int, distorted bool) ([]byte, int) {
	header := fmt.Sprintf("YUV4MPEG2 W%d H%d F24:1 Ip A0:0 C420jpeg\n", fixtureWidth, fixtureHeight)
	var b bytes.Buffer
	b.Grow(len(header) + frames*(len("FRAME\n")+frameBytes))
	b.WriteString(header)
	head := 0
	frame := make([]byte, frameBytes)
	for f := range frames {
		fillFrame(frame, f, distorted)
		b.WriteString("FRAME\n")
		b.Write(frame)
		if f == 0 {
			head = b.Len()
		}
	}
	return b.Bytes(), head
}

// fillFrame writes frame f: luma and chroma ramps that move with f, and for
// the distorted clip an error of -3..3 from a xorshift sequence.
func fillFrame(frame []byte, f int, distorted bool) {
	state := uint32(2463534242) + uint32(f) // #nosec G115 -- f < frames, a small flag value
	for i := range frame {
		x, y := i%fixtureWidth, i/fixtureWidth
		v := (x*3 + y*5 + f*7 + (x*y)/17) & 0xff
		if distorted {
			state ^= state << 13
			state ^= state >> 17
			state ^= state << 5
			v = min(max(v+int(state%7)-3, 0), 255)
		}
		frame[i] = byte(v) // #nosec G115 -- v is clamped to 0..255
	}
}

func (f *fixtures) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	data, ok := f.files[r.URL.Path]
	if !ok {
		http.NotFound(w, r)
		return
	}
	if r.Method != http.MethodGet && r.Method != http.MethodHead {
		w.WriteHeader(http.StatusMethodNotAllowed)
		return
	}
	w.Header().Set("Content-Type", "video/x-yuv4mpeg")
	w.Header().Set("Content-Length", strconv.Itoa(len(data)))
	if r.Method == http.MethodHead {
		return
	}
	if err := send(w, data[:f.head]); err != nil {
		fmt.Fprintf(os.Stderr, "stream %s: %v\n", r.URL.Path, err)
		return
	}
	if rc := http.NewResponseController(w); rc.Flush() != nil || !f.hold(r.Context(), remoteHost(r)) {
		return
	}
	if err := send(w, data[f.head:]); err != nil {
		fmt.Fprintf(os.Stderr, "stream %s: %v\n", r.URL.Path, err)
	}
}

// hold waits for release (at most gateTimeout); false when the request
// ended first or comes from the killed node, whose stream then waits until
// its connection closes.
func (f *fixtures) hold(ctx context.Context, from string) bool {
	t := time.NewTimer(gateTimeout)
	defer t.Stop()
	select {
	case <-f.gate:
	case <-t.C:
		return true
	case <-ctx.Done():
		return false
	}
	if f.dead == "" || from != f.dead {
		return true
	}
	select {
	case <-t.C:
	case <-ctx.Done():
	}
	return false
}

// remoteHost is the address a request came from, without its port.
func remoteHost(r *http.Request) string {
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err != nil {
		return r.RemoteAddr
	}
	return host
}

// send writes p; a failed write means the node went away (killed).
func send(w http.ResponseWriter, p []byte) error {
	if _, err := w.Write(p); err != nil {
		return fmt.Errorf("write: %w", err)
	}
	return nil
}

// serve starts the fixture server on addr and returns its stop function,
// which also releases every held stream.
func (f *fixtures) serve(ctx context.Context, addr string) (func(), error) {
	ln, err := (&net.ListenConfig{}).Listen(ctx, "tcp", addr)
	if err != nil {
		return nil, fmt.Errorf("fixture server: %w", err)
	}
	srv := &http.Server{Handler: f, ReadHeaderTimeout: callTimeout}
	go func() {
		if serr := srv.Serve(ln); serr != nil && !errors.Is(serr, http.ErrServerClosed) {
			fmt.Fprintf(os.Stderr, "fixture server: %v\n", serr)
		}
	}()
	return func() {
		f.release("")
		sctx, cancel := context.WithTimeout(context.WithoutCancel(ctx), callTimeout)
		defer cancel()
		if serr := srv.Shutdown(sctx); serr != nil {
			fmt.Fprintf(os.Stderr, "fixture server shutdown: %v\n", serr)
		}
	}, nil
}

// waitServed waits until the fixtures answer through url (the Service the
// nodes use), so no node reads them before the Service has an endpoint.
func waitServed(ctx context.Context, url string) error {
	client := &http.Client{Timeout: callTimeout}
	var last error
	for range maxPolls {
		req, err := http.NewRequestWithContext(ctx, http.MethodHead, url+"/ref.y4m", nil)
		if err != nil {
			return fmt.Errorf("fixture request: %w", err)
		}
		resp, err := client.Do(req)
		if err == nil {
			cerr := resp.Body.Close()
			if resp.StatusCode == http.StatusOK && cerr == nil {
				return nil
			}
			last = fmt.Errorf("HEAD %s: %s (%v)", url, resp.Status, cerr)
		} else {
			last = err
		}
		if werr := wait(ctx, pollEvery); werr != nil {
			return fmt.Errorf("fixtures not served at %s: %w (last: %v)", url, werr, last)
		}
	}
	return fmt.Errorf("fixtures not served at %s after %d polls: %w", url, maxPolls, last)
}
