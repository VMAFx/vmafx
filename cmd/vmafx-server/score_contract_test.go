// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

//go:build cgo

// The scoring contract test (#2155): the same request through the vmaf CLI,
// the C API and the scoring server (gRPC and POST /v1/score) gives the same
// score bit for bit, and every surface reports the same library build.
//
// It needs a build: VMAF_BIN (the vmaf CLI), VMAFX_CONTRACT_CAPI (the
// vmafx_score_contract program of core/test), VMAFX_CONTRACT_YUV (the
// directory of the Netflix 576x324 pair) and VMAFX_CONTRACT_MODEL_DIR (the
// repository's model/ directory). Meson's test_vmafx_score_contract sets all
// four and runs it (suite `contract`); without them it is skipped with the
// reason. The CLI leg spells its flags here, independently of the server's
// generated mapping, so a mapping that drops or alters an option fails.

package main

import (
	"context"
	"encoding/json"
	"io"
	"math"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/prometheus/client_golang/prometheus"
	"google.golang.org/protobuf/proto"

	vmafxv1 "github.com/VMAFx/vmafx/gen/go"
	"github.com/VMAFx/vmafx/pkg/libvmaf"
	"github.com/VMAFx/vmafx/pkg/observability"
)

// contractCase is one request in every surface's own spelling.
type contractCase struct {
	name               string
	model              string                // ScoreRequest.model; "" = library default
	options            *vmafxv1.ScoreOptions // ScoreRequest.options beyond the geometry
	cliArgs            []string              // the same options as vmaf flags
	threads, subsample uint32                // the same options for the C API program
}

const (
	contractRef    = "src01_hrc00_576x324.yuv"
	contractDis    = "src01_hrc01_576x324.yuv"
	contractWidth  = 576
	contractHeight = 324
)

var contractCases = []contractCase{
	{name: "library default model"},
	{
		name: "vmaf_v0.6.1 with 4 threads", model: "vmaf_v0.6.1",
		options: &vmafxv1.ScoreOptions{Threads: proto.Uint32(4)},
		cliArgs: []string{"-m", "version=vmaf_v0.6.1", "--threads", "4"}, threads: 4,
	},
	{
		name: "vmaf_v0.6.1 every 3rd frame", model: "vmaf_v0.6.1",
		options: &vmafxv1.ScoreOptions{Subsample: proto.Uint32(3)},
		cliArgs: []string{"-m", "version=vmaf_v0.6.1", "--subsample", "3"}, subsample: 3,
	},
}

// contractEnv is the build under test, or a skip reason.
type contractEnv struct {
	cli, capi, yuv, models string
}

func loadContractEnv(t *testing.T) contractEnv {
	t.Helper()
	env := contractEnv{
		cli:    os.Getenv("VMAF_BIN"),
		capi:   os.Getenv("VMAFX_CONTRACT_CAPI"),
		yuv:    os.Getenv("VMAFX_CONTRACT_YUV"),
		models: os.Getenv("VMAFX_CONTRACT_MODEL_DIR"),
	}
	if env.cli == "" || env.capi == "" || env.yuv == "" || env.models == "" {
		t.Skip("scoring contract needs VMAF_BIN, VMAFX_CONTRACT_CAPI, VMAFX_CONTRACT_YUV and " +
			"VMAFX_CONTRACT_MODEL_DIR (meson test test_vmafx_score_contract sets them)")
	}
	return env
}

// surfaceResult is what one surface reports for a case.
type surfaceResult struct {
	score   float64
	version string
	abi     [3]uint32
}

func TestScoreContract(t *testing.T) {
	env := loadContractEnv(t)
	server, rest := contractServers(t, env)
	for _, tc := range contractCases {
		t.Run(tc.name, func(t *testing.T) {
			results := map[string]surfaceResult{
				"cli":  runContractCLI(t, env, tc),
				"capi": runContractCAPI(t, env, tc),
				"grpc": runContractGRPC(t, server, env, tc),
				"rest": runContractREST(t, rest, env, tc),
			}
			want := results["cli"]
			for surface, got := range results {
				if math.Float64bits(got.score) != math.Float64bits(want.score) {
					t.Errorf("%s score %v (%x) != cli score %v (%x)", surface, got.score,
						math.Float64bits(got.score), want.score, math.Float64bits(want.score))
				}
				if got.version != want.version || got.abi != want.abi {
					t.Errorf("%s provenance %s %v != cli provenance %s %v", surface,
						got.version, got.abi, want.version, want.abi)
				}
			}
			t.Logf("%s: VMAF %.17g on every surface (%s, ABI %v)", tc.name, want.score, want.version, want.abi)
		})
	}
}

func contractServers(t *testing.T, env contractEnv) (*grpcServer, *httptest.Server) {
	t.Helper()
	scorer, err := libvmaf.New(env.cli, env.models)
	if err != nil {
		t.Fatalf("libvmaf.New: %v", err)
	}
	reg := prometheus.NewRegistry()
	metrics := observability.NewMetrics(reg)
	log := observability.NewLogger("ERROR")
	grpcImpl := newGRPCServer(scorer, metrics, log)
	mux := http.NewServeMux()
	newHTTPServer(scorer, metrics, reg, log, grpcImpl).routes(mux)
	rest := httptest.NewServer(mux)
	t.Cleanup(rest.Close)
	return grpcImpl, rest
}

// contractRequest is the case as a ScoreRequest: the raw-input geometry plus
// the case's options.
func contractRequest(env contractEnv, tc contractCase) *vmafxv1.ScoreRequest {
	options := &vmafxv1.ScoreOptions{}
	if tc.options != nil {
		options = proto.CloneOf(tc.options)
	}
	options.Width, options.Height = proto.Uint32(contractWidth), proto.Uint32(contractHeight)
	options.PixelFormat, options.Bitdepth = proto.String("420"), proto.Uint32(8)
	return &vmafxv1.ScoreRequest{
		Reference: filepath.Join(env.yuv, contractRef),
		Distorted: filepath.Join(env.yuv, contractDis),
		Model:     tc.model,
		Options:   options,
	}
}

func runContractCLI(t *testing.T, env contractEnv, tc contractCase) surfaceResult {
	t.Helper()
	out := filepath.Join(t.TempDir(), "cli.json")
	args := []string{
		"-r", filepath.Join(env.yuv, contractRef), "-d", filepath.Join(env.yuv, contractDis),
		"-w", strconv.Itoa(contractWidth), "-h", strconv.Itoa(contractHeight), "-p", "420", "-b", "8",
		"--precision", "max", "-q", "--json", "-o", out,
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Minute)
	defer cancel()
	if raw, err := exec.CommandContext(ctx, env.cli, append(args, tc.cliArgs...)...).CombinedOutput(); err != nil {
		t.Fatalf("vmaf: %v\n%s", err, raw)
	}
	data, err := os.ReadFile(out) //nolint:gosec // the test's own temp file
	if err != nil {
		t.Fatalf("read the CLI report: %v", err)
	}
	var report struct {
		Pooled     map[string]struct{ Mean float64 } `json:"pooled_metrics"`
		Provenance contractProvenance                `json:"provenance"`
	}
	if err := json.Unmarshal(data, &report); err != nil {
		t.Fatalf("parse the CLI report: %v", err)
	}
	return surfaceResult{report.Pooled["vmaf"].Mean, report.Provenance.Version, report.Provenance.abi()}
}

// contractProvenance is the provenance record as the CLI and the C API program
// print it.
type contractProvenance struct {
	ABIMajor uint32 `json:"abi_major"`
	ABIMinor uint32 `json:"abi_minor"`
	ABIPatch uint32 `json:"abi_patch"`
	Version  string `json:"version"`
}

func (p contractProvenance) abi() [3]uint32 { return [3]uint32{p.ABIMajor, p.ABIMinor, p.ABIPatch} }

func runContractCAPI(t *testing.T, env contractEnv, tc contractCase) surfaceResult {
	t.Helper()
	model := tc.model
	if model == "" {
		model = "-"
	}
	args := []string{
		filepath.Join(env.yuv, contractRef), filepath.Join(env.yuv, contractDis),
		strconv.Itoa(contractWidth), strconv.Itoa(contractHeight), "8", model,
		strconv.FormatUint(uint64(tc.threads), 10), strconv.FormatUint(uint64(tc.subsample), 10),
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Minute)
	defer cancel()
	raw, err := exec.CommandContext(ctx, env.capi, args...).Output()
	if err != nil {
		t.Fatalf("vmafx_score_contract: %v", err)
	}
	var out struct {
		VMAF       float64            `json:"vmaf"`
		VMAFHex    string             `json:"vmaf_hex"`
		Provenance contractProvenance `json:"provenance"`
	}
	if err := json.Unmarshal(raw, &out); err != nil {
		t.Fatalf("parse %s: %v", raw, err)
	}
	exact, err := strconv.ParseFloat(out.VMAFHex, 64)
	if err != nil || math.Float64bits(exact) != math.Float64bits(out.VMAF) {
		t.Fatalf("C API %%a %q and %%.17g %v disagree (%v)", out.VMAFHex, out.VMAF, err)
	}
	return surfaceResult{exact, out.Provenance.Version, out.Provenance.abi()}
}

func runContractGRPC(t *testing.T, server *grpcServer, env contractEnv, tc contractCase) surfaceResult {
	t.Helper()
	resp, err := server.Score(context.Background(), contractRequest(env, tc))
	if err != nil {
		t.Fatalf("gRPC Score: %v", err)
	}
	return responseResult(t, resp)
}

func runContractREST(t *testing.T, rest *httptest.Server, env contractEnv, tc contractCase) surfaceResult {
	t.Helper()
	body, err := requestJSONMarshal(contractRequest(env, tc))
	if err != nil {
		t.Fatalf("encode request: %v", err)
	}
	resp, err := rest.Client().Post(rest.URL+"/v1/score", "application/json", strings.NewReader(body))
	if err != nil {
		t.Fatalf("POST /v1/score: %v", err)
	}
	defer resp.Body.Close()
	raw, _ := io.ReadAll(resp.Body)
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("POST /v1/score = %d: %s", resp.StatusCode, raw)
	}
	decoded := &vmafxv1.ScoreResponse{}
	if err := requestJSONOptions.Unmarshal(raw, decoded); err != nil {
		t.Fatalf("decode %s: %v", raw, err)
	}
	return responseResult(t, decoded)
}

func responseResult(t *testing.T, resp *vmafxv1.ScoreResponse) surfaceResult {
	t.Helper()
	lib := resp.GetProvenance().GetLibrary()
	if resp.GetProvenance().GetPrecision() != "max" {
		t.Errorf("server precision %q, want max", resp.GetProvenance().GetPrecision())
	}
	return surfaceResult{
		resp.GetScore(), lib.GetVersion(), [3]uint32{lib.GetAbiMajor(), lib.GetAbiMinor(), lib.GetAbiPatch()},
	}
}

// requestJSONMarshal writes a request as the REST contract spells it.
func requestJSONMarshal(req *vmafxv1.ScoreRequest) (string, error) {
	raw, err := responseJSONOptions.Marshal(req)
	return string(raw), err
}
