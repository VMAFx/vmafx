// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

//go:build cgo

package main

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"reflect"
	"strings"
	"testing"
	"time"

	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/proto"

	vmafxv1 "github.com/VMAFx/vmafx/gen/go"
	"github.com/VMAFx/vmafx/pkg/libvmaf"
)

// TestScoreRunMapsOptionsToGeneratedFlags: every option a request sets
// becomes the vmaf flag the definition names, in a fixed order, with the
// lossless precision of the proto surface when the request names none.
func TestScoreRunMapsOptionsToGeneratedFlags(t *testing.T) {
	t.Parallel()
	req := &vmafxv1.ScoreRequest{
		Reference: "/r.yuv", Distorted: "/d.yuv", Model: "vmaf_v0.6.1",
		Options: &vmafxv1.ScoreOptions{
			Width: proto.Uint32(576), Height: proto.Uint32(324),
			PixelFormat: proto.String("420"), Bitdepth: proto.Uint32(8),
			Subsample: proto.Uint32(2), Threads: proto.Uint32(4),
			Feature: []string{"psnr"}, DisableClip: proto.Bool(true),
			ViewDistance: proto.Float64(4.5), Backend: proto.String("sycl"), Device: proto.String("1"),
		},
	}
	run, precision, err := scoreRun(req)
	if err != nil {
		t.Fatalf("scoreRun: %v", err)
	}
	wantArgs := []string{
		"--width", "576", "--height", "324", "-p", "420", "-b", "8", "--subsample", "2",
		"--precision", "max", "--backend", "sycl", "--sycl_device", "1",
		"--feature", "psnr", "--threads", "4",
	}
	if !reflect.DeepEqual(run.Args, wantArgs) {
		t.Errorf("Args = %q\nwant  %q", run.Args, wantArgs)
	}
	if run.ModelSuffix != ":disable_clip:adm.adm_norm_view_dist=4.5" || precision != "max" {
		t.Errorf("ModelSuffix = %q, precision = %q", run.ModelSuffix, precision)
	}
	if run.Reference != "/r.yuv" || run.Distorted != "/d.yuv" || run.Model != "vmaf_v0.6.1" {
		t.Errorf("inputs not passed: %+v", run)
	}
}

// TestScoreRunRefusesOptionsTheDefinitionRefuses: a bad value, a reserved
// option off its default and a device without an indexed backend are
// InvalidArgument; none is dropped silently.
func TestScoreRunRefusesOptionsTheDefinitionRefuses(t *testing.T) {
	t.Parallel()
	cases := map[string]*vmafxv1.ScoreOptions{
		"invalid bitdepth 14":     {Bitdepth: proto.Uint32(14)},
		"invalid backend vulkan":  {Backend: proto.String("vulkan")},
		"invalid target_width":    {TargetWidth: proto.Uint32(1920)},
		"needs a backend that":    {Backend: proto.String("cuda"), Device: proto.String("0")},
		"neither auto nor":        {Backend: proto.String("hip"), Device: proto.String("first")},
		"invalid precision":       {Precision: proto.String("18")},
		"invalid view_distance 0": {ViewDistance: proto.Float64(0)},
	}
	for want, options := range cases {
		_, err := runScore(context.Background(), nil, &vmafxv1.ScoreRequest{
			Reference: "/r", Distorted: "/d", Options: options,
		})
		if status.Code(err) != codes.InvalidArgument || !strings.Contains(err.Error(), want) {
			t.Errorf("options %v: got %v, want InvalidArgument containing %q", options, err, want)
		}
	}
}

// TestScoreProvenanceNeedsTheReportRecord is the planted defect of the
// provenance step: a report without a record, or with a field the proto
// Provenance message does not know, fails the request.
func TestScoreProvenanceNeedsTheReportRecord(t *testing.T) {
	t.Parallel()
	if _, err := scoreProvenance(&libvmaf.Result{}, "max"); err == nil {
		t.Error("a report without provenance was accepted")
	}
	drifted := &libvmaf.Result{Provenance: json.RawMessage(`{"abi_major": 0, "abi_mayor": 1}`)}
	if _, err := scoreProvenance(drifted, "max"); err == nil {
		t.Error("a provenance record with an unknown field was accepted")
	}
	good := &libvmaf.Result{
		Provenance:      json.RawMessage(`{"abi_minor": 1, "active_backend": "cuda", "version": "v"}`),
		ModelName:       "m",
		ModelSHA256:     "ab",
		BackendUsed:     "cuda",
		FeatureBackends: []libvmaf.FeatureBackend{{Extractor: "adm", Backend: "cuda"}},
	}
	p, err := scoreProvenance(good, "legacy")
	if err != nil || p.GetLibrary().GetAbiMinor() != 1 || p.GetLibrary().GetActiveBackend() != "cuda" ||
		p.GetFeatureBackends()[0].GetExtractor() != "adm" || p.GetPrecision() != "legacy" {
		t.Errorf("scoreProvenance = %v, %v", p, err)
	}
}

// TestScoreResponseCarriesProvenance: the gRPC response names the library
// record of the report, the model it loaded with the file's SHA-256 and the
// precision.
func TestScoreResponseCarriesProvenance(t *testing.T) {
	t.Parallel()
	client, stop := startGRPCTestServer(t)
	defer stop()
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	resp, err := client.Score(ctx, &vmafxv1.ScoreRequest{Reference: "/r", Distorted: "/d", Model: "vmaf_v0.6.1"})
	if err != nil {
		t.Fatalf("Score: %v", err)
	}
	digest := sha256.Sum256([]byte(`{}`)) // writeModelFile's model
	p := resp.GetProvenance()
	if p.GetLibrary().GetAbiPatch() != 4 || p.GetLibrary().GetVersion() != "v1.0.0-rc.4-test" ||
		p.GetModel() != "vmaf_v0.6.1" || p.GetModelSha256() != hex.EncodeToString(digest[:]) ||
		p.GetBackendUsed() != "cpu" || p.GetPrecision() != "max" {
		t.Errorf("provenance = %v", p)
	}
}

// TestHTTPScoreReadsTheContractAndAnswersWithProvenance: POST /v1/score takes
// the contract's ScoreRequest (options included), refuses a field the
// contract does not know, and answers with the provenance.
func TestHTTPScoreReadsTheContractAndAnswersWithProvenance(t *testing.T) {
	t.Parallel()
	hs, _ := newTestHTTPServer(t)
	mux := http.NewServeMux()
	hs.routes(mux)
	ts := httptest.NewServer(mux)
	defer ts.Close()

	post := func(body string) (int, string) {
		resp, err := ts.Client().Post(ts.URL+"/v1/score", "application/json", strings.NewReader(body))
		if err != nil {
			t.Fatalf("POST /v1/score: %v", err)
		}
		defer resp.Body.Close()
		raw, _ := io.ReadAll(resp.Body)
		return resp.StatusCode, string(raw)
	}
	code, body := post(`{"reference": "/r", "distorted": "/d", "model": "vmaf_v0.6.1", "options": {"width": 576, "threads": 2}}`)
	if code != http.StatusOK || !strings.Contains(body, `"provenance"`) || !strings.Contains(body, `"abi_patch":4`) {
		t.Errorf("options request: %d %s", code, body)
	}
	if code, body = post(`{"reference": "/r", "distorted": "/d", "optionz": {}}`); code != http.StatusBadRequest {
		t.Errorf("unknown field: %d %s", code, body)
	}
	if code, body = post(`{"reference": "/r", "distorted": "/d", "model": "vmaf_v0.6.1", "options": {"bitdepth": 14}}`); code != http.StatusBadRequest {
		t.Errorf("bad option: %d %s", code, body)
	}
}
