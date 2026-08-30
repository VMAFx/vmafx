// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// pkg/libvmaf/dnn.go — cgo binding for libvmaf's standalone ONNX
// Runtime session API (core/include/libvmaf/dnn.h).
//
// Why this exists: the MCP `eval_model_on_split` / `compare_models`
// tools need to run an ONNX regressor over a batch of feature rows.
// The Go server previously shelled out to python3 with an inline script
// that imported onnxruntime. libvmaf already links ONNX Runtime and
// already exposes exactly the primitive required — a named-tensor
// float32 in / float32 out session (vmaf_dnn_session_run) — so binding
// it here removes the Python dependency without adding a third-party
// ONNX package to the Go module. It also inherits libvmaf's model
// hardening for free: the size cap and the operator allowlist that
// vmaf_dnn_session_open applies (ADR-0211).
//
// Availability contract: libvmaf always exports these symbols. When it
// was built without ONNX Runtime (meson -Denable_dnn=disabled, or the
// `auto` default on a host with no ORT pkg-config), every entry point
// compiles to an -ENOSYS stub — see core/src/dnn/dnn_api.c. Callers get
// ErrDNNUnavailable and can degrade the same way the Python server does
// when its optional `eval` extra is not installed.
//
// Threading: a VmafDnnSession is explicitly documented as not
// thread-safe. DNNSession therefore serialises Run and Close with a
// mutex so a session shared across MCP request goroutines cannot race
// inside ORT.

//go:build cgo

package libvmaf

/*
#include <libvmaf/dnn.h>
#include <errno.h>
#include <stdlib.h>
*/
import "C"

import (
	"context"
	"errors"
	"fmt"
	"runtime"
	"sync"
	"unsafe"
)

// Sentinel errors mapped from the negative errno values dnn.h returns.
var (
	// ErrDNNUnavailable means libvmaf was built without ONNX Runtime
	// (-ENOSYS). Analogous to the Python server's missing `eval` extra.
	ErrDNNUnavailable = errors.New(
		"libvmaf was built without DNN support (ONNX Runtime not found at build time); " +
			"rebuild with -Denable_dnn=enabled against an installed onnxruntime")
	// ErrDNNModelTooLarge is -E2BIG from the model size cap.
	ErrDNNModelTooLarge = errors.New("onnx model exceeds libvmaf's size cap")
	// ErrDNNSessionClosed is returned by Run after Close.
	ErrDNNSessionClosed = errors.New("dnn session is closed")
)

// DNNAvailable reports whether this libvmaf build carries a working ONNX
// Runtime. False means every session call will return ErrDNNUnavailable.
func DNNAvailable() bool {
	return C.vmaf_dnn_available() != 0
}

// DNNSession is an open standalone ONNX Runtime session.
type DNNSession struct {
	mu   sync.Mutex
	sess *C.VmafDnnSession
	path string
}

// Negative errno values dnn.h documents, mirrored as plain Go ints so
// the mapping below stays unit-testable (cgo is not available inside
// _test.go files).
const (
	rcENOSYS = -int(C.ENOSYS)
	rcE2BIG  = -int(C.E2BIG)
	rcENOENT = -int(C.ENOENT)
	rcEINVAL = -int(C.EINVAL)
	rcEIO    = -int(C.EIO)
	rcENOSPC = -int(C.ENOSPC)
)

// dnnErr converts a negative errno returned by dnn.h into a Go error.
// op names the C entry point for context.
func dnnErr(op string, rc int) error {
	switch rc {
	case 0:
		return nil
	case rcENOSYS:
		return ErrDNNUnavailable
	case rcE2BIG:
		return ErrDNNModelTooLarge
	case rcENOENT:
		return fmt.Errorf("%s: model file not found", op)
	case rcEINVAL:
		return fmt.Errorf("%s: invalid argument (bad path, or graph arity does not match the "+
			"single input / single output this call binds)", op)
	case rcEIO:
		return fmt.Errorf("%s: ONNX Runtime failed to load or execute the model", op)
	case rcENOSPC:
		return fmt.Errorf("%s: output buffer too small", op)
	default:
		return fmt.Errorf("%s: libvmaf returned %d", op, rc)
	}
}

// OpenDNNSession opens an ONNX Runtime session against the model at
// path. The caller must Close it.
//
// Device selection is left at VMAF_DNN_DEVICE_AUTO by passing a NULL
// config, which lets ORT pick its provider chain exactly as the Python
// server's CPUExecutionProvider default would for a CPU-only build.
func OpenDNNSession(path string) (*DNNSession, error) {
	cPath := C.CString(path)
	defer C.free(unsafe.Pointer(cPath))

	var sess *C.VmafDnnSession
	rc := int(C.vmaf_dnn_session_open(&sess, cPath, nil))
	if err := dnnErr("vmaf_dnn_session_open", rc); err != nil {
		return nil, err
	}
	if sess == nil {
		return nil, errors.New("vmaf_dnn_session_open returned a nil session")
	}
	return &DNNSession{sess: sess, path: path}, nil
}

// newSessionWithDummyHandleForTest builds a DNNSession around a
// malloc'd dummy handle so the cgo argument-marshalling path in run() is
// reachable from tests.
//
// This is ONLY safe on a libvmaf built without ONNX Runtime, where every
// dnn.h entry point returns -ENOSYS after `(void)sess` without ever
// dereferencing the handle (core/src/dnn/dnn_api.c). Callers must gate on
// !DNNAvailable(); handing a real ORT build a bogus session pointer would
// crash. It exists because the argument marshalling is exactly where the
// cgo pointer rules bite (a descriptor struct holding Go pointers must be
// pinned), and that bug is otherwise unreachable without an ORT build.
func newSessionWithDummyHandleForTest() *DNNSession {
	h := (*C.VmafDnnSession)(C.calloc(1, C.size_t(unsafe.Sizeof(C.VmafDnnSession{}))))
	return &DNNSession{sess: h, path: "<dummy>"}
}

// freeDummyHandleForTest releases what newSessionWithDummyHandleForTest
// allocated, bypassing vmaf_dnn_session_close (which must not be handed a
// handle it did not create).
func (s *DNNSession) freeDummyHandleForTest() {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.sess != nil {
		C.free(unsafe.Pointer(s.sess))
		s.sess = nil
	}
}

// AttachedEP reports the ONNX Runtime execution provider that actually
// bound to the session ("CPU", "CUDA", "CoreML", ...). Empty when the
// session is closed or DNN support is absent.
func (s *DNNSession) AttachedEP() string {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.sess == nil {
		return ""
	}
	if ep := C.vmaf_dnn_session_attached_ep(s.sess); ep != nil {
		return C.GoString(ep)
	}
	return ""
}

// Close releases the session. Safe to call more than once.
func (s *DNNSession) Close() {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.sess != nil {
		C.vmaf_dnn_session_close(s.sess)
		s.sess = nil
	}
}

// Predict binds x as a [rows, cols] float32 tensor on the graph input
// named inputName and returns the single output tensor flattened.
//
// It implements modeleval.Predictor. One output value per row is the
// expected shape for a regressor; a graph that produces a different
// element count still returns successfully here so the caller can raise
// the same shape-mismatch diagnostic the Python original does.
func (s *DNNSession) Predict(_ context.Context, inputName string, x []float32, rows, cols int) ([]float32, error) {
	return s.run(inputName, x, rows, cols)
}

func (s *DNNSession) run(inputName string, x []float32, rows, cols int) ([]float32, error) {
	if rows <= 0 || cols <= 0 {
		return nil, fmt.Errorf("invalid tensor shape [%d, %d]", rows, cols)
	}
	if len(x) != rows*cols {
		return nil, fmt.Errorf("buffer holds %d elements, shape [%d, %d] needs %d",
			len(x), rows, cols, rows*cols)
	}

	s.mu.Lock()
	defer s.mu.Unlock()
	if s.sess == nil {
		return nil, ErrDNNSessionClosed
	}

	cName := C.CString(inputName)
	defer C.free(unsafe.Pointer(cName))

	shape := []C.int64_t{C.int64_t(rows), C.int64_t(cols)}
	out := make([]float32, rows)

	// The descriptor structs are Go memory that holds Go pointers (into
	// x, shape and out), and we pass their addresses to C. The cgo
	// pointer rules forbid passing a Go pointer to Go memory containing
	// pointers to *unpinned* Go memory — without pinning this panics with
	// "cgo argument has Go pointer to unpinned Go pointer" on every call.
	// Pinning the three backing arrays makes it legal and keeps them from
	// moving while ORT reads and writes them. C.CString memory is malloc'd
	// and needs no pin.
	var pinner runtime.Pinner
	defer pinner.Unpin()
	pinner.Pin(&x[0])
	pinner.Pin(&shape[0])
	pinner.Pin(&out[0])

	in := C.VmafDnnInput{
		name:  cName,
		data:  (*C.float)(unsafe.Pointer(&x[0])),
		shape: (*C.int64_t)(unsafe.Pointer(&shape[0])),
		rank:  2,
	}
	outDesc := C.VmafDnnOutput{
		name:     nil,
		data:     (*C.float)(unsafe.Pointer(&out[0])),
		capacity: C.size_t(len(out)),
	}

	rc := int(C.vmaf_dnn_session_run(s.sess, &in, 1, &outDesc, 1))

	// On -ENOSPC the call still reports how many elements the graph
	// would have produced. Resize and retry once so a model whose
	// output arity differs from one-per-row reaches the caller as a
	// shape mismatch rather than an opaque buffer error.
	if rc == rcENOSPC {
		need := int(outDesc.written)
		if need <= 0 || need == len(out) {
			return nil, dnnErr("vmaf_dnn_session_run", rc)
		}
		out = make([]float32, need)
		// Fresh allocation: pin it too before it is handed to C.
		pinner.Pin(&out[0])
		outDesc.data = (*C.float)(unsafe.Pointer(&out[0]))
		outDesc.capacity = C.size_t(need)
		outDesc.written = 0
		rc = int(C.vmaf_dnn_session_run(s.sess, &in, 1, &outDesc, 1))
	}
	if err := dnnErr("vmaf_dnn_session_run", rc); err != nil {
		return nil, err
	}

	written := int(outDesc.written)
	if written >= 0 && written < len(out) {
		out = out[:written]
	}
	return out, nil
}
