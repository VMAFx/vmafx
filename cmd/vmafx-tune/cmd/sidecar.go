// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package cmd

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"os"
	"sort"
	"strings"

	"github.com/golusoris/golusoris/clikit"
	"github.com/spf13/cobra"

	"github.com/VMAFx/vmafx/pkg/codecadapter"
	"github.com/VMAFx/vmafx/pkg/predictor"
	"github.com/VMAFx/vmafx/pkg/pyjson"
	"github.com/VMAFx/vmafx/pkg/sidecar"
)

// sidecarFlags holds the flags shared by every "sidecar" child command, plus
// the per-child extras. The flag names mirror the Python
// `vmaf-tune sidecar <cmd>` argument parser exactly.
type sidecarFlags struct {
	// Shared (_add_sidecar_common_args).
	codec            string
	cacheDir         string
	predictorVersion string
	model            string

	// Per-child.
	featuresJSON string
	crf          int
	observedVMAF float64
	capturesJSON string
	noPersist    bool
	asJSON       bool
}

// newSidecarCmd builds the "sidecar" parent command and its children.
//
// The parent mirrors `vmaf-tune sidecar`: it trains and inspects the local
// on-host predictor sidecar (ADR-0394 bias-correction model).
func newSidecarCmd() *cobra.Command {
	cmd := clikit.Command("sidecar",
		"Train and inspect the local on-host predictor sidecar (ADR-0394)")
	cmd.Long = `Train and inspect the local on-host predictor sidecar.

The shipped predictor is a fixed, deterministic asset. The sidecar is a
bias-correction term you train on your own host from the residuals between
predicted VMAF and the libvmaf score actually observed at encode time:

  sidecar_vmaf = predictor_vmaf + sidecar_correction

State lives under ${XDG_CACHE_HOME:-~/.cache}/vmaf-tune/sidecar/<predictor
version>/<codec>/state.json alongside an anonymous random host UUID. A
predictor-version mismatch on load discards everything except the UUID and
resets to cold start, so a shipped-model upgrade can never replay a stale
correction.

Subcommands:
  status        print sidecar state metadata
  predict       predict VMAF with the sidecar correction folded in
  record        record one observed encode result into the sidecar fit
  batch-record  record a JSONL capture file, one observation per row

See docs/ai/local-sidecar-training.md for the operator guide.`

	cmd.AddCommand(
		newSidecarStatusCmd(),
		newSidecarPredictCmd(),
		newSidecarRecordCmd(),
		newSidecarBatchRecordCmd(),
	)
	return cmd
}

// addSidecarCommonFlags wires the shared local-sidecar configuration flags.
func addSidecarCommonFlags(cmd *cobra.Command, flags *sidecarFlags) {
	cmd.Flags().StringVar(&flags.codec, "codec", "libx264",
		"Codec bucket for the sidecar state; one of "+sidecarKnownCodecs())
	cmd.Flags().StringVar(&flags.cacheDir, "cache-dir", "",
		"Sidecar cache root (default ${XDG_CACHE_HOME:-~/.cache}/vmaf-tune/sidecar)")
	cmd.Flags().StringVar(&flags.predictorVersion, "predictor-version",
		sidecar.DefaultPredictorVersion,
		fmt.Sprintf("Predictor version namespace (default %s)", sidecar.DefaultPredictorVersion))
	cmd.Flags().StringVar(&flags.model, "model", "",
		"Optional predictor_<codec>.onnx path; default uses the analytical fallback")
}

// addSidecarJSONFlag wires the --json machine-readable output flag.
func addSidecarJSONFlag(cmd *cobra.Command, flags *sidecarFlags) {
	cmd.Flags().BoolVar(&flags.asJSON, "json", false, "Emit machine-readable JSON")
}

// newSidecarStatusCmd builds "sidecar status".
func newSidecarStatusCmd() *cobra.Command {
	flags := &sidecarFlags{}
	cmd := clikit.Command("status", "Print sidecar state metadata",
		clikit.WithRunE(withGolusoris(func(ctx context.Context, d deps, _ []string) error {
			return runSidecarStatus(ctx, d, flags)
		})),
	)
	cmd.Long = `Print the local sidecar's state metadata for one codec bucket.

The payload reports the anonymous host UUID, the on-disk state path, the
predictor-version namespace the state was trained against, how many captures
have been folded in, and the RMS of the buffered residuals (the drift signal).

A cold-start sidecar reports updates=0 and residual_rms=0.000000.

Example:
  vmafx-tune-go sidecar status --codec libx264 --json`
	addSidecarCommonFlags(cmd, flags)
	addSidecarJSONFlag(cmd, flags)
	return cmd
}

// newSidecarPredictCmd builds "sidecar predict".
func newSidecarPredictCmd() *cobra.Command {
	flags := &sidecarFlags{}
	cmd := clikit.Command("predict",
		"Predict VMAF with the sidecar correction folded in",
		clikit.WithRunE(withGolusoris(func(ctx context.Context, d deps, _ []string) error {
			return runSidecarPredict(ctx, d, flags)
		})),
	)
	cmd.Long = `Predict VMAF for one shot at one CRF with the sidecar correction applied.

--features-json points at a JSON object carrying the ShotFeatures fields
(probe_bitrate_kbps, probe_i_frame_avg_bytes, saliency_mean, y_var, fps, ...).
Missing fields default to zero.

Example:
  vmafx-tune-go sidecar predict --features-json shot.json --crf 23 --json`
	addSidecarCommonFlags(cmd, flags)
	cmd.Flags().StringVar(&flags.featuresJSON, "features-json", "",
		"Path to a JSON object carrying the shot's feature values (required)")
	cmd.Flags().IntVar(&flags.crf, "crf", 0, "CRF to predict at (required)")
	addSidecarJSONFlag(cmd, flags)
	_ = cmd.MarkFlagRequired("features-json")
	_ = cmd.MarkFlagRequired("crf")
	return cmd
}

// newSidecarRecordCmd builds "sidecar record".
func newSidecarRecordCmd() *cobra.Command {
	flags := &sidecarFlags{}
	cmd := clikit.Command("record",
		"Record one observed encode result into the sidecar fit",
		clikit.WithRunE(withGolusoris(func(ctx context.Context, d deps, _ []string) error {
			return runSidecarRecord(ctx, d, flags)
		})),
	)
	cmd.Long = `Fold one observed VMAF measurement into the local sidecar's ridge fit.

The residual (observed - predicted) is computed against the bare predictor,
never against the sidecar-corrected value, so repeated captures converge rather
than compounding. State is persisted unless --no-persist is passed.

Example:
  vmafx-tune-go sidecar record \
    --features-json shot.json \
    --crf 23 \
    --observed-vmaf 94.2 \
    --json`
	addSidecarCommonFlags(cmd, flags)
	cmd.Flags().StringVar(&flags.featuresJSON, "features-json", "",
		"Path to a JSON object carrying the shot's feature values (required)")
	cmd.Flags().IntVar(&flags.crf, "crf", 0, "CRF the observation was measured at (required)")
	cmd.Flags().Float64Var(&flags.observedVMAF, "observed-vmaf", 0,
		"Observed libvmaf score for the encode (required)")
	cmd.Flags().BoolVar(&flags.noPersist, "no-persist", false,
		"Update in memory only; mainly useful for tests")
	addSidecarJSONFlag(cmd, flags)
	_ = cmd.MarkFlagRequired("features-json")
	_ = cmd.MarkFlagRequired("crf")
	_ = cmd.MarkFlagRequired("observed-vmaf")
	return cmd
}

// newSidecarBatchRecordCmd builds "sidecar batch-record".
func newSidecarBatchRecordCmd() *cobra.Command {
	flags := &sidecarFlags{}
	cmd := clikit.Command("batch-record",
		"Record a JSONL capture file with one encode observation per row",
		clikit.WithRunE(withGolusoris(func(ctx context.Context, d deps, _ []string) error {
			return runSidecarBatchRecord(ctx, d, flags)
		})),
	)
	cmd.Long = `Fold a whole JSONL capture file into the local sidecar's ridge fit.

Each line is a JSON object carrying the ShotFeatures fields plus "crf" and
"observed_vmaf". Malformed rows are skipped with a one-line diagnostic on
stderr and counted in rows_skipped; the run still succeeds so a single bad line
does not discard a long capture session.

State is written once at the end rather than per row, so a 10k-row capture
costs one filesystem write.

Example:
  vmafx-tune-go sidecar batch-record --captures-jsonl captures.jsonl --json`
	addSidecarCommonFlags(cmd, flags)
	cmd.Flags().StringVar(&flags.capturesJSON, "captures-jsonl", "",
		"Path to the JSONL capture file, one observation per line (required)")
	addSidecarJSONFlag(cmd, flags)
	_ = cmd.MarkFlagRequired("captures-jsonl")
	return cmd
}

// buildSidecarPredictor constructs the configured sidecar for a CLI handler.
func buildSidecarPredictor(flags *sidecarFlags) (*sidecar.Predictor, error) {
	if _, err := codecadapter.Get(flags.codec); err != nil {
		return nil, err
	}
	cfg := sidecar.NewConfig()
	cfg.PredictorVersion = flags.predictorVersion
	if flags.cacheDir != "" {
		cfg.CacheDir = flags.cacheDir
	}
	base, err := newBasePredictor(flags.model)
	if err != nil {
		return nil, err
	}
	return sidecar.ForCodec(base, flags.codec, cfg)
}

// newBasePredictor builds the wrapped predictor for the requested model path.
//
// The Python Predictor loads an ONNX graph through onnxruntime when
// --model is set, and silently falls back to the analytical curve when
// onnxruntime is not importable. The Go port has no in-process ONNX runtime, so
// a --model path is rejected rather than silently scored against a different
// model than the operator asked for: a wrong-but-plausible VMAF number is worse
// than a clear refusal.
func newBasePredictor(modelPath string) (*predictor.Predictor, error) {
	if modelPath != "" {
		return nil, fmt.Errorf(
			"--model %q: the Go sidecar has no in-process ONNX runtime, so a learned "+
				"predictor_<codec>.onnx cannot be loaded; omit --model to use the "+
				"analytical fallback, or run 'vmaf-tune sidecar' for the ONNX path",
			modelPath)
	}
	return predictor.New(), nil
}

// sidecarStatusPayload returns the machine-readable status payload.
func sidecarStatusPayload(sp *sidecar.Predictor) map[string]any {
	return map[string]any{
		"schema":              "vmaf-tune-sidecar-status/v1",
		"codec":               sp.Codec,
		"host_uuid":           sp.HostUUID,
		"state_path":          sp.StatePath,
		"predictor_version":   sp.Model.Config.PredictorVersion,
		"schema_version":      sp.Model.SchemaVersionOf(),
		"n_updates":           sp.Model.NUpdates,
		"recent_residual_rms": sp.Model.RecentResidualRMS(),
	}
}

// emitSidecarPayload writes a payload to stdout as JSON or as the plain-text
// one-liner, matching the Python formatters.
func emitSidecarPayload(payload map[string]any, asJSON bool, textLine string) error {
	if asJSON {
		encoded, err := pyjson.MarshalIndentSorted(payload, 2)
		if err != nil {
			return fmt.Errorf("encode sidecar payload: %w", err)
		}
		_, err = fmt.Fprintln(os.Stdout, encoded)
		return err
	}
	_, err := fmt.Fprint(os.Stdout, textLine)
	return err
}

// payloadString / payloadInt / payloadFloat read a payload field with the
// zero value as the fallback, keeping the text formatters total.
func payloadString(p map[string]any, key string) string {
	v, _ := p[key].(string)
	return v
}

func payloadInt(p map[string]any, key string) int {
	v, _ := p[key].(int)
	return v
}

func payloadFloat(p map[string]any, key string) float64 {
	v, _ := p[key].(float64)
	return v
}

// runSidecarStatus implements "sidecar status".
func runSidecarStatus(ctx context.Context, d deps, flags *sidecarFlags) error {
	sp, err := buildSidecarPredictor(flags)
	if err != nil {
		return fmt.Errorf("vmafx-tune sidecar: %w", err)
	}
	payload := sidecarStatusPayload(sp)
	d.Log.InfoContext(ctx, "sidecar status",
		"codec", sp.Codec, "state_path", sp.StatePath, "n_updates", sp.Model.NUpdates)
	return emitSidecarPayload(payload, flags.asJSON, fmt.Sprintf(
		"codec=%s predictor_version=%s updates=%d residual_rms=%.6f state=%s\n",
		payloadString(payload, "codec"),
		payloadString(payload, "predictor_version"),
		payloadInt(payload, "n_updates"),
		payloadFloat(payload, "recent_residual_rms"),
		payloadString(payload, "state_path")))
}

// runSidecarPredict implements "sidecar predict".
func runSidecarPredict(ctx context.Context, d deps, flags *sidecarFlags) error {
	sp, err := buildSidecarPredictor(flags)
	if err != nil {
		return fmt.Errorf("vmafx-tune sidecar: %w", err)
	}
	features, err := readShotFeaturesFile(flags.featuresJSON)
	if err != nil {
		return fmt.Errorf("vmafx-tune sidecar predict: %w", err)
	}
	base := sp.Base.PredictVMAF(features, flags.crf, flags.codec)
	correction := sp.Model.PredictCorrection(features, flags.crf)
	sidecarVMAF := sp.PredictVMAF(features, flags.crf, "")

	payload := map[string]any{
		"schema":       "vmaf-tune-sidecar-predict/v1",
		"codec":        flags.codec,
		"crf":          flags.crf,
		"base_vmaf":    base,
		"correction":   correction,
		"sidecar_vmaf": sidecarVMAF,
		"n_updates":    sp.Model.NUpdates,
	}
	d.Log.InfoContext(ctx, "sidecar predict",
		"codec", flags.codec, "crf", flags.crf, "sidecar_vmaf", sidecarVMAF)
	return emitSidecarPayload(payload, flags.asJSON, fmt.Sprintf(
		"base=%.6f correction=%.6f sidecar=%.6f updates=%d\n",
		base, correction, sidecarVMAF, sp.Model.NUpdates))
}

// runSidecarRecord implements "sidecar record".
func runSidecarRecord(ctx context.Context, d deps, flags *sidecarFlags) error {
	sp, err := buildSidecarPredictor(flags)
	if err != nil {
		return fmt.Errorf("vmafx-tune sidecar: %w", err)
	}
	features, err := readShotFeaturesFile(flags.featuresJSON)
	if err != nil {
		return fmt.Errorf("vmafx-tune sidecar record: %w", err)
	}
	base := sp.Base.PredictVMAF(features, flags.crf, flags.codec)
	if rErr := sp.RecordCapture(features, flags.crf, flags.observedVMAF, "",
		!flags.noPersist); rErr != nil {
		return fmt.Errorf("vmafx-tune sidecar record: %w", rErr)
	}

	payload := sidecarStatusPayload(sp)
	payload["schema"] = "vmaf-tune-sidecar-record/v1"
	payload["crf"] = flags.crf
	payload["observed_vmaf"] = flags.observedVMAF
	payload["base_vmaf"] = base
	payload["residual"] = flags.observedVMAF - base

	d.Log.InfoContext(ctx, "sidecar capture recorded",
		"codec", sp.Codec, "crf", flags.crf, "n_updates", sp.Model.NUpdates)
	return emitSidecarPayload(payload, flags.asJSON, fmt.Sprintf(
		"recorded updates=%d residual=%.6f state=%s\n",
		payloadInt(payload, "n_updates"),
		payloadFloat(payload, "residual"),
		payloadString(payload, "state_path")))
}

// runSidecarBatchRecord implements "sidecar batch-record".
func runSidecarBatchRecord(ctx context.Context, d deps, flags *sidecarFlags) error {
	sp, err := buildSidecarPredictor(flags)
	if err != nil {
		return fmt.Errorf("vmafx-tune sidecar: %w", err)
	}

	rows, skipped, err := foldCapturesFile(sp, flags.capturesJSON)
	if err != nil {
		return fmt.Errorf("vmafx-tune sidecar batch-record: cannot read input: %w", err)
	}
	if rows > 0 {
		if sErr := sp.Save(); sErr != nil {
			return fmt.Errorf("vmafx-tune sidecar batch-record: %w", sErr)
		}
	}

	payload := sidecarStatusPayload(sp)
	payload["schema"] = "vmaf-tune-sidecar-batch-record/v1"
	payload["rows_recorded"] = rows
	payload["rows_skipped"] = skipped

	d.Log.InfoContext(ctx, "sidecar batch capture recorded",
		"codec", sp.Codec, "rows_recorded", rows, "rows_skipped", skipped)
	return emitSidecarPayload(payload, flags.asJSON, fmt.Sprintf(
		"recorded=%d skipped=%d updates=%d state=%s\n",
		rows, skipped,
		payloadInt(payload, "n_updates"),
		payloadString(payload, "state_path")))
}

// foldCapturesFile folds every well-formed row of a JSONL capture file into sp
// without persisting, returning (recorded, skipped).
//
// A malformed row is reported on stderr and counted rather than aborting: a
// long capture session should not be discarded because one line was truncated.
func foldCapturesFile(sp *sidecar.Predictor, path string) (int, int, error) {
	f, err := os.Open(path) // #nosec G304 -- operator-supplied --captures-jsonl path.
	if err != nil {
		return 0, 0, err
	}
	defer func() { _ = f.Close() }()

	rows, skipped := 0, 0
	sc := bufio.NewScanner(f)
	// Capture rows are small, but a feature dump with long path strings can
	// exceed bufio's 64 KiB default token size.
	sc.Buffer(make([]byte, 0, 64*1024), 4*1024*1024)
	lineno := 0
	for sc.Scan() {
		lineno++
		line := strings.TrimSpace(sc.Text())
		if line == "" {
			continue
		}
		features, crf, observed, parseErr := parseCaptureRow(line)
		if parseErr != nil {
			skipped++
			fmt.Fprintf(os.Stderr, "vmafx-tune sidecar batch-record: skip line %d: %v\n",
				lineno, parseErr)
			continue
		}
		// persist=false: the state is written once after the loop.
		if rErr := sp.RecordCapture(features, crf, observed, "", false); rErr != nil {
			return rows, skipped, rErr
		}
		rows++
	}
	if scanErr := sc.Err(); scanErr != nil {
		return rows, skipped, scanErr
	}
	return rows, skipped, nil
}

// parseCaptureRow decodes one JSONL capture row.
func parseCaptureRow(line string) (predictor.ShotFeatures, int, float64, error) {
	var raw map[string]any
	if err := json.Unmarshal([]byte(line), &raw); err != nil {
		return predictor.ShotFeatures{}, 0, 0, err
	}
	features, err := shotFeaturesFromMapping(raw)
	if err != nil {
		return predictor.ShotFeatures{}, 0, 0, err
	}
	crfRaw, ok := raw["crf"]
	if !ok {
		return predictor.ShotFeatures{}, 0, 0, errors.New("'crf'")
	}
	crf, ok := numericField(crfRaw)
	if !ok {
		return predictor.ShotFeatures{}, 0, 0, fmt.Errorf("invalid crf value %v", crfRaw)
	}
	observedRaw, ok := raw["observed_vmaf"]
	if !ok {
		return predictor.ShotFeatures{}, 0, 0, errors.New("'observed_vmaf'")
	}
	observed, ok := numericField(observedRaw)
	if !ok {
		return predictor.ShotFeatures{}, 0, 0,
			fmt.Errorf("invalid observed_vmaf value %v", observedRaw)
	}
	return features, int(crf), observed, nil
}

// readShotFeaturesFile reads a JSON object into a ShotFeatures.
func readShotFeaturesFile(path string) (predictor.ShotFeatures, error) {
	data, err := os.ReadFile(path) // #nosec G304 -- operator-supplied --features-json path.
	if err != nil {
		return predictor.ShotFeatures{}, err
	}
	var raw map[string]any
	if uErr := json.Unmarshal(data, &raw); uErr != nil {
		return predictor.ShotFeatures{}, fmt.Errorf("%s is not a JSON object: %w", path, uErr)
	}
	return shotFeaturesFromMapping(raw)
}

// shotFeatureFloatFields maps the JSON key of every float ShotFeatures field to
// its address-taker, so the decoder stays a single table rather than a wall of
// type switches.
var shotFeatureFloatFields = map[string]func(*predictor.ShotFeatures) *float64{
	"probe_bitrate_kbps":      func(f *predictor.ShotFeatures) *float64 { return &f.ProbeBitrateKbps },
	"probe_i_frame_avg_bytes": func(f *predictor.ShotFeatures) *float64 { return &f.ProbeIFrameAvgBytes },
	"probe_p_frame_avg_bytes": func(f *predictor.ShotFeatures) *float64 { return &f.ProbePFrameAvgBytes },
	"probe_b_frame_avg_bytes": func(f *predictor.ShotFeatures) *float64 { return &f.ProbeBFrameAvgBytes },
	"saliency_mean":           func(f *predictor.ShotFeatures) *float64 { return &f.SaliencyMean },
	"saliency_var":            func(f *predictor.ShotFeatures) *float64 { return &f.SaliencyVar },
	"frame_diff_mean":         func(f *predictor.ShotFeatures) *float64 { return &f.FrameDiffMean },
	"y_avg":                   func(f *predictor.ShotFeatures) *float64 { return &f.YAvg },
	"y_var":                   func(f *predictor.ShotFeatures) *float64 { return &f.YVar },
	"fps":                     func(f *predictor.ShotFeatures) *float64 { return &f.FPS },
}

// shotFeatureIntFields is the integer counterpart of shotFeatureFloatFields.
var shotFeatureIntFields = map[string]func(*predictor.ShotFeatures) *int{
	"shot_length_frames": func(f *predictor.ShotFeatures) *int { return &f.ShotLengthFrames },
	"width":              func(f *predictor.ShotFeatures) *int { return &f.Width },
	"height":             func(f *predictor.ShotFeatures) *int { return &f.Height },
}

// sidecarRequiredFeatureKeys are the four probe-encode fields a capture must
// carry. They have no meaningful default — a zero probe bitrate would silently
// train the ridge fit on a fabricated complexity barometer — so a row missing
// any of them is rejected rather than defaulted.
var sidecarRequiredFeatureKeys = []string{
	"probe_bitrate_kbps",
	"probe_i_frame_avg_bytes",
	"probe_p_frame_avg_bytes",
	"probe_b_frame_avg_bytes",
}

// shotFeaturesFromMapping builds a ShotFeatures from a decoded JSON object.
//
// A row may either carry the feature fields at the top level or nest them under
// a "features" key; the wrapper form is unwrapped first. The four
// sidecarRequiredFeatureKeys must be present. Every other field defaults to
// zero (the Python dataclass behaviour); a present but non-numeric value is an
// error rather than a silent zero, so a typo in a capture file surfaces instead
// of poisoning the fit.
func shotFeaturesFromMapping(row map[string]any) (predictor.ShotFeatures, error) {
	raw := row
	if wrapped, ok := row["features"]; ok {
		nested, isObject := wrapped.(map[string]any)
		if !isObject {
			return predictor.ShotFeatures{}, errors.New("'features' must be a JSON object")
		}
		raw = nested
	}

	var missing []string
	for _, key := range sidecarRequiredFeatureKeys {
		if _, ok := raw[key]; !ok {
			missing = append(missing, key)
		}
	}
	if len(missing) > 0 {
		return predictor.ShotFeatures{},
			fmt.Errorf("features missing required keys: %s", strings.Join(missing, ", "))
	}

	var f predictor.ShotFeatures
	for key, field := range shotFeatureFloatFields {
		v, ok := raw[key]
		if !ok || v == nil {
			continue
		}
		num, ok := numericField(v)
		if !ok {
			return predictor.ShotFeatures{},
				fmt.Errorf("invalid sidecar feature value: %s=%v", key, v)
		}
		*field(&f) = num
	}
	for key, field := range shotFeatureIntFields {
		v, ok := raw[key]
		if !ok || v == nil {
			continue
		}
		num, ok := numericField(v)
		if !ok {
			return predictor.ShotFeatures{},
				fmt.Errorf("invalid sidecar feature value: %s=%v", key, v)
		}
		*field(&f) = int(num)
	}
	return f, nil
}

// numericField coerces a decoded JSON value to float64. Strings are accepted
// because Python's float() / int() constructors take them and some capture
// producers quote their numerics.
func numericField(v any) (float64, bool) {
	switch t := v.(type) {
	case float64:
		if math.IsNaN(t) {
			return 0, false
		}
		return t, true
	case json.Number:
		f, err := t.Float64()
		return f, err == nil
	case string:
		var f float64
		if _, err := fmt.Sscanf(strings.TrimSpace(t), "%g", &f); err != nil {
			return 0, false
		}
		return f, true
	case bool:
		// Python's float(True) is 1.0; matched so a producer that emits
		// JSON booleans behaves identically under both implementations.
		if t {
			return 1, true
		}
		return 0, true
	default:
		return 0, false
	}
}

// sidecarKnownCodecs returns the codec names --codec accepts, for help text.
func sidecarKnownCodecs() string {
	names := codecadapter.KnownCodecs()
	sort.Strings(names)
	return strings.Join(names, ", ")
}
