// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// pkg/corpus/schema_test.go — row-schema contract test.
//
// The expected list was dumped from vmaftune.CORPUS_ROW_KEYS. It is compared
// in order, not as a set: the tuple's order is the documented contract
// downstream positional consumers index into.

package corpus

import (
	"reflect"
	"testing"
)

func TestRowKeysMatchPython(t *testing.T) {
	t.Parallel()

	want := []string{
		"schema_version",
		"run_id",
		"timestamp",
		"src",
		"src_sha256",
		"width",
		"height",
		"pix_fmt",
		"framerate",
		"duration_s",
		"encoder",
		"encoder_version",
		"preset",
		"crf",
		"extra_params",
		"encode_path",
		"encode_size_bytes",
		"bitrate_kbps",
		"encode_time_ms",
		"vmaf_score",
		"vmaf_model",
		"score_time_ms",
		"ffmpeg_version",
		"vmaf_binary_version",
		"exit_status",
		"clip_mode",
		"hdr_transfer",
		"hdr_primaries",
		"hdr_forced",
		"shot_count",
		"shot_avg_duration_sec",
		"shot_duration_std_sec",
		"adm2_mean",
		"vif_scale0_mean",
		"vif_scale1_mean",
		"vif_scale2_mean",
		"vif_scale3_mean",
		"motion2_mean",
		"adm2_std",
		"vif_scale0_std",
		"vif_scale1_std",
		"vif_scale2_std",
		"vif_scale3_std",
		"motion2_std",
		"enc_internal_qp_mean",
		"enc_internal_qp_std",
		"enc_internal_bits_mean",
		"enc_internal_bits_std",
		"enc_internal_mv_mean",
		"enc_internal_mv_std",
		"enc_internal_itex_mean",
		"enc_internal_ptex_mean",
		"enc_internal_intra_ratio",
		"enc_internal_skip_ratio",
	}
	if !reflect.DeepEqual(RowKeys, want) {
		t.Errorf("RowKeys =\n  %v\nwant\n  %v", RowKeys, want)
	}
}

func TestCanonical6Ordering(t *testing.T) {
	t.Parallel()

	// The order pins the column index every downstream positional consumer
	// relies on; the derived _mean / _std tuples must follow it.
	wantFeatures := []string{
		"adm2", "vif_scale0", "vif_scale1", "vif_scale2", "vif_scale3", "motion2",
	}
	if !reflect.DeepEqual(Canonical6Features, wantFeatures) {
		t.Errorf("Canonical6Features = %v, want %v", Canonical6Features, wantFeatures)
	}
	for i, f := range wantFeatures {
		if Canonical6MeanKeys[i] != f+"_mean" {
			t.Errorf("Canonical6MeanKeys[%d] = %q, want %q", i, Canonical6MeanKeys[i], f+"_mean")
		}
		if Canonical6StdKeys[i] != f+"_std" {
			t.Errorf("Canonical6StdKeys[%d] = %q, want %q", i, Canonical6StdKeys[i], f+"_std")
		}
	}
}

func TestSchemaVersionIsV3(t *testing.T) {
	t.Parallel()

	// Bumping this is a coordinated change with the Phase B/C consumers.
	if SchemaVersion != 3 {
		t.Errorf("SchemaVersion = %d, want 3", SchemaVersion)
	}
}
