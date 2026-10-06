// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

//go:build cgo

package libvmaf

/*
#include <string.h>

#include <libvmaf/libvmaf.h>
#include <vmafx/libvmaf_bridge.h>
#include <vmafx/provenance.h>

// vmafx_go_provenance fills the provenance record of the VMAFx context a
// libvmaf handle is bound to; -1 when the handle has none.
static int vmafx_go_provenance(VmafContext *vmaf, VmafxProvenance *out)
{
    VmafxContext *context = vmafx_context_from_libvmaf(vmaf);
    if (!context)
        return -1;
    memset(out, 0, sizeof(*out));
    out->struct_size = (uint32_t)sizeof(*out);
    return vmafx_context_provenance(context, out, NULL);
}
*/
import "C"

import (
	"encoding/json"
	"fmt"
)

// backendNames are the lower-case VmafxBackend value names (the enum of the
// API definition, values equal to enum VmafBackend), as the CLI report and the
// proto Provenance message spell them.
var backendNames = map[uint32]string{0: "cpu", 1: "cuda", 2: "sycl", 3: "metal", 4: "hip"}

// contextProvenance is the provenance record of a libvmaf context as JSON,
// one key per field of VmafxProvenance (the form the CLI report carries).
func contextProvenance(vmafCtx *C.VmafContext) (json.RawMessage, error) {
	var record C.VmafxProvenance
	if rc := C.vmafx_go_provenance(vmafCtx, &record); rc != 0 {
		return nil, fmt.Errorf("libvmaf: read the provenance record: status %d", int(rc))
	}
	backend, ok := backendNames[uint32(record.active_backend)]
	if !ok {
		backend = fmt.Sprintf("backend-%d", uint32(record.active_backend))
	}
	return json.Marshal(map[string]any{
		"abi_major":      uint32(record.abi_major),
		"abi_minor":      uint32(record.abi_minor),
		"abi_patch":      uint32(record.abi_patch),
		"active_backend": backend,
		"n_extractors":   uint32(record.n_extractors),
		"version":        C.GoString(record.version),
	})
}

// Provenance is the provenance record of the stream's context and the SHA-256
// of its model file (#2155); call it after Finish.
func (s *StreamScorer) Provenance() (json.RawMessage, string, error) {
	if s.teardownStarted || s.vmafCtx == nil {
		return nil, "", fmt.Errorf("StreamScorer.Provenance after teardown: %w", ErrInvalidArgument)
	}
	record, err := contextProvenance(s.vmafCtx)
	if err != nil {
		return nil, "", err
	}
	digest, err := fileSHA256(s.cfg.ModelPath)
	if err != nil {
		return nil, "", err
	}
	return record, digest, nil
}
