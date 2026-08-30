// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

// Package prefilter is the Go port of tools/vmaf-tune/src/vmaftune/
// prefilter.py plus its filter_adapters/pelorus_deband.py dependency
// (ADR-1116 / ADR-0106, against the frozen Pelorus ADR-0110 contract).
//
// It drives a VMAF-in-the-loop joint search over the Pelorus deband filter's
// ten strength knobs AND the encoder's CRF axis. Each trial proposes a
// (deband, crf) point, runs deband -> encode -> score through an injected
// probe, and feeds the achieved VMAF plus bitrate back to the sampler.
//
// vmafx is Vulkan-free: it never runs the deband filter itself. The adapter
// only emits the "-vf" fragment string; the live probe path is gated behind
// FilterAvailable because the filter must be compiled into the ffmpeg build.
package prefilter

import (
	"fmt"
	"sort"
	"strconv"
	"strings"
)

// FilterName is the ffmpeg filter emitted in the -vf fragment. The contract
// (ADR-0110) freezes the Vulkan variant specifically.
const FilterName = "pelorus_deband_vulkan"

// KnobKind mirrors the AVOption type. "bool" is a 0/1 integer in FFmpeg but
// stays distinct so the search-space builder can treat it as a binary
// categorical rather than a wide ordinal range.
type KnobKind string

const (
	KindInt   KnobKind = "int"
	KindFloat KnobKind = "float"
	KindBool  KnobKind = "bool"
	KindEnum  KnobKind = "enum"
)

// Knob is one tunable AVOption from the frozen control-plane contract.
// Lo and Hi are inclusive; Default is the filter's own default, used when the
// knob is omitted from a parameter set.
type Knob struct {
	Name      string
	Kind      KnobKind
	Lo        float64
	Hi        float64
	Default   float64
	Semantics string
}

// IsIntegral reports whether the AVOption only accepts integer values.
func (k Knob) IsIntegral() bool {
	return k.Kind == KindInt || k.Kind == KindBool || k.Kind == KindEnum
}

// DebandKnobs is the frozen contract (Pelorus ADR-0110 /
// docs/api/control-plane.md).
//
// DO NOT widen, narrow, rename, or retype without a coordinated two-repo PR.
// The order matches the contract table top to bottom, and the emitted -vf
// fragment follows this order so the string is deterministic and
// diff-friendly.
//
// The out-of-contract AVOptions (sample / blur / planes / meta) are
// pipeline-topology or reporting switches: they are set once per run by the
// harness and are intentionally absent here so the optimizer cannot sweep
// them.
var DebandKnobs = []Knob{
	{"range", KindInt, 1.0, 31.0, 15.0, "reference-sampling radius in pixels"},
	{"thry", KindFloat, 0.0, 0.25, 0.012, "luma flat-test threshold (normalized full-range)"},
	{"thrc", KindFloat, 0.0, 0.25, 0.012, "chroma flat-test threshold (normalized)"},
	{"grainy", KindFloat, 0.0, 0.4, 0.006, "luma grain amplitude (normalized)"},
	{"grainc", KindFloat, 0.0, 0.4, 0.0, "chroma grain amplitude (normalized)"},
	{"softness", KindFloat, 0.0, 1.0, 0.5, "soft-blend transition width (0 = hard switch)"},
	{"detail", KindFloat, 0.0, 0.25, 0.06, "detail-mask activity threshold (normalized)"},
	{"dither", KindEnum, 0.0, 2.0, 2.0, "0=none, 1=bayer8, 2=bluenoise"},
	{"dynamic", KindBool, 0.0, 1.0, 1.0, "re-seed grain each frame"},
	{"protect", KindBool, 0.0, 1.0, 1.0, "gate debanding off textured regions"},
}

// DitherModes maps the dither enum onto its canonical name. The AVOption
// parser accepts both forms; the fragment emits integers for a stable,
// locale-independent string.
var DitherModes = map[int]string{0: "none", 1: "bayer8", 2: "bluenoise"}

// knobByName is the O(1) validation lookup.
var knobByName = func() map[string]Knob {
	m := make(map[string]Knob, len(DebandKnobs))
	for _, k := range DebandKnobs {
		m[k.Name] = k
	}
	return m
}()

// Adapter is the pre-encode filter adapter. It is stateless, so one value is
// safe to share across trials.
//
// AdapterVersion bumps when the knob table or the emission shape changes; it
// folds into any cache key the search derives, so a contract update
// invalidates stale recommendations (ADR-0298 convention).
type Adapter struct {
	Name           string
	FilterName     string
	AdapterVersion string
}

// PelorusDeband returns the shipped adapter.
func PelorusDeband() Adapter {
	return Adapter{Name: "pelorus_deband", FilterName: FilterName, AdapterVersion: "1"}
}

// registry maps adapter names onto their constructors.
var registry = map[string]func() Adapter{
	"pelorus_deband": PelorusDeband,
}

// GetAdapter returns the registered adapter named name.
func GetAdapter(name string) (Adapter, error) {
	ctor, ok := registry[name]
	if !ok {
		return Adapter{}, fmt.Errorf(
			"unknown filter %q; known pre-encode filters: %v", name, KnownFilters())
	}
	return ctor(), nil
}

// KnownFilters returns the sorted adapter names.
func KnownFilters() []string {
	out := make([]string, 0, len(registry))
	for k := range registry {
		out = append(out, k)
	}
	sort.Strings(out)
	return out
}

// Knobs returns the frozen knob table in contract order.
func (a Adapter) Knobs() []Knob {
	out := make([]Knob, len(DebandKnobs))
	copy(out, DebandKnobs)
	return out
}

// Knob returns the named knob or an error naming the contract.
func (a Adapter) Knob(name string) (Knob, error) {
	k, ok := knobByName[name]
	if !ok {
		return Knob{}, fmt.Errorf(
			"unknown pelorus deband knob %q; the frozen contract (ADR-0110) "+
				"exposes: %v", name, knobNames())
	}
	return k, nil
}

// knobNames returns the sorted contract knob names.
func knobNames() []string {
	out := make([]string, 0, len(knobByName))
	for k := range knobByName {
		out = append(out, k)
	}
	sort.Strings(out)
	return out
}

// Defaults returns the contract default for every knob.
func (a Adapter) Defaults() map[string]float64 {
	out := make(map[string]float64, len(DebandKnobs))
	for _, k := range DebandKnobs {
		out[k.Name] = k.Default
	}
	return out
}

// Validate rejects unknown knob names (including the deliberately
// out-of-contract sample / blur / planes / meta) and out-of-range values.
// Integral knobs additionally reject non-integer inputs, so a fractional
// "range" or "dither" is a hard error rather than a silently-rounded
// surprise.
func (a Adapter) Validate(params map[string]float64) error {
	for key, value := range params {
		knob, ok := knobByName[key]
		if !ok {
			return fmt.Errorf(
				"%q is not an autotune knob in the pelorus deband control-plane "+
					"contract (ADR-0110). Tunable knobs: %v. Note "+
					"sample/blur/planes/meta are intentionally out-of-contract and "+
					"must not be swept.", key, knobNames())
		}
		if value != value { // NaN
			return fmt.Errorf("%s: value is NaN", key)
		}
		if value < knob.Lo || value > knob.Hi {
			return fmt.Errorf(
				"%s=%v is outside the frozen contract range [%g, %g]",
				key, value, knob.Lo, knob.Hi)
		}
		if knob.IsIntegral() && value != float64(int64(value)) {
			return fmt.Errorf(
				"%s=%v must be an integer (%s knob); fractional values are "+
					"rejected (the AVOption is integral)", key, value, knob.Kind)
		}
	}
	return nil
}

// Clamp bounds every supplied knob into its contract range, rounding
// integral knobs afterwards. Unknown knob names still error — clamping is for
// out-of-range numbers, not for typos or out-of-contract options.
func (a Adapter) Clamp(params map[string]float64) (map[string]float64, error) {
	out := make(map[string]float64, len(params))
	for key, value := range params {
		knob, ok := knobByName[key]
		if !ok {
			return nil, fmt.Errorf(
				"cannot clamp unknown knob %q; not in the frozen contract (ADR-0110)",
				key)
		}
		v := value
		if v < knob.Lo {
			v = knob.Lo
		}
		if v > knob.Hi {
			v = knob.Hi
		}
		if knob.IsIntegral() {
			v = float64(roundHalfAwayFromZero(v))
		}
		out[key] = v
	}
	return out, nil
}

// roundHalfAwayFromZero rounds like Python's round-half-to-even would NOT:
// it matches int(round(x)) for the positive, non-tie values the contract's
// integral knobs take. Ties are vanishingly rare here (the sampler proposes
// integers directly), and away-from-zero is the intuitive reading.
func roundHalfAwayFromZero(v float64) int {
	if v < 0 {
		return -int(-v + 0.5)
	}
	return int(v + 0.5)
}

// FormatValue renders a validated value into its ffmpeg AVOption token.
//
// Integral knobs emit a bare integer; floats emit a %g token so 0.012 stays
// "0.012" and 0.0 collapses to "0" (both accepted by the AVOption parser)
// without trailing-zero noise. The contract is bit-depth independent, so no
// locale-sensitive formatting is involved.
func FormatValue(knob Knob, value float64) string {
	if knob.IsIntegral() {
		return strconv.Itoa(roundHalfAwayFromZero(value))
	}
	return strconv.FormatFloat(value, 'g', -1, 64)
}

// VFFragment emits the "pelorus_deband_vulkan=key=val:..." fragment.
//
// Values are validated against the frozen contract before emission. Omitted
// knobs fall back to the filter's own default: with includeDefaults false
// they are simply left off the fragment; with it true every contract knob is
// emitted explicitly, which is what a reproducible fully-pinned encode
// command and a parameter-set diff both want.
//
// Knobs are emitted in contract order regardless of map iteration order, so
// the fragment is deterministic.
func (a Adapter) VFFragment(params map[string]float64, includeDefaults bool) (string, error) {
	if err := a.Validate(params); err != nil {
		return "", err
	}
	resolved := make(map[string]float64, len(params))
	for k, v := range params {
		resolved[k] = v
	}
	if includeDefaults {
		merged := a.Defaults()
		for k, v := range resolved {
			merged[k] = v
		}
		resolved = merged
	}
	tokens := make([]string, 0, len(DebandKnobs))
	for _, knob := range DebandKnobs {
		v, ok := resolved[knob.Name]
		if !ok {
			continue
		}
		tokens = append(tokens, knob.Name+"="+FormatValue(knob, v))
	}
	if len(tokens) == 0 {
		// No knobs requested and defaults not forced: emit the bare filter
		// name so the chain is still a valid -vf entry.
		return a.FilterName, nil
	}
	return a.FilterName + "=" + strings.Join(tokens, ":"), nil
}

// VFArgs returns the ["-vf", "<fragment>"] argv slice.
func (a Adapter) VFArgs(params map[string]float64, includeDefaults bool) ([]string, error) {
	fragment, err := a.VFFragment(params, includeDefaults)
	if err != nil {
		return nil, err
	}
	return []string{"-vf", fragment}, nil
}
