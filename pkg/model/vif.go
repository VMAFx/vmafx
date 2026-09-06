// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// pkg/model/vif.go — does a VMAF model request the VIF feature natively?
//
// Every Go libvmaf argv builder in the fork (pkg/corpus, pkg/fast,
// pkg/scorecli, pkg/tune/executor) needs the canonical-6 feature columns
// (adm2, vif_scale0..3, motion2) populated. The v0.6 model generation
// requests vif_scale0..3 itself; the v1 generation — including the default
// DefaultVersion (ADR-1168 / ADR-1169) — does not, so callers must append
// `--feature vif` to the vmaf argv. This helper is the single place that
// decides which case applies, so the four builders cannot drift.

package model

import (
	"encoding/json"
	"os"
	"strings"
)

// RequestsVIF reports whether a model version or path already requests VIF
// features natively, in which case appending `--feature vif` is redundant.
//
// The argument is whatever the caller would hand to `--model`: a bare
// version ("vmaf_v0.6.1"), a "version=..." string, or a "path=/abs/model.json"
// string. Models in the v0.6 generation (vmaf_v0.6.1, vmaf_b_v0.6.3,
// vmaf_4k_v0.6.1, vmaf_v0.6.1neg, vmaf_float_v0.6.1) include vif_scale0..3.
// For a path that is a readable JSON model file the feature list is inspected
// directly. Anything else — including the v1 default — reports false so the
// caller requests VIF explicitly; libvmaf tolerates a duplicate request, so
// a false negative costs nothing while a false positive would leave the
// vif_scale0..3 columns NaN.
func RequestsVIF(model string) bool {
	if model == "" {
		return false
	}
	val := model
	if strings.HasPrefix(val, "version=") || strings.HasPrefix(val, "path=") {
		val = val[strings.IndexByte(val, '=')+1:]
	}
	if strings.Contains(val, "v0.6") {
		return true
	}
	data, err := os.ReadFile(val) // #nosec G304 -- caller-supplied model path.
	if err != nil {
		return false
	}
	var m struct {
		ModelDict struct {
			FeatureNames []string `json:"feature_names"`
		} `json:"model_dict"`
	}
	if err := json.Unmarshal(data, &m); err != nil {
		return false
	}
	for _, fn := range m.ModelDict.FeatureNames {
		if strings.Contains(strings.ToLower(fn), "vif") {
			return true
		}
	}
	return false
}
