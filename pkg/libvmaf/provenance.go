// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

//go:build cgo

package libvmaf

import (
	"encoding/json"
	"fmt"
)

// Provenance is the provenance record of the stream's context, the JSON the
// CLI report embeds (vmafx_context_provenance_json(), RC4 WP5: one key per
// field of VmafxProvenance and its models, features and annotations, which the
// server reads into the proto Provenance message), and the SHA-256 of its
// model file (#2155); call it after Finish.
func (s *StreamScorer) Provenance() (json.RawMessage, string, error) {
	if s.teardownStarted || s.vmafCtx == nil {
		return nil, "", fmt.Errorf("StreamScorer.Provenance after teardown: %w", ErrInvalidArgument)
	}
	record, err := s.session().provenance()
	if err != nil {
		return nil, "", fmt.Errorf("libvmaf: read the provenance record: %w", err)
	}
	digest, err := fileSHA256(s.cfg.ModelPath)
	if err != nil {
		return nil, "", err
	}
	return json.RawMessage(record), digest, nil
}
