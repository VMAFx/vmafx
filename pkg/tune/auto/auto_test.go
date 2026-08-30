// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent

package auto

import (
	"context"
	"encoding/json"
	"math"
	"os"
	"path/filepath"
	"reflect"
	"testing"
)

// ---------------------------------------------------------------------------
// Short-circuit predicates.
// ---------------------------------------------------------------------------

// TestShortCircuitPredicates pins every predicate's firing condition, both
// sides of each gate. The predicates are the contract the driver's stage order
// is built on, so a silent flip here would change plans without changing the
// schema.
func TestShortCircuitPredicates(t *testing.T) {
	t.Parallel()

	truePtr, falsePtr := true, false

	tests := []struct {
		name  string
		fn    func(SourceMeta, *PlanState) bool
		meta  SourceMeta
		state PlanState
		want  bool
	}{
		{
			name: "single-rung fires below 2160",
			fn:   ShouldShortCircuitSingleRungLadder,
			meta: SourceMeta{Height: 1080},
			want: true,
		},
		{
			name: "single-rung dormant at exactly 2160",
			fn:   ShouldShortCircuitSingleRungLadder,
			meta: SourceMeta{Height: 2160},
			want: false,
		},
		{
			name:  "codec-pinned fires on an explicit pin",
			fn:    ShouldShortCircuitCodecPinned,
			state: PlanState{UserPinnedCodec: "libx265", AllowCodecs: []string{"a", "b"}},
			want:  true,
		},
		{
			name:  "codec-pinned fires on a single-entry allow list",
			fn:    ShouldShortCircuitCodecPinned,
			state: PlanState{AllowCodecs: []string{"libx264"}},
			want:  true,
		},
		{
			name:  "codec-pinned dormant on a two-codec allow list",
			fn:    ShouldShortCircuitCodecPinned,
			state: PlanState{AllowCodecs: []string{"libx264", "libx265"}},
			want:  false,
		},
		{
			name:  "predictor-gospel fires on GOSPEL",
			fn:    ShouldShortCircuitPredictorGospel,
			state: PlanState{PredictorVerdict: VerdictGospel},
			want:  true,
		},
		{
			name:  "predictor-gospel dormant on LIKELY",
			fn:    ShouldShortCircuitPredictorGospel,
			state: PlanState{PredictorVerdict: VerdictLikely},
			want:  false,
		},
		{
			name: "skip-saliency fires on live_action",
			fn:   ShouldShortCircuitSkipSaliency,
			meta: SourceMeta{ContentClass: "live_action"},
			want: true,
		},
		{
			name: "skip-saliency dormant on animation",
			fn:   ShouldShortCircuitSkipSaliency,
			meta: SourceMeta{ContentClass: "animation"},
			want: false,
		},
		{
			name: "skip-saliency dormant on screen_content",
			fn:   ShouldShortCircuitSkipSaliency,
			meta: SourceMeta{ContentClass: "screen_content"},
			want: false,
		},
		{
			name: "sdr-skip fires on SDR",
			fn:   ShouldShortCircuitSDRSkip,
			meta: SourceMeta{IsHDR: false},
			want: true,
		},
		{
			name: "sdr-skip dormant on HDR",
			fn:   ShouldShortCircuitSDRSkip,
			meta: SourceMeta{IsHDR: true},
			want: false,
		},
		{
			name: "sample-clip-propagate fires on a positive clip",
			fn:   ShouldShortCircuitSampleClipPropagate,
			meta: SourceMeta{SampleClipSeconds: 8.0},
			want: true,
		},
		{
			name: "sample-clip-propagate dormant at zero",
			fn:   ShouldShortCircuitSampleClipPropagate,
			meta: SourceMeta{SampleClipSeconds: 0.0},
			want: false,
		},
		{
			name: "skip-per-shot needs both short AND low-variance",
			fn:   ShouldShortCircuitSkipPerShot,
			meta: SourceMeta{DurationS: 100.0, ShotVariance: 0.05},
			want: true,
		},
		{
			name: "skip-per-shot dormant for a short high-variance trailer",
			fn:   ShouldShortCircuitSkipPerShot,
			meta: SourceMeta{DurationS: 100.0, ShotVariance: 0.5},
			want: false,
		},
		{
			name: "skip-per-shot dormant for a long low-variance lecture",
			fn:   ShouldShortCircuitSkipPerShot,
			meta: SourceMeta{DurationS: 3600.0, ShotVariance: 0.01},
			want: false,
		},
		{
			name: "low-complexity fires below the probe threshold",
			fn:   ShouldShortCircuitLowComplexity,
			meta: SourceMeta{ComplexityScore: 150.0},
			want: true,
		},
		{
			name: "low-complexity dormant when the probe has not run",
			fn:   ShouldShortCircuitLowComplexity,
			meta: SourceMeta{ComplexityScore: 0.0},
			want: false,
		},
		{
			name: "low-complexity dormant on a NaN probe",
			fn:   ShouldShortCircuitLowComplexity,
			meta: SourceMeta{ComplexityScore: math.NaN()},
			want: false,
		},
		{
			name:  "baseline-meets-target fires at exactly the target",
			fn:    ShouldShortCircuitBaselineMeetsTarget,
			meta:  SourceMeta{BaselineVMAF: 93.0},
			state: PlanState{TargetVMAF: 93.0},
			want:  true,
		},
		{
			name:  "baseline-meets-target dormant below the target",
			fn:    ShouldShortCircuitBaselineMeetsTarget,
			meta:  SourceMeta{BaselineVMAF: 80.0},
			state: PlanState{TargetVMAF: 93.0},
			want:  false,
		},
		{
			name:  "baseline-meets-target dormant when no baseline was scored",
			fn:    ShouldShortCircuitBaselineMeetsTarget,
			meta:  SourceMeta{BaselineVMAF: 0.0},
			state: PlanState{TargetVMAF: 93.0},
			want:  false,
		},
		{
			name:  "no-two-pass fires when the adapter says no",
			fn:    ShouldShortCircuitNoTwoPass,
			state: PlanState{AdapterSupportsTwoPass: &falsePtr},
			want:  true,
		},
		{
			name:  "no-two-pass dormant when the adapter says yes",
			fn:    ShouldShortCircuitNoTwoPass,
			state: PlanState{AdapterSupportsTwoPass: &truePtr},
			want:  false,
		},
		{
			name:  "no-two-pass dormant while unresolved",
			fn:    ShouldShortCircuitNoTwoPass,
			state: PlanState{AdapterSupportsTwoPass: nil},
			want:  false,
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			state := tc.state
			if got := tc.fn(tc.meta, &state); got != tc.want {
				t.Errorf("predicate = %v, want %v", got, tc.want)
			}
		})
	}
}

// TestPlanStateFiredIsIdempotent guards the metadata block against duplicate
// short-circuit entries when a predicate fires once per cell.
func TestPlanStateFiredIsIdempotent(t *testing.T) {
	t.Parallel()

	state := &PlanState{}
	state.Fired(SCSDRSkip)
	state.Fired(SCSDRSkip)
	state.Fired(SCCodecPinned)
	state.Fired(SCSDRSkip)

	want := []string{"sdr-skip", "codec-pinned"}
	if !reflect.DeepEqual(state.ShortCircuits, want) {
		t.Errorf("ShortCircuits = %v, want %v", state.ShortCircuits, want)
	}
}

// TestEvaluateShortCircuitsIsDeterministic checks the documented invariant
// that evaluating twice yields the same list in the same order.
func TestEvaluateShortCircuitsIsDeterministic(t *testing.T) {
	t.Parallel()

	meta := SourceMeta{Height: 1080, ContentClass: "live_action", DurationS: 10}
	state := &PlanState{AllowCodecs: []string{"libx264"}, TargetVMAF: 93}

	first := EvaluateShortCircuits(meta, state)
	second := EvaluateShortCircuits(meta, state)
	if !reflect.DeepEqual(first, second) {
		t.Fatalf("evaluation is not idempotent: %v then %v", first, second)
	}
	want := []string{
		"ladder-single-rung", "codec-pinned", "skip-saliency",
		"sdr-skip", "skip-per-shot",
	}
	if !reflect.DeepEqual(first, want) {
		t.Errorf("fired = %v, want %v", first, want)
	}
}

// ---------------------------------------------------------------------------
// F.3 confidence policy.
// ---------------------------------------------------------------------------

func TestConfidenceAwareEscalation(t *testing.T) {
	t.Parallel()

	thresholds := DefaultConfidenceThresholds() // tight 2.0 / wide 5.0

	tests := []struct {
		name    string
		verdict string
		width   float64
		want    ConfidenceDecision
		wantErr bool
	}{
		{"NaN defers to the verdict", VerdictGospel, math.NaN(), RecommendEscalation, false},
		{"NaN with FALL_BACK still defers", VerdictFallBack, math.NaN(), RecommendEscalation, false},
		{"tight overrides FALL_BACK", VerdictFallBack, 1.0, SkipEscalation, false},
		{"tight boundary is inclusive", VerdictFallBack, 2.0, SkipEscalation, false},
		{"wide overrides GOSPEL", VerdictGospel, 9.0, ForceEscalation, false},
		{"wide boundary is inclusive", VerdictGospel, 5.0, ForceEscalation, false},
		{"middle band honours GOSPEL", VerdictGospel, 3.0, SkipEscalation, false},
		{"middle band honours LIKELY", VerdictLikely, 3.0, SkipEscalation, false},
		{"middle band honours FALL_BACK", VerdictFallBack, 3.0, RecommendEscalation, false},
		{"middle band with no verdict recommends", "", 3.0, RecommendEscalation, false},
		{"negative width is a caller bug", VerdictGospel, -1.0, "", true},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got, err := ConfidenceAwareEscalation(tc.verdict, tc.width, thresholds)
			if tc.wantErr {
				if err == nil {
					t.Fatal("expected an error, got nil")
				}
				return
			}
			if err != nil {
				t.Fatalf("unexpected error: %v", err)
			}
			if got != tc.want {
				t.Errorf("decision = %q, want %q", got, tc.want)
			}
		})
	}
}

func TestNewConfidenceThresholdsRejectsInvalidPairs(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name        string
		tight, wide float64
		wantErr     bool
	}{
		{"valid", 1.5, 4.0, false},
		{"equal bounds are valid", 3.0, 3.0, false},
		{"tight above wide", 6.0, 4.0, true},
		{"zero tight", 0.0, 4.0, true},
		{"negative wide", 1.0, -4.0, true},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			_, err := NewConfidenceThresholds(tc.tight, tc.wide, "test")
			if (err != nil) != tc.wantErr {
				t.Errorf("err = %v, wantErr %v", err, tc.wantErr)
			}
		})
	}
}

// TestLoadConfidenceThresholdsFallsBackToDefaults asserts every documented
// failure path degrades to the emergency floor instead of erroring out.
func TestLoadConfidenceThresholdsFallsBackToDefaults(t *testing.T) {
	t.Parallel()

	dir := t.TempDir()
	write := func(name, content string) string {
		path := filepath.Join(dir, name)
		if err := os.WriteFile(path, []byte(content), 0o600); err != nil {
			t.Fatalf("write fixture: %v", err)
		}
		return path
	}

	tests := []struct {
		name        string
		path        string
		wantTight   float64
		wantWide    float64
		wantSourced bool
	}{
		{"no sidecar", "", 2.0, 5.0, false},
		{"missing file", filepath.Join(dir, "nope.json"), 2.0, 5.0, false},
		{"malformed JSON", write("bad.json", "{not json"), 2.0, 5.0, false},
		{"missing keys", write("empty.json", `{"other": 1}`), 2.0, 5.0, false},
		{
			"invalid ordering",
			write("inverted.json", `{"tight_interval_max_width": 9, "wide_interval_min_width": 4}`),
			2.0, 5.0, false,
		},
		{
			"valid sidecar",
			write("good.json", `{"tight_interval_max_width": 1.6, "wide_interval_min_width": 4.2}`),
			1.6, 4.2, true,
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got := LoadConfidenceThresholds(tc.path, nil)
			if got.TightIntervalMaxWidth != tc.wantTight || got.WideIntervalMinWidth != tc.wantWide {
				t.Errorf("thresholds = (%v, %v), want (%v, %v)",
					got.TightIntervalMaxWidth, got.WideIntervalMinWidth, tc.wantTight, tc.wantWide)
			}
			if tc.wantSourced && got.Source == "default" {
				t.Error("a valid sidecar should record its path as the source")
			}
			if !tc.wantSourced && got.Source != "default" {
				t.Errorf("fallback should record source=default, got %q", got.Source)
			}
		})
	}
}

// ---------------------------------------------------------------------------
// F.4 recipes.
// ---------------------------------------------------------------------------

func TestResolveRecipeClass(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name string
		meta SourceMeta
		want string
	}{
		{"unknown SDR class", SourceMeta{ContentClass: "live_action"}, RecipeClassDefault},
		{"empty class", SourceMeta{}, RecipeClassDefault},
		{"animation wins", SourceMeta{ContentClass: "animation"}, RecipeClassAnimation},
		{
			"animation wins over HDR promotion",
			SourceMeta{ContentClass: "animation", IsHDR: true},
			RecipeClassAnimation,
		},
		{
			"screen content wins over HDR promotion",
			SourceMeta{ContentClass: "screen_content", IsHDR: true},
			RecipeClassScreenContent,
		},
		{"ugc wins", SourceMeta{ContentClass: "UGC"}, RecipeClassUGC},
		{
			"HDR promotes a generic live-action label",
			SourceMeta{ContentClass: "live_action", IsHDR: true},
			RecipeClassLiveActionHDR,
		},
		{
			"HDR promotes an unlabelled source",
			SourceMeta{IsHDR: true},
			RecipeClassLiveActionHDR,
		},
		{
			"explicit HDR class",
			SourceMeta{ContentClass: "live_action_hdr"},
			RecipeClassLiveActionHDR,
		},
		{"whitespace and case are normalised", SourceMeta{ContentClass: "  Animation "}, RecipeClassAnimation},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			if got := ResolveRecipeClass(tc.meta); got != tc.want {
				t.Errorf("ResolveRecipeClass = %q, want %q", got, tc.want)
			}
		})
	}
}

// TestRecipeTableForClassReturnsAFreshCopy pins the read-only invariant: a
// caller mutating one run's overrides must not affect the next run.
func TestRecipeTableForClassReturnsAFreshCopy(t *testing.T) {
	t.Parallel()

	table := NewRecipeTable(t.TempDir(), nil) // no calibrated JSON: placeholders
	first := table.ForClass(RecipeClassAnimation)
	if len(first) == 0 {
		t.Fatal("animation recipe should not be empty")
	}
	first["force_single_rung"] = false
	first["injected"] = "should not persist"

	second := table.ForClass(RecipeClassAnimation)
	if v, _ := second["force_single_rung"].(bool); !v {
		t.Error("mutating the returned map leaked into the table")
	}
	if _, ok := second["injected"]; ok {
		t.Error("an injected key leaked into the table")
	}
}

// TestRecipeTableDropsUndocumentedKeys guards plan.metadata.recipe_overrides
// against calibrator provenance blocks and schema drift.
func TestRecipeTableDropsUndocumentedKeys(t *testing.T) {
	t.Parallel()

	dir := t.TempDir()
	aiDir := filepath.Join(dir, "ai", "data")
	if err := os.MkdirAll(aiDir, 0o750); err != nil {
		t.Fatalf("mkdir: %v", err)
	}
	payload := `{
	  "recipes": {
	    "animation": {
	      "_provenance": {"source": "proxy"},
	      "tight_interval_max_width": 1.75,
	      "not_a_recipe_key": 42
	    },
	    "unknown_class": {"tight_interval_max_width": 9.0}
	  }
	}`
	fixture := filepath.Join(aiDir, "phase_f_recipes_calibrated.json")
	if err := os.WriteFile(fixture, []byte(payload), 0o600); err != nil {
		t.Fatalf("write fixture: %v", err)
	}

	table := NewRecipeTable(dir, nil)
	got := table.ForClass(RecipeClassAnimation)
	want := map[string]any{"tight_interval_max_width": 1.75}
	if !reflect.DeepEqual(got, want) {
		t.Errorf("animation recipe = %v, want %v", got, want)
	}
	if len(table.ForClass("unknown_class")) != 0 {
		t.Error("an unknown class should resolve to the empty default recipe")
	}
}

// TestApplyRecipeThresholdsCapsTightAtWide guards the tight <= wide invariant
// the ConfidenceThresholds constructor enforces.
func TestApplyRecipeThresholdsCapsTightAtWide(t *testing.T) {
	t.Parallel()

	base := DefaultConfidenceThresholds() // 2.0 / 5.0

	tests := []struct {
		name      string
		recipe    map[string]any
		wantTight float64
		wantSrc   string
	}{
		{"no override keeps the base", map[string]any{}, 2.0, "default"},
		{
			"a tighter gate is honoured",
			map[string]any{RecipeKeyTightIntervalMaxWidth: 1.2},
			1.2, "recipe:animation/default",
		},
		{
			"a gate wider than wide is capped",
			map[string]any{RecipeKeyTightIntervalMaxWidth: 42.0},
			5.0, "recipe:animation/default",
		},
		{
			"a non-numeric override is ignored",
			map[string]any{RecipeKeyTightIntervalMaxWidth: "wide-ish"},
			2.0, "default",
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got := applyRecipeThresholds(tc.recipe, RecipeClassAnimation, base)
			if got.TightIntervalMaxWidth != tc.wantTight {
				t.Errorf("tight = %v, want %v", got.TightIntervalMaxWidth, tc.wantTight)
			}
			if got.WideIntervalMinWidth != base.WideIntervalMinWidth {
				t.Errorf("wide must be preserved verbatim; got %v", got.WideIntervalMinWidth)
			}
			if got.Source != tc.wantSrc {
				t.Errorf("source = %q, want %q", got.Source, tc.wantSrc)
			}
		})
	}
}

// ---------------------------------------------------------------------------
// Winner selection.
// ---------------------------------------------------------------------------

func cell(rung int, codecName string, crf int, vmaf, bitrate float64) map[string]any {
	return map[string]any{
		"rung": rung, "codec": codecName, "crf": crf,
		"estimated_vmaf": vmaf, "estimated_bitrate_kbps": bitrate,
	}
}

func TestPickAutoWinner(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name       string
		cells      []map[string]any
		target     float64
		budget     float64
		wantStatus string
		wantIndex  int
	}{
		{
			name:       "no cells at all",
			cells:      nil,
			target:     93,
			budget:     8000,
			wantStatus: StatusNoEligibleCells,
			wantIndex:  -1,
		},
		{
			name: "every estimate is NaN",
			cells: []map[string]any{
				cell(1080, "libx264", 23, math.NaN(), math.NaN()),
			},
			target: 93, budget: 8000,
			wantStatus: StatusNoEligibleCells,
			wantIndex:  -1,
		},
		{
			name: "cheapest in-budget cell wins",
			cells: []map[string]any{
				cell(1080, "libx264", 23, 95.0, 6000.0),
				cell(1080, "libx265", 28, 94.0, 4000.0),
				cell(1080, "libsvtav1", 35, 93.5, 4500.0),
			},
			target: 93, budget: 8000,
			wantStatus: StatusBudgetAndQualityMet,
			wantIndex:  1,
		},
		{
			name: "a bitrate tie breaks on higher VMAF",
			cells: []map[string]any{
				cell(1080, "libx264", 23, 93.5, 4000.0),
				cell(1080, "libx265", 28, 97.0, 4000.0),
			},
			target: 93, budget: 8000,
			wantStatus: StatusBudgetAndQualityMet,
			wantIndex:  1,
		},
		{
			name: "a bitrate+VMAF tie breaks on the higher rung",
			cells: []map[string]any{
				cell(720, "libx264", 23, 95.0, 4000.0),
				cell(2160, "libx264", 23, 95.0, 4000.0),
			},
			target: 93, budget: 8000,
			wantStatus: StatusBudgetAndQualityMet,
			wantIndex:  1,
		},
		{
			name: "quality met but every cell blows the budget",
			cells: []map[string]any{
				cell(1080, "libx264", 23, 95.0, 30000.0),
				cell(1080, "libx265", 28, 94.0, 12000.0),
			},
			target: 93, budget: 8000,
			wantStatus: StatusQualityMetBudgetExceeded,
			wantIndex:  1,
		},
		{
			name: "nothing meets quality: closest miss wins",
			cells: []map[string]any{
				cell(1080, "libx264", 23, 70.0, 1000.0),
				cell(1080, "libx265", 28, 88.0, 5000.0),
				cell(1080, "libsvtav1", 35, 85.0, 900.0),
			},
			target: 93, budget: 8000,
			wantStatus: StatusTargetUnmet,
			wantIndex:  1,
		},
		{
			name: "non-finite cells are skipped, finite ones still win",
			cells: []map[string]any{
				cell(1080, "libx264", 23, math.Inf(1), 100.0),
				cell(1080, "libx265", 28, 94.0, 4000.0),
			},
			target: 93, budget: 8000,
			wantStatus: StatusBudgetAndQualityMet,
			wantIndex:  1,
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			winner := PickAutoWinner(tc.cells, tc.target, tc.budget)
			if got := winner["status"]; got != tc.wantStatus {
				t.Fatalf("status = %v, want %v", got, tc.wantStatus)
			}
			if tc.wantIndex < 0 {
				if _, ok := winner["cell_index"]; ok {
					t.Error("an ineligible plan must not carry a cell_index")
				}
				return
			}
			if got := winner["cell_index"]; got != tc.wantIndex {
				t.Errorf("cell_index = %v, want %v", got, tc.wantIndex)
			}
			markSelectedCell(tc.cells, winner)
			for i, c := range tc.cells {
				want := i == tc.wantIndex
				if got, _ := c["selected"].(bool); got != want {
					t.Errorf("cell %d selected = %v, want %v", i, got, want)
				}
			}
		})
	}
}

// ---------------------------------------------------------------------------
// Source probing.
// ---------------------------------------------------------------------------

// TestProbeSourceMetaDegradesGracefully pins the documented fallbacks: every
// probe failure mode lands on 1920x1080 / duration 0 / SDR rather than
// aborting the plan.
func TestProbeSourceMetaDegradesGracefully(t *testing.T) {
	t.Parallel()

	geometry := `{"streams":[{"width":3840,"height":2160,"r_frame_rate":"24000/1001"}]}`
	duration := `{"format":{"duration":"612.500000"}}`
	hdrPayload := `{"streams":[{"color_transfer":"smpte2084","color_primaries":"bt2020",` +
		`"color_space":"bt2020nc","color_range":"tv","pix_fmt":"yuv420p10le"}]}`

	tests := []struct {
		name         string
		responses    map[string]string
		exitCode     int
		wantWidth    int
		wantHeight   int
		wantDuration float64
		wantHDR      bool
	}{
		{
			name: "full probe",
			responses: map[string]string{
				"stream=width,height,r_frame_rate": geometry,
				"format=duration":                  duration,
				"show_streams":                     hdrPayload,
			},
			wantWidth: 3840, wantHeight: 2160, wantDuration: 612.5, wantHDR: true,
		},
		{
			name:      "ffprobe fails everywhere",
			responses: map[string]string{},
			exitCode:  1,
			wantWidth: 1920, wantHeight: 1080, wantDuration: 0.0, wantHDR: false,
		},
		{
			name: "geometry only",
			responses: map[string]string{
				"stream=width,height,r_frame_rate": geometry,
			},
			wantWidth: 3840, wantHeight: 2160, wantDuration: 0.0, wantHDR: false,
		},
		{
			name: "duration as a bare JSON number",
			responses: map[string]string{
				"format=duration": `{"format":{"duration":42.5}}`,
			},
			wantWidth: 1920, wantHeight: 1080, wantDuration: 42.5, wantHDR: false,
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			runner := stubProbeRunner(tc.responses, tc.exitCode)
			meta, info := ProbeSourceMeta(context.Background(), "src.mkv", 0, "ffprobe", runner, nil)
			if meta.Width != tc.wantWidth || meta.Height != tc.wantHeight {
				t.Errorf("geometry = %dx%d, want %dx%d",
					meta.Width, meta.Height, tc.wantWidth, tc.wantHeight)
			}
			if meta.DurationS != tc.wantDuration {
				t.Errorf("duration = %v, want %v", meta.DurationS, tc.wantDuration)
			}
			if meta.IsHDR != tc.wantHDR {
				t.Errorf("is_hdr = %v, want %v", meta.IsHDR, tc.wantHDR)
			}
			if (info != nil) != tc.wantHDR {
				t.Errorf("hdr info presence = %v, want %v", info != nil, tc.wantHDR)
			}
			if meta.ContentClass != "live_action" {
				t.Errorf("content_class = %q, want live_action", meta.ContentClass)
			}
		})
	}
}

// ---------------------------------------------------------------------------
// End-to-end parity against the Python emitter.
// ---------------------------------------------------------------------------

// pythonCase mirrors one entry of testdata/python_plans/index.json.
type pythonCase struct {
	Src               string   `json:"src"`
	TargetVMAF        float64  `json:"target_vmaf"`
	MaxBudgetKbps     float64  `json:"max_budget_kbps"`
	AllowCodecs       []string `json:"allow_codecs"`
	Pinned            *string  `json:"pinned"`
	SampleClipSeconds float64  `json:"sample_clip_seconds"`
	Meta              struct {
		Height            int     `json:"height"`
		Width             int     `json:"width"`
		IsHDR             bool    `json:"is_hdr"`
		ContentClass      string  `json:"content_class"`
		DurationS         float64 `json:"duration_s"`
		ShotVariance      float64 `json:"shot_variance"`
		SampleClipSeconds float64 `json:"sample_clip_seconds"`
		ComplexityScore   float64 `json:"complexity_score"`
		BaselineVMAF      float64 `json:"baseline_vmaf"`
	} `json:"meta"`
}

// TestEmitPlanJSONMatchesPython is the load-bearing parity gate: for each
// recorded case the Go driver must emit the exact bytes the Python
// vmaftune.auto.emit_plan_json produced, NaN tokens and float spelling
// included. The fixtures were generated by scripts run against the in-tree
// Python implementation; regenerate them only alongside a deliberate schema
// change on both sides.
func TestEmitPlanJSONMatchesPython(t *testing.T) {
	t.Parallel()

	indexPath := filepath.Join("testdata", "python_plans", "index.json")
	raw, err := os.ReadFile(indexPath)
	if err != nil {
		t.Fatalf("read fixture index: %v", err)
	}
	var cases map[string]pythonCase
	if err := json.Unmarshal(raw, &cases); err != nil {
		t.Fatalf("parse fixture index: %v", err)
	}
	if len(cases) == 0 {
		t.Fatal("fixture index is empty")
	}

	// The recipe table must resolve the repo's calibrated JSON, exactly as
	// the Python module-level loader does when it walks up from its source
	// file. The test binary's working directory is the package dir, so the
	// upward walk from "." finds the same ai/data file.
	recipes := NewRecipeTable(".", nil)

	for name, tc := range cases {
		t.Run(name, func(t *testing.T) {
			t.Parallel()

			wantPath := filepath.Join("testdata", "python_plans", name+".json")
			want, err := os.ReadFile(wantPath)
			if err != nil {
				t.Fatalf("read fixture: %v", err)
			}

			meta := SourceMeta{
				Height:            tc.Meta.Height,
				Width:             tc.Meta.Width,
				IsHDR:             tc.Meta.IsHDR,
				ContentClass:      tc.Meta.ContentClass,
				DurationS:         tc.Meta.DurationS,
				ShotVariance:      tc.Meta.ShotVariance,
				SampleClipSeconds: tc.Meta.SampleClipSeconds,
				ComplexityScore:   tc.Meta.ComplexityScore,
				BaselineVMAF:      tc.Meta.BaselineVMAF,
			}
			pinned := ""
			if tc.Pinned != nil {
				pinned = *tc.Pinned
			}

			plan, err := RunAuto(context.Background(), Options{
				Src:               tc.Src,
				TargetVMAF:        tc.TargetVMAF,
				MaxBudgetKbps:     tc.MaxBudgetKbps,
				AllowCodecs:       tc.AllowCodecs,
				UserPinnedCodec:   pinned,
				SampleClipSeconds: tc.SampleClipSeconds,
				MetaOverride:      &meta,
				Recipes:           recipes,
			})
			if err != nil {
				t.Fatalf("RunAuto: %v", err)
			}
			got, err := EmitPlanJSON(plan)
			if err != nil {
				t.Fatalf("EmitPlanJSON: %v", err)
			}
			if got != string(want) {
				t.Errorf("plan JSON differs from the Python fixture\n--- want ---\n%s\n--- got ---\n%s",
					want, got)
			}
		})
	}
}

// TestRunAutoSmokeIsDeterministic pins the smoke path: no probes, a synthetic
// GOSPEL verdict, a tight synthetic interval, and placeholder estimates.
func TestRunAutoSmokeIsDeterministic(t *testing.T) {
	t.Parallel()

	opts := Options{
		Src:           "/tmp/clip.mkv",
		TargetVMAF:    93.0,
		MaxBudgetKbps: 8000.0,
		AllowCodecs:   []string{"libx264"},
		Smoke:         true,
		Recipes:       NewRecipeTable(t.TempDir(), nil),
	}
	first, err := RunAuto(context.Background(), opts)
	if err != nil {
		t.Fatalf("RunAuto: %v", err)
	}
	second, err := RunAuto(context.Background(), opts)
	if err != nil {
		t.Fatalf("RunAuto (repeat): %v", err)
	}

	firstJSON, err := EmitPlanJSON(first)
	if err != nil {
		t.Fatalf("EmitPlanJSON: %v", err)
	}
	secondJSON, err := EmitPlanJSON(second)
	if err != nil {
		t.Fatalf("EmitPlanJSON: %v", err)
	}
	if firstJSON != secondJSON {
		t.Error("two smoke runs of the same options produced different plans")
	}

	if len(first.Cells) != 1 {
		t.Fatalf("expected 1 cell, got %d", len(first.Cells))
	}
	c := first.Cells[0]
	if c["prediction_source"] != "smoke-placeholder" {
		t.Errorf("prediction_source = %v, want smoke-placeholder", c["prediction_source"])
	}
	if c["interval_width"] != 1.0 {
		t.Errorf("smoke interval_width = %v, want 1.0", c["interval_width"])
	}
	if c["confidence_decision"] != string(SkipEscalation) {
		t.Errorf("confidence_decision = %v, want %v", c["confidence_decision"], SkipEscalation)
	}
	if c["verdict"] != VerdictGospel {
		t.Errorf("verdict = %v, want %v", c["verdict"], VerdictGospel)
	}
}

// TestRunAutoCellIntervalsSeam pins the F.3 production-wiring behaviour: a
// covered cell uses its supplied width, an uncovered cell degrades to NaN
// (deferring to the native verdict) rather than borrowing a synthetic width.
func TestRunAutoCellIntervalsSeam(t *testing.T) {
	t.Parallel()

	meta := SourceMeta{Height: 1080, Width: 1920, ContentClass: "live_action"}
	plan, err := RunAuto(context.Background(), Options{
		Src:           "/tmp/clip.mkv",
		TargetVMAF:    93.0,
		MaxBudgetKbps: 8000.0,
		AllowCodecs:   []string{"libx264", "libx265"},
		MetaOverride:  &meta,
		Recipes:       NewRecipeTable(t.TempDir(), nil),
		CellIntervals: []CellInterval{
			{Rung: 1080, Codec: "libx264", Verdict: VerdictFallBack, IntervalWidth: 0.5},
		},
	})
	if err != nil {
		t.Fatalf("RunAuto: %v", err)
	}
	if len(plan.Cells) != 2 {
		t.Fatalf("expected 2 cells, got %d", len(plan.Cells))
	}

	covered := plan.Cells[0]
	if covered["interval_width"] != 0.5 {
		t.Errorf("covered interval_width = %v, want 0.5", covered["interval_width"])
	}
	if covered["confidence_decision"] != string(SkipEscalation) {
		t.Errorf("a tight interval must override FALL_BACK; got %v",
			covered["confidence_decision"])
	}
	if covered["verdict"] != VerdictFallBack {
		t.Errorf("covered verdict = %v, want %v", covered["verdict"], VerdictFallBack)
	}

	uncovered := plan.Cells[1]
	width, ok := uncovered["interval_width"].(float64)
	if !ok || !math.IsNaN(width) {
		t.Errorf("uncovered interval_width = %v, want NaN", uncovered["interval_width"])
	}
	if uncovered["confidence_decision"] != string(RecommendEscalation) {
		t.Errorf("an uncovered cell must defer; got %v", uncovered["confidence_decision"])
	}
}

// TestRunAutoForceSingleRungFromRecipe checks the documented F.4 ordering: the
// recipe fires before the ladder stage, so force_single_rung collapses a 4K
// source to one rung.
func TestRunAutoForceSingleRungFromRecipe(t *testing.T) {
	t.Parallel()

	recipes := NewRecipeTable(t.TempDir(), nil) // placeholders: animation forces single rung

	tests := []struct {
		name         string
		contentClass string
		wantRungs    int
	}{
		{"live action keeps the multi-rung ladder", "live_action", 5},
		{"animation collapses to a single rung", "animation", 1},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			meta := SourceMeta{Height: 2160, Width: 3840, ContentClass: tc.contentClass}
			plan, err := RunAuto(context.Background(), Options{
				Src:           "/tmp/uhd.mkv",
				TargetVMAF:    93.0,
				MaxBudgetKbps: 20000.0,
				AllowCodecs:   []string{"libx264"},
				MetaOverride:  &meta,
				Recipes:       recipes,
			})
			if err != nil {
				t.Fatalf("RunAuto: %v", err)
			}
			if len(plan.Cells) != tc.wantRungs {
				t.Errorf("cells = %d, want %d", len(plan.Cells), tc.wantRungs)
			}
		})
	}
}

// stubProbeRunner returns a runner that answers by matching a substring of the
// argv against the response table; unmatched calls report exit 1.
func stubProbeRunner(responses map[string]string, exitCode int) func(
	ctx context.Context, argv []string) (string, int, error) {
	return func(_ context.Context, argv []string) (string, int, error) {
		if exitCode != 0 {
			return "", exitCode, nil
		}
		joined := ""
		for _, a := range argv {
			joined += a + " "
		}
		for needle, payload := range responses {
			if contains(joined, needle) {
				return payload, 0, nil
			}
		}
		return "", 1, nil
	}
}

func contains(haystack, needle string) bool {
	return len(needle) > 0 && len(haystack) >= len(needle) &&
		(indexOf(haystack, needle) >= 0)
}

func indexOf(haystack, needle string) int {
	for i := 0; i+len(needle) <= len(haystack); i++ {
		if haystack[i:i+len(needle)] == needle {
			return i
		}
	}
	return -1
}
