// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent

package auto

import (
	"encoding/json"
	"log/slog"
	"os"
	"path/filepath"
	"strings"
)

// Recipe-class sentinels recorded in plan.metadata.recipe_applied. "default"
// is the no-recipe path; the four named classes correspond to the recipes
// documented in Research-0067.
const (
	RecipeClassDefault       = "default"
	RecipeClassAnimation     = "animation"
	RecipeClassScreenContent = "screen_content"
	RecipeClassLiveActionHDR = "live_action_hdr"
	RecipeClassUGC           = "ugc"
)

// Recipe override keys (F.4, ADR-0325 §F.4). Only these four are honoured;
// anything else in a calibrated JSON is dropped before it can reach
// plan.metadata.recipe_overrides.
const (
	RecipeKeyTightIntervalMaxWidth = "tight_interval_max_width"
	RecipeKeyForceSingleRung       = "force_single_rung"
	RecipeKeySaliencyIntensity     = "saliency_intensity"
	RecipeKeyTargetVMAFOffset      = "target_vmaf_offset"
)

var recipeKeys = map[string]bool{
	RecipeKeyTightIntervalMaxWidth: true,
	RecipeKeyForceSingleRung:       true,
	RecipeKeySaliencyIntensity:     true,
	RecipeKeyTargetVMAFOffset:      true,
}

// CalibratedRecipesFilename is the repo-relative path to the F.5 calibration
// output produced by ai/scripts/calibrate_phase_f_recipes.py.
const CalibratedRecipesFilename = "ai/data/phase_f_recipes_calibrated.json"

// f4PlaceholderRecipes are the F.4 documented placeholders, used when the F.5
// calibrated JSON is missing or malformed.
//
// Every value here is a documented placeholder, not a measured outcome — the
// per-class rationale lives in Research-0067 §"F.4 recipe-override
// placeholders". Animation gets a tighter conformal gate and a single-rung
// ladder (it compresses uniformly on flat colour fields); screen content gets
// the most aggressive saliency (high QP on background, near-lossless on
// text); live-action HDR narrows the gate further because a wide interval on
// HDR is more suspect than on SDR; UGC widens it because predictor
// uncertainty is the UGC baseline.
func f4PlaceholderRecipes() map[string]map[string]any {
	return map[string]map[string]any{
		RecipeClassAnimation: {
			RecipeKeyTightIntervalMaxWidth: 1.5,
			RecipeKeyForceSingleRung:       true,
			RecipeKeySaliencyIntensity:     "aggressive",
			RecipeKeyTargetVMAFOffset:      2.0,
		},
		RecipeClassScreenContent: {
			RecipeKeySaliencyIntensity: "very_aggressive",
			RecipeKeyTargetVMAFOffset:  1.0,
		},
		RecipeClassLiveActionHDR: {
			RecipeKeyTightIntervalMaxWidth: 1.2,
			RecipeKeyTargetVMAFOffset:      0.0,
		},
		RecipeClassUGC: {
			RecipeKeyTightIntervalMaxWidth: 3.0,
			RecipeKeyTargetVMAFOffset:      -1.0,
		},
	}
}

// FindCalibratedRecipesPath walks upward from start looking for the F.5
// calibrated JSON, returning "" when it cannot be located.
//
// The Python loader walks up from the module's __file__, which pins it to the
// source tree. A compiled Go binary has no source location, so we walk up
// from the supplied directory (the caller passes the working directory) and,
// failing that, from the executable's directory. Both resolve the same file
// for an in-repo invocation; neither resolves it for an installed binary,
// which is exactly the graceful-degradation path the Python loader also takes
// (placeholders plus a one-line warning).
func FindCalibratedRecipesPath(start string) string {
	for _, root := range candidateRoots(start) {
		if root == "" {
			continue
		}
		dir := root
		for {
			candidate := filepath.Join(dir, CalibratedRecipesFilename)
			if info, err := os.Stat(candidate); err == nil && !info.IsDir() {
				return candidate
			}
			parent := filepath.Dir(dir)
			if parent == dir {
				break
			}
			dir = parent
		}
	}
	return ""
}

func candidateRoots(start string) []string {
	roots := make([]string, 0, 2)
	if start != "" {
		if abs, err := filepath.Abs(start); err == nil {
			roots = append(roots, abs)
		}
	}
	if exe, err := os.Executable(); err == nil {
		roots = append(roots, filepath.Dir(exe))
	}
	return roots
}

// LoadCalibratedRecipes returns the per-content-class override table, merging
// the F.5 calibrated JSON over the F.4 placeholders.
//
// Failure paths — no JSON found, unreadable, malformed, missing "recipes"
// object — all degrade to the placeholders with a one-line warning. Unknown
// classes in the JSON are ignored; "_"-prefixed keys (the calibrator's
// _provenance blocks) are stripped so they never leak into
// plan.metadata.recipe_overrides.
func LoadCalibratedRecipes(searchStart string, log *slog.Logger) map[string]map[string]any {
	if log == nil {
		log = slog.Default()
	}
	merged := f4PlaceholderRecipes()

	path := FindCalibratedRecipesPath(searchStart)
	if path == "" {
		log.Warn("no calibrated recipes JSON found; using F.4 placeholder recipes " +
			"(documented-placeholder values, not measured outcomes)")
		return merged
	}
	data, err := os.ReadFile(path) // #nosec G304 -- path is resolved by an upward walk over the repo tree
	if err != nil {
		log.Warn("failed to read calibrated recipes; falling back to F.4 placeholders",
			"path", path, "error", err)
		return merged
	}
	var payload struct {
		Recipes map[string]map[string]any `json:"recipes"`
	}
	if err := json.Unmarshal(data, &payload); err != nil {
		log.Warn("calibrated recipes JSON is malformed; falling back to F.4 placeholders",
			"path", path, "error", err)
		return merged
	}
	if payload.Recipes == nil {
		log.Warn("calibrated recipes JSON missing 'recipes' object; "+
			"falling back to F.4 placeholders", "path", path)
		return merged
	}

	for class, raw := range payload.Recipes {
		if _, known := merged[class]; !known {
			continue
		}
		clean := map[string]any{}
		for key, value := range raw {
			if strings.HasPrefix(key, "_") {
				continue
			}
			if recipeKeys[key] {
				clean[key] = value
			}
		}
		if len(clean) > 0 {
			merged[class] = clean
		}
	}
	return merged
}

// RecipeTable holds the resolved per-class overrides for one auto run.
// Construct it once per run so a mutation from one plan cannot leak into the
// next (the Python module keeps the same invariant with per-class factories).
type RecipeTable struct {
	byClass map[string]map[string]any
}

// NewRecipeTable loads the calibrated recipes, searching upward from
// searchStart.
func NewRecipeTable(searchStart string, log *slog.Logger) *RecipeTable {
	return &RecipeTable{byClass: LoadCalibratedRecipes(searchStart, log)}
}

// ForClass returns a fresh override map for the named class. Mutating the
// result never affects the table. Unknown class strings degrade to the empty
// default recipe, and only the four documented keys survive.
func (t *RecipeTable) ForClass(contentClass string) map[string]any {
	out := map[string]any{}
	if t == nil {
		return out
	}
	src, ok := t.byClass[strings.ToLower(strings.TrimSpace(contentClass))]
	if !ok {
		return out
	}
	for k, v := range src {
		if recipeKeys[k] {
			out[k] = v
		}
	}
	return out
}

// ResolveRecipeClass maps a SourceMeta onto a recipe-table key.
//
// The HDR signal trumps a generic live-action label: an operator who passes
// content_class=live_action on an HDR source still gets the HDR recipe, which
// matches ADR-0300's permissive HDR detection. An explicit animation /
// screen_content / UGC label wins over the HDR promotion.
func ResolveRecipeClass(meta SourceMeta) string {
	raw := strings.ToLower(strings.TrimSpace(meta.ContentClass))
	switch raw {
	case RecipeClassLiveActionHDR:
		return RecipeClassLiveActionHDR
	case RecipeClassAnimation, RecipeClassScreenContent, RecipeClassUGC:
		return raw
	}
	if meta.IsHDR {
		return RecipeClassLiveActionHDR
	}
	return RecipeClassDefault
}
