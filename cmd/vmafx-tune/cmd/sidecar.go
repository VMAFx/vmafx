// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package cmd

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"strconv"
	"strings"

	"github.com/golusoris/golusoris/clikit"
	"github.com/spf13/cobra"
	"github.com/spf13/pflag"

	"github.com/VMAFx/vmafx/pkg/tune/codec"
	"github.com/VMAFx/vmafx/pkg/tune/predictor"
	"github.com/VMAFx/vmafx/pkg/tune/pyjson"
	"github.com/VMAFx/vmafx/pkg/tune/sidecar"
)

// sidecarCommonFlags are the shared local-sidecar configuration flags every
// nested subcommand carries. Names mirror the Python argparse surface.
type sidecarCommonFlags struct {
	codec            string
	cacheDir         string
	predictorVersion string
	model            string
	jsonOut          bool

	// flagSet is the command's own flag set, recorded so run functions can
	// ask which flags were actually passed (see requireFlags).
	flagSet *pflag.FlagSet
}

// requireFlags mirrors argparse's required=True: every named flag must have
// been passed on the command line. The check lives here rather than in
// cobra's MarkFlagRequired so a missing flag exits 2 like the Python CLI
// (see useUsageExitCode); the message follows argparse's wording.
func (f *sidecarCommonFlags) requireFlags(names ...string) error {
	var missing []string
	for _, name := range names {
		if f.flagSet == nil || !f.flagSet.Changed(name) {
			missing = append(missing, "--"+name)
		}
	}
	if len(missing) == 0 {
		return nil
	}
	return asUsageError(fmt.Errorf(
		"the following arguments are required: %s", strings.Join(missing, ", ")))
}

// sidecarRequiredFeatureKeys are the feature-JSON keys with no default. The
// remaining ShotFeatures fields default to 0.
var sidecarRequiredFeatureKeys = []string{
	"probe_bitrate_kbps",
	"probe_i_frame_avg_bytes",
	"probe_p_frame_avg_bytes",
	"probe_b_frame_avg_bytes",
}

// newSidecarCmd builds the "sidecar" subcommand group.
func newSidecarCmd() *cobra.Command {
	cmd := clikit.Command("sidecar",
		"Train and inspect the local on-host predictor sidecar (ADR-0394)")
	cmd.Long = `Train and inspect the local on-host predictor sidecar.

The shipped VMAF predictor is a fixed, deterministic asset. The sidecar is a
bias-correction term you train on your own host from the residuals between
predicted VMAF and the libvmaf score actually observed at encode time:

  sidecar_vmaf = predictor_vmaf + sidecar_correction(features)

The shipped predictor is never mutated, so model upgrades stay deterministic
across hosts. State lives under
${XDG_CACHE_HOME:-~/.cache}/vmaf-tune/sidecar/<predictor-version>/<codec>/,
alongside an anonymous random host UUID that is never derived from any
machine-identifying signal. A predictor-version mismatch on load discards the
fit and resets to cold start — with zero weights the correction is exactly
0.0, so the sidecar degenerates to the bare predictor until the first capture.

Subcommands:
  status        print sidecar state metadata
  predict       predict VMAF with the sidecar correction folded in
  record        record one observed encode result into the fit
  batch-record  record a JSONL capture file, one observation per row

Exit status follows the Python vmaf-tune sidecar: 0 on success, 2 for a usage
or validation failure (bad flag, unknown codec, unreadable or malformed input),
1 when the cache directory or state file cannot be written.`

	cmd.AddCommand(newSidecarStatusCmd())
	cmd.AddCommand(newSidecarPredictCmd())
	cmd.AddCommand(newSidecarRecordCmd())
	cmd.AddCommand(newSidecarBatchRecordCmd())

	return cmd
}

// addSidecarCommonFlags wires the shared configuration flags onto cmd and
// installs the Python exit-status convention for flag-layer failures.
func addSidecarCommonFlags(cmd *cobra.Command, flags *sidecarCommonFlags) {
	cmd.Flags().StringVar(&flags.codec, "codec", "libx264",
		"codec bucket for the sidecar state (default libx264)")
	cmd.Flags().StringVar(&flags.cacheDir, "cache-dir", "",
		"sidecar cache root (default ${XDG_CACHE_HOME:-~/.cache}/vmaf-tune/sidecar)")
	cmd.Flags().StringVar(&flags.predictorVersion, "predictor-version",
		sidecar.DefaultPredictorVersion,
		"predictor version namespace (default "+sidecar.DefaultPredictorVersion+")")
	cmd.Flags().StringVar(&flags.model, "model", "",
		"optional predictor_<codec>.onnx path; default uses the analytical fallback")
	cmd.Flags().BoolVar(&flags.jsonOut, "json", false,
		"emit machine-readable JSON")
	flags.flagSet = cmd.Flags()

	// Required-flag enforcement lives in each run function (requireFlags),
	// not MarkFlagRequired, so a missing flag exits 2 like the Python CLI.
	useUsageExitCode(cmd)
}

// buildSidecarPredictor constructs the configured sidecar for a CLI handler.
//
// Exit-status mapping mirrors _run_sidecar in the Python CLI: argparse rejects
// an unknown --codec and Predictor() raises on an unresolvable --model, both
// exit 2, so those errors carry the usage status. A cache-directory I/O
// failure while creating the host UUID is an uncaught OSError in Python — a
// plain exit 1 — so ForCodec's error is returned untagged.
func buildSidecarPredictor(flags *sidecarCommonFlags, d deps) (*sidecar.Predictor, error) {
	if _, err := codec.Get(flags.codec); err != nil {
		return nil, asUsageError(fmt.Errorf("--codec: %w", err))
	}
	base, err := predictor.New(flags.model, d.Log)
	if err != nil {
		return nil, asUsageError(err)
	}
	cfg := sidecar.Config{
		PredictorVersion: flags.predictorVersion,
		CacheDir:         flags.cacheDir,
	}
	return sidecar.ForCodec(base, flags.codec, cfg)
}

// ---------------------------------------------------------------------------
// status
// ---------------------------------------------------------------------------

func newSidecarStatusCmd() *cobra.Command {
	flags := &sidecarCommonFlags{}
	cmd := clikit.Command("status", "Print sidecar state metadata",
		clikit.WithRunE(withGolusoris(func(ctx context.Context, d deps, _ []string) error {
			return runSidecarStatus(ctx, d, flags)
		})),
	)
	addSidecarCommonFlags(cmd, flags)
	return cmd
}

func runSidecarStatus(_ context.Context, d deps, flags *sidecarCommonFlags) error {
	sp, err := buildSidecarPredictor(flags, d)
	if err != nil {
		return err
	}
	return emitSidecarStatus(sidecarStatusPayload(sp), flags.jsonOut)
}

// sidecarStatusPayload is the machine-readable status block. Keys and schema
// tag match the Python _sidecar_status_payload.
func sidecarStatusPayload(sp *sidecar.Predictor) map[string]any {
	return map[string]any{
		"schema":              "vmaf-tune-sidecar-status/v1",
		"codec":               sp.Codec,
		"host_uuid":           sp.HostUUID,
		"state_path":          sp.StatePath,
		"predictor_version":   sp.Model.Config.PredictorVersion,
		"schema_version":      sidecar.SchemaVersion,
		"n_updates":           sp.Model.NUpdates,
		"recent_residual_rms": sp.Model.RecentResidualRMS(),
	}
}

func emitSidecarStatus(payload map[string]any, asJSON bool) error {
	if asJSON {
		return emitJSONPayload(payload)
	}
	_, err := fmt.Printf(
		"codec=%s predictor_version=%s updates=%d residual_rms=%.6f state=%s\n",
		payload["codec"], payload["predictor_version"], payload["n_updates"],
		payload["recent_residual_rms"], payload["state_path"])
	return err
}

func emitJSONPayload(payload map[string]any) error {
	rendered, err := pyjson.Marshal(payload, 2)
	if err != nil {
		return fmt.Errorf("render sidecar payload: %w", err)
	}
	_, err = fmt.Println(rendered)
	return err
}

// ---------------------------------------------------------------------------
// predict
// ---------------------------------------------------------------------------

type sidecarPredictFlags struct {
	sidecarCommonFlags
	featuresJSON string
	crf          int
}

func newSidecarPredictCmd() *cobra.Command {
	flags := &sidecarPredictFlags{}
	cmd := clikit.Command("predict",
		"Predict VMAF with the sidecar correction folded in",
		clikit.WithRunE(withGolusoris(func(ctx context.Context, d deps, _ []string) error {
			return runSidecarPredict(ctx, d, flags)
		})),
	)
	addSidecarCommonFlags(cmd, &flags.sidecarCommonFlags)
	cmd.Flags().StringVar(&flags.featuresJSON, "features-json", "",
		"path to a JSON object of ShotFeatures (or a {\"features\": {...}} wrapper) (required)")
	cmd.Flags().IntVar(&flags.crf, "crf", 0, "CRF the prediction is for (required)")
	return cmd
}

func runSidecarPredict(_ context.Context, d deps, flags *sidecarPredictFlags) error {
	if err := flags.requireFlags("features-json", "crf"); err != nil {
		return err
	}
	sp, err := buildSidecarPredictor(&flags.sidecarCommonFlags, d)
	if err != nil {
		return err
	}
	features, err := loadSidecarFeatures(flags.featuresJSON)
	if err != nil {
		return err
	}

	base := sp.Base.PredictVMAF(features, flags.crf, flags.codec)
	correction := sp.Model.PredictCorrection(features, flags.crf)
	payload := map[string]any{
		"schema":       "vmaf-tune-sidecar-predict/v1",
		"codec":        flags.codec,
		"crf":          flags.crf,
		"base_vmaf":    base,
		"correction":   correction,
		"sidecar_vmaf": sp.PredictVMAF(features, flags.crf, flags.codec),
		"n_updates":    sp.Model.NUpdates,
	}
	if flags.jsonOut {
		return emitJSONPayload(payload)
	}
	_, err = fmt.Printf("base=%.6f correction=%.6f sidecar=%.6f updates=%d\n",
		payload["base_vmaf"], payload["correction"], payload["sidecar_vmaf"],
		payload["n_updates"])
	return err
}

// ---------------------------------------------------------------------------
// record
// ---------------------------------------------------------------------------

type sidecarRecordFlags struct {
	sidecarCommonFlags
	featuresJSON string
	crf          int
	observedVMAF float64
	noPersist    bool
}

func newSidecarRecordCmd() *cobra.Command {
	flags := &sidecarRecordFlags{}
	cmd := clikit.Command("record",
		"Record one observed encode result into the sidecar fit",
		clikit.WithRunE(withGolusoris(func(ctx context.Context, d deps, _ []string) error {
			return runSidecarRecord(ctx, d, flags)
		})),
	)
	addSidecarCommonFlags(cmd, &flags.sidecarCommonFlags)
	cmd.Flags().StringVar(&flags.featuresJSON, "features-json", "",
		"path to a JSON object of ShotFeatures (or a {\"features\": {...}} wrapper) (required)")
	cmd.Flags().IntVar(&flags.crf, "crf", 0, "CRF the observation was encoded at (required)")
	cmd.Flags().Float64Var(&flags.observedVMAF, "observed-vmaf", 0,
		"the pooled-mean VMAF libvmaf actually measured (required)")
	cmd.Flags().BoolVar(&flags.noPersist, "no-persist", false,
		"update in memory only; mainly useful for tests")
	return cmd
}

func runSidecarRecord(_ context.Context, d deps, flags *sidecarRecordFlags) error {
	if err := flags.requireFlags("features-json", "crf", "observed-vmaf"); err != nil {
		return err
	}
	sp, err := buildSidecarPredictor(&flags.sidecarCommonFlags, d)
	if err != nil {
		return err
	}
	features, err := loadSidecarFeatures(flags.featuresJSON)
	if err != nil {
		return err
	}

	base := sp.Base.PredictVMAF(features, flags.crf, flags.codec)
	// A persistence failure here is an uncaught OSError in Python (exit 1),
	// so the error stays untagged.
	if err := sp.RecordCapture(
		features, flags.crf, flags.observedVMAF, flags.codec, !flags.noPersist,
	); err != nil {
		return err
	}

	payload := sidecarStatusPayload(sp)
	payload["schema"] = "vmaf-tune-sidecar-record/v1"
	payload["crf"] = flags.crf
	payload["observed_vmaf"] = flags.observedVMAF
	payload["base_vmaf"] = base
	payload["residual"] = flags.observedVMAF - base

	if flags.jsonOut {
		return emitJSONPayload(payload)
	}
	_, err = fmt.Printf("recorded updates=%d residual=%.6f state=%s\n",
		payload["n_updates"], payload["residual"], payload["state_path"])
	return err
}

// ---------------------------------------------------------------------------
// batch-record
// ---------------------------------------------------------------------------

type sidecarBatchFlags struct {
	sidecarCommonFlags
	capturesJSONL string
}

func newSidecarBatchRecordCmd() *cobra.Command {
	flags := &sidecarBatchFlags{}
	cmd := clikit.Command("batch-record",
		"Record a JSONL capture file with one encode observation per row",
		clikit.WithRunE(withGolusoris(func(ctx context.Context, d deps, _ []string) error {
			return runSidecarBatchRecord(ctx, d, flags)
		})),
	)
	addSidecarCommonFlags(cmd, &flags.sidecarCommonFlags)
	cmd.Flags().StringVar(&flags.capturesJSONL, "captures-jsonl", "",
		"JSONL file: one object per line carrying the feature keys plus "+
			"\"crf\" and \"observed_vmaf\" (required)")
	return cmd
}

// runSidecarBatchRecord folds every well-formed row into the fit and persists
// once at the end. A malformed row is skipped with a stderr note rather than
// aborting the batch — an operator's capture log is often partially corrupt
// after an interrupted run, and losing the good rows to one bad line is worse
// than the noise.
//
// The file is read whole and split with CPython's universal-newline rule, so
// line numbers in the skip diagnostics match the Python CLI's and there is no
// line-length ceiling (Python has none either).
func runSidecarBatchRecord(_ context.Context, d deps, flags *sidecarBatchFlags) error {
	if err := flags.requireFlags("captures-jsonl"); err != nil {
		return err
	}
	sp, err := buildSidecarPredictor(&flags.sidecarCommonFlags, d)
	if err != nil {
		return err
	}
	data, err := os.ReadFile(flags.capturesJSONL) // #nosec G304 -- operator-supplied CLI flag
	if err != nil {
		// Python: `except OSError` around the read → exit 2.
		return asUsageError(fmt.Errorf("cannot read input: %w", err))
	}

	rows, skipped := 0, 0
	for index, raw := range splitLinesUniversal(string(data)) {
		lineNo := index + 1
		line := strings.TrimSpace(raw)
		if line == "" {
			continue
		}
		features, crf, observed, err := parseCaptureRow(line)
		if err != nil {
			skipped++
			fmt.Fprintf(os.Stderr,
				"vmafx-tune sidecar batch-record: skip line %d: %v\n", lineNo, err)
			continue
		}
		if err := sp.RecordCapture(features, crf, observed, flags.codec, false); err != nil {
			skipped++
			fmt.Fprintf(os.Stderr,
				"vmafx-tune sidecar batch-record: skip line %d: %v\n", lineNo, err)
			continue
		}
		rows++
	}
	if rows > 0 {
		// Python: sp.save() outside the try block → an uncaught OSError, exit 1.
		if err := sp.Save(); err != nil {
			return err
		}
	}

	payload := sidecarStatusPayload(sp)
	payload["schema"] = "vmaf-tune-sidecar-batch-record/v1"
	payload["rows_recorded"] = rows
	payload["rows_skipped"] = skipped

	if flags.jsonOut {
		return emitJSONPayload(payload)
	}
	_, err = fmt.Printf("recorded=%d skipped=%d updates=%d state=%s\n",
		payload["rows_recorded"], payload["rows_skipped"],
		payload["n_updates"], payload["state_path"])
	return err
}

// splitLinesUniversal splits text the way CPython's text-mode file iteration
// does with newline=None: "\n", "\r\n" and a lone "\r" each terminate a line,
// and a final line without a terminator is still yielded. Every physical line
// is returned, blank ones included, so callers can number them like
// enumerate(fh, start=1).
func splitLinesUniversal(text string) []string {
	var lines []string
	start := 0
	for i := 0; i < len(text); i++ {
		switch text[i] {
		case '\n':
			lines = append(lines, text[start:i])
			start = i + 1
		case '\r':
			lines = append(lines, text[start:i])
			if i+1 < len(text) && text[i+1] == '\n' {
				i++
			}
			start = i + 1
		default:
		}
	}
	if start < len(text) {
		lines = append(lines, text[start:])
	}
	return lines
}

// parseCaptureRow decodes one batch-record JSONL line.
func parseCaptureRow(line string) (predictor.ShotFeatures, int, float64, error) {
	var row map[string]any
	if err := json.Unmarshal([]byte(line), &row); err != nil {
		return predictor.ShotFeatures{}, 0, 0, fmt.Errorf("row is not a JSON object: %w", err)
	}
	features, err := sidecarFeaturesFromMapping(row)
	if err != nil {
		return predictor.ShotFeatures{}, 0, 0, err
	}
	crf, err := intField(row, "crf")
	if err != nil {
		return predictor.ShotFeatures{}, 0, 0, err
	}
	observed, ok := numericField(row, "observed_vmaf")
	if !ok {
		return predictor.ShotFeatures{}, 0, 0, errors.New("missing required key: observed_vmaf")
	}
	return features, crf, observed, nil
}

// ---------------------------------------------------------------------------
// Feature-JSON parsing.
// ---------------------------------------------------------------------------

// loadSidecarFeatures reads a --features-json file into ShotFeatures. Every
// failure is a usage error: the Python CLI reports it and returns 2.
func loadSidecarFeatures(path string) (predictor.ShotFeatures, error) {
	row, err := readJSONObject(path)
	if err != nil {
		return predictor.ShotFeatures{}, asUsageError(err)
	}
	features, err := sidecarFeaturesFromMapping(row)
	if err != nil {
		return predictor.ShotFeatures{}, asUsageError(err)
	}
	return features, nil
}

// readJSONObject reads a JSON object from path.
func readJSONObject(path string) (map[string]any, error) {
	data, err := os.ReadFile(path) // #nosec G304 -- operator-supplied CLI flag
	if err != nil {
		return nil, fmt.Errorf("cannot read %s: %w", path, err)
	}
	var doc any
	if err := json.Unmarshal(data, &doc); err != nil {
		return nil, fmt.Errorf("%s is not valid JSON: %w", path, err)
	}
	obj, ok := doc.(map[string]any)
	if !ok {
		return nil, fmt.Errorf("%s must contain a JSON object", path)
	}
	return obj, nil
}

// sidecarFeaturesFromMapping builds ShotFeatures from a JSON object, honouring
// an optional {"features": {...}} wrapper so a capture row can carry crf and
// observed_vmaf alongside the feature block.
func sidecarFeaturesFromMapping(row map[string]any) (predictor.ShotFeatures, error) {
	raw := row
	if wrapped, ok := row["features"]; ok {
		obj, ok := wrapped.(map[string]any)
		if !ok {
			return predictor.ShotFeatures{}, errors.New("'features' must be a JSON object")
		}
		raw = obj
	}
	var missing []string
	for _, key := range sidecarRequiredFeatureKeys {
		if _, ok := raw[key]; !ok {
			missing = append(missing, key)
		}
	}
	if len(missing) > 0 {
		return predictor.ShotFeatures{}, fmt.Errorf(
			"features missing required keys: %s", strings.Join(missing, ", "))
	}
	for _, key := range sidecarRequiredFeatureKeys {
		if _, ok := numericField(raw, key); !ok {
			return predictor.ShotFeatures{}, fmt.Errorf(
				"invalid sidecar feature value: %s is not numeric", key)
		}
	}
	return predictor.ShotFeatures{
		ProbeBitrateKbps:    floatField(raw, "probe_bitrate_kbps"),
		ProbeIFrameAvgBytes: floatField(raw, "probe_i_frame_avg_bytes"),
		ProbePFrameAvgBytes: floatField(raw, "probe_p_frame_avg_bytes"),
		ProbeBFrameAvgBytes: floatField(raw, "probe_b_frame_avg_bytes"),
		SaliencyMean:        floatField(raw, "saliency_mean"),
		SaliencyVar:         floatField(raw, "saliency_var"),
		FrameDiffMean:       floatField(raw, "frame_diff_mean"),
		YAvg:                floatField(raw, "y_avg"),
		YVar:                floatField(raw, "y_var"),
		ShotLengthFrames:    int(floatField(raw, "shot_length_frames")),
		FPS:                 floatField(raw, "fps"),
		Width:               int(floatField(raw, "width")),
		Height:              int(floatField(raw, "height")),
	}, nil
}

// numericField reports whether key holds a JSON number (or a numeric string,
// which CPython's float() would also accept).
func numericField(row map[string]any, key string) (float64, bool) {
	switch v := row[key].(type) {
	case float64:
		return v, true
	case json.Number:
		f, err := v.Float64()
		return f, err == nil
	case string:
		var f float64
		if _, err := fmt.Sscanf(strings.TrimSpace(v), "%g", &f); err == nil {
			return f, true
		}
		return 0, false
	default:
		return 0, false
	}
}

// intField mirrors CPython's int(row[key]) on a decoded JSON value: a JSON
// number truncates toward zero (int(28.7) == 28), a string must spell an
// integer literal (int("28.5") raises), and a missing key is an error.
func intField(row map[string]any, key string) (int, error) {
	value, ok := row[key]
	if !ok {
		return 0, fmt.Errorf("missing required key: %s", key)
	}
	switch v := value.(type) {
	case float64:
		return int(v), nil
	case json.Number:
		f, err := v.Float64()
		if err != nil {
			return 0, fmt.Errorf("invalid %s: %v", key, err)
		}
		return int(f), nil
	case string:
		n, err := strconv.Atoi(strings.TrimSpace(v))
		if err != nil {
			return 0, fmt.Errorf("invalid %s: %q is not an integer", key, v)
		}
		return n, nil
	default:
		return 0, fmt.Errorf("invalid %s: not numeric", key)
	}
}

// floatField returns the numeric value at key, or 0 when absent — the same
// default every optional ShotFeatures field carries.
func floatField(row map[string]any, key string) float64 {
	f, _ := numericField(row, key)
	return f
}
