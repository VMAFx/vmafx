// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// pkg/corpus/schema.go — corpus JSONL row schema (Go port of vmaftune.__init__).
//
// The JSONL schema emitted by the corpus sweep is the API contract Phase B
// (target-VMAF bisect) and Phase C (per-title CRF predictor) consume; bumping
// SchemaVersion is a coordinated change.

package corpus

// SchemaVersion is bumped on any backward-incompatible row-schema change.
//
//   - v2 added clip_mode (additive, default "full") for the sample-clip mode
//     introduced under ADR-0297.
//   - v3 adds the HDR provenance triple (hdr_transfer / hdr_primaries /
//     hdr_forced), the canonical-6 per-frame libvmaf features as mean and std
//     aggregates (ADR-0366), the TransNet-V2 shot-metadata trio (ADR-0223), and
//     ten enc_internal_* scalar aggregates from x264/x265 pass-1 stats
//     (ADR-0332).
const SchemaVersion = 3

// Canonical6Features are the canonical-6 libvmaf feature names. Order is
// load-bearing — downstream code indexes into the derived _mean / _std column
// tuples positionally.
var Canonical6Features = []string{
	"adm2",
	"vif_scale0",
	"vif_scale1",
	"vif_scale2",
	"vif_scale3",
	"motion2",
}

// Canonical6MeanKeys / Canonical6StdKeys are the v3 row keys derived from
// Canonical6Features.
var (
	Canonical6MeanKeys = derivedKeys("_mean")
	Canonical6StdKeys  = derivedKeys("_std")
)

func derivedKeys(suffix string) []string {
	out := make([]string, len(Canonical6Features))
	for i, f := range Canonical6Features {
		out[i] = f + suffix
	}
	return out
}

// EncoderStatsColumns are the ten enc_internal_* scalar columns (ADR-0332).
// The order is stable so downstream consumers can rely on positional
// iteration.
var EncoderStatsColumns = []string{
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

// RowKeys is the canonical row-key list — exposed so tests, downstream
// loaders, and the Phase B bisect can verify the contract programmatically.
var RowKeys = buildRowKeys()

func buildRowKeys() []string {
	keys := []string{
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
		// TransNet-V2 shot-metadata trio (ADR-0223 / research-0086). All
		// additive; shot_count == 0 flags "shot detection unavailable for
		// this source" so downstream consumers can opt out without a
		// schema-version bump.
		"shot_count",
		"shot_avg_duration_sec",
		"shot_duration_std_sec",
	}
	keys = append(keys, Canonical6MeanKeys...)
	keys = append(keys, Canonical6StdKeys...)
	keys = append(keys, EncoderStatsColumns...)
	return keys
}
