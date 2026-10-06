// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-server/score_options.go — ScoreRequest.options to a vmaf run, and
// the provenance every scoring response carries (#2155, RC4 WP8).
//
// The options message and every flag come from the option groups of
// core/api/vmafx.toml (proto/vmafx_api.proto, pkg/scoreopts): this file names
// no scoring flag. The server returns lossless scores (precision "max", the
// proto surface's default) unless the request asks otherwise, so a score it
// returns is the score the CLI and the C API compute, bit for bit.

//go:build cgo

package main

import (
	"context"
	"errors"
	"fmt"
	"maps"
	"net/http"
	"strconv"

	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/encoding/protojson"

	vmafxv1 "github.com/VMAFx/vmafx/gen/go"
	"github.com/VMAFx/vmafx/pkg/libvmaf"
	"github.com/VMAFx/vmafx/pkg/scoreopts"
)

// errInvalidOptions marks an options error the caller can fix (InvalidArgument).
var errInvalidOptions = errors.New("invalid scoring options")

// scoreRun builds the vmaf run of a request: its inputs and model, the
// model-spec suffixes and the flags of every option it sets. The second
// result is the precision the scores are computed with.
func scoreRun(req *vmafxv1.ScoreRequest) (libvmaf.Request, string, error) {
	doc, err := scoreopts.Load()
	if err != nil {
		return libvmaf.Request{}, "", err
	}
	values := scoreopts.Values{}
	if options := req.GetOptions(); options != nil {
		values, err = doc.FromMessage(options.ProtoReflect(), "proto")
		if err != nil {
			return libvmaf.Request{}, "", fmt.Errorf("%w: %w", errInvalidOptions, err)
		}
	}
	args, precision, err := optionArgs(doc, values)
	if err != nil {
		return libvmaf.Request{}, "", err
	}
	return libvmaf.Request{
		Reference:   req.GetReference(),
		Distorted:   req.GetDistorted(),
		Model:       req.GetModel(),
		ModelSuffix: doc.ModelSpec("", values),
		Args:        args,
	}, precision, nil
}

// optionArgs is the flags of the set options: the "core" value options in
// definition order (raw-input geometry, subsample), the precision (the proto
// default when unset), the backend and its device, then the "extra" options.
func optionArgs(doc *scoreopts.Document, values scoreopts.Values) ([]string, string, error) {
	var args []string
	for i := range doc.Argv {
		entry := &doc.Argv[i]
		value, set := values[entry.Option]
		if set && entry.Stage == "core" && entry.Form == "value" && entry.Option != "precision" {
			args = append(args, entry.Args(value)...)
		}
	}
	precision, err := precisionOf(doc, values)
	if err != nil {
		return nil, "", err
	}
	precisionFlag, err := doc.Flag("precision")
	if err != nil {
		return nil, "", err
	}
	args = append(args, precisionFlag, precision)
	backendArgs, err := backendDeviceArgs(doc, values)
	if err != nil {
		return nil, "", err
	}
	args = append(args, backendArgs...)
	return append(args, doc.ExtraArgs(values)...), precision, nil
}

// precisionOf is the request's precision, else the proto surface's default.
func precisionOf(doc *scoreopts.Document, values scoreopts.Values) (string, error) {
	if precision, ok := values["precision"].(string); ok {
		return precision, nil
	}
	option, ok := doc.Option("precision")
	if !ok {
		return "", errors.New("scoreopts: no precision option")
	}
	precision, ok := option.DefaultOn("proto").(string)
	if !ok {
		return "", errors.New("scoreopts: the precision option has no proto default")
	}
	return precision, nil
}

// backendDeviceArgs is `--backend <name>` for a requested backend and the
// backend's device flag for a requested device index. A device needs a backend
// that selects devices by index; anything else is refused, never ignored.
func backendDeviceArgs(doc *scoreopts.Document, values scoreopts.Values) ([]string, error) {
	backend, _ := values["backend"].(string)
	device, _ := values["device"].(string)
	var args []string
	if backend != "" {
		flag, err := doc.Flag("backend")
		if err != nil {
			return nil, err
		}
		args = append(args, flag, backend)
	}
	if device == "" || device == "auto" {
		return args, nil
	}
	if _, err := strconv.ParseUint(device, 10, 32); err != nil {
		return nil, fmt.Errorf("%w: device %q is neither auto nor a device index", errInvalidOptions, device)
	}
	// A backend selects devices by index iff the CLI has a `<backend>_device`
	// option (sycl, hip, metal); the definition says which.
	flag, err := doc.Flag(backend + "_device")
	if backend == "" || err != nil {
		return nil, fmt.Errorf("%w: device %s needs a backend that selects devices by index (backend %q)",
			errInvalidOptions, device, backend)
	}
	return append(args, flag, device), nil
}

// scoreProvenance is the provenance of a run: the library record the report
// carries, the model the server loaded and the backend receipt.
func scoreProvenance(result *libvmaf.Result, precision string) (*vmafxv1.ScoreProvenance, error) {
	if len(result.Provenance) == 0 {
		return nil, errors.New("the vmaf report carries no provenance record " +
			"(a vmaf binary older than the scoring API contract)")
	}
	library := &vmafxv1.Provenance{}
	if err := protojson.Unmarshal(result.Provenance, library); err != nil {
		return nil, fmt.Errorf("the report's provenance record does not match proto Provenance: %w", err)
	}
	backends := make([]*vmafxv1.FeatureBackend, 0, len(result.FeatureBackends))
	for _, fb := range result.FeatureBackends {
		backends = append(backends, &vmafxv1.FeatureBackend{Extractor: fb.Extractor, Backend: fb.Backend})
	}
	return &vmafxv1.ScoreProvenance{
		Library:         library,
		Model:           result.ModelName,
		ModelSha256:     result.ModelSHA256,
		BackendUsed:     result.BackendUsed,
		FeatureBackends: backends,
		Precision:       precision,
	}, nil
}

// runScore is the one scoring path of gRPC Score, POST /v1/score and the REST
// adapter: the request's options become vmaf flags, the run's report becomes
// the response with its provenance. An options error is InvalidArgument, a run
// error Internal.
//
// The caller's context is passed on so a client disconnect or deadline tears
// down the vmaf subprocess via exec.CommandContext (fixes
// T-LIBVMAF-SCORE-NEEDS-CTX-2026-05-31).
func runScore(ctx context.Context, scorer *libvmaf.Scorer, req *vmafxv1.ScoreRequest) (*vmafxv1.ScoreResponse, error) {
	run, precision, err := scoreRun(req)
	if err == nil && scorer == nil {
		err = errors.New("scorer not initialised")
	}
	if errors.Is(err, errInvalidOptions) {
		return nil, status.Error(codes.InvalidArgument, err.Error())
	}
	if err != nil {
		return nil, status.Errorf(codes.Internal, "scoring options: %v", err)
	}
	result, err := scorer.Run(ctx, run)
	if err != nil {
		return nil, status.Errorf(codes.Internal, "scoring failed: %v", err)
	}
	provenance, err := scoreProvenance(result, precision)
	if err != nil {
		return nil, status.Errorf(codes.Internal, "scoring provenance: %v", err)
	}
	features := make(map[string]float64, len(result.Features))
	maps.Copy(features, result.Features)
	return &vmafxv1.ScoreResponse{Score: result.Score, Features: features, Provenance: provenance}, nil
}

// httpStatusOf maps a runScore error onto an HTTP status.
func httpStatusOf(err error) int {
	switch status.Code(err) {
	case codes.InvalidArgument:
		return http.StatusBadRequest
	case codes.ResourceExhausted:
		return http.StatusTooManyRequests
	default:
		return http.StatusInternalServerError
	}
}

// responseJSONOptions writes proto field names, as the OpenAPI contract
// spells them (score, features, provenance.library.abi_major, ...).
var responseJSONOptions = protojson.MarshalOptions{UseProtoNames: true}

// requestJSONOptions reads a JSON request body as a ScoreRequest; a field the
// contract does not know is refused, never ignored.
var requestJSONOptions = protojson.UnmarshalOptions{DiscardUnknown: false}

// streamProvenance is the provenance of a finished ScoreStream: the record of
// the in-process context, the model file's SHA-256, the context's backend.
// In-process scores are doubles, so the precision is lossless ("max").
func streamProvenance(scorer *libvmaf.StreamScorer, model string) (*vmafxv1.ScoreProvenance, error) {
	record, digest, err := scorer.Provenance()
	if err != nil {
		return nil, err
	}
	provenance, err := scoreProvenance(&libvmaf.Result{
		Provenance: record, ModelName: model, ModelSHA256: digest,
	}, "max")
	if err != nil {
		return nil, err
	}
	provenance.BackendUsed = provenance.GetLibrary().GetActiveBackend()
	return provenance, nil
}
