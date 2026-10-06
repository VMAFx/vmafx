// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

//go:build cgo

package libvmaf

/*
#include <stddef.h>

#include <libvmaf/libvmaf.h>
#include <vmafx/libvmaf_bridge.h>
#include <vmafx/provenance.h>

// vmafx_go_provenance_json points *json at the provenance record of the
// VMAFx context a libvmaf handle is bound to, in the JSON form the vmaf CLI's
// report carries (RC4 WP5); -1 when the handle has none. The text lives
// until the next call on the context.
static int vmafx_go_provenance_json(VmafContext *vmaf, const char **json)
{
    VmafxContext *context = vmafx_context_from_libvmaf(vmaf);
    if (!context)
        return -1;
    return vmafx_context_provenance_json(context, 0u, json, NULL);
}
*/
import "C"

import (
	"encoding/json"
	"fmt"
)

// contextProvenance is the provenance record of a libvmaf context as JSON,
// the same text the CLI report embeds (vmafx_context_provenance_json(), RC4
// WP5): one key per field of VmafxProvenance and its models, features and
// annotations, which the server reads into the proto Provenance message.
func contextProvenance(vmafCtx *C.VmafContext) (json.RawMessage, error) {
	var text *C.char
	if rc := C.vmafx_go_provenance_json(vmafCtx, &text); rc != 0 || text == nil {
		return nil, fmt.Errorf("libvmaf: read the provenance record: status %d", int(rc))
	}
	return json.RawMessage(C.GoString(text)), nil
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
