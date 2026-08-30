// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

// Package codecadapter is the Go port of the argv-shaping half of
// vmaftune.codec_adapters (ADR-0237 / ADR-0294 / ADR-0326).
//
// Every codec exposes a different FFmpeg parameter shape — x264 dials `-crf`,
// NVENC `-cq`, QSV `-global_quality`, AMF a `-rc cqp -qp_i/-qp_p` block, libaom
// and libvpx `-cpu-used`, VideoToolbox `-realtime` plus `-q:v`, VVenC `-qp`.
// The adapter contract is what keeps that difference out of every caller: an
// adapter answers "what is the `-c:v ...` argv slice for this (preset,
// quality)?" and nothing branches on codec identity.
//
// Scope: this port covers the runtime-shaped subset the encode driver consumes
// — Encoder, Presets, FFmpegCodecArgs and ExtraParams. The Python adapters
// carry more surface that no Go subcommand needs yet (validate, gop_args,
// force_keyframes_args, probe_args, two_pass_args, saliency ROI helpers,
// per-adapter quality windows). Those stay in Python until a Go caller needs
// them; porting dead API would be speculative and unverifiable.
//
// Parity note — ExtraParams takes (preset, quality) because CPython's
// encode._resolve_codec_args inspects each adapter's extra_params signature and
// passes those two arguments when the adapter declares them. Only the AMF
// adapters do; every other adapter's extra_params ignores both.
package codecadapter

import (
	"fmt"
	"slices"
	"sort"
	"strconv"
	"strings"
)

// Adapter is one codec's argv shape.
type Adapter struct {
	// Name is the registry key (identical to Encoder for every adapter in
	// the Python registry, kept separate to mirror the contract).
	Name string

	// Encoder is the FFmpeg encoder token emitted after `-c:v`.
	Encoder string

	// QualityKnob names the quality axis this codec dials: "crf", "cq",
	// "global_quality", "qp", "q:v" or "profile".
	QualityKnob string

	// Presets is the adapter's canonical preset vocabulary, in the order the
	// Python adapter declares it.
	Presets []string

	// codecArgs builds the `-c:v ...` slice. Never nil.
	codecArgs func(a *Adapter, preset string, quality int) ([]string, error)

	// extraParams builds the trailing non-codec argv. Nil means "none".
	extraParams func(a *Adapter, preset string, quality int) ([]string, error)
}

// FFmpegCodecArgs returns the `-c:v ...` argv slice for one encode.
func (a *Adapter) FFmpegCodecArgs(preset string, quality int) ([]string, error) {
	return a.codecArgs(a, preset, quality)
}

// ExtraParams returns the adapter-level argv appended after the codec slice.
// Returns nil for adapters that contribute none.
func (a *Adapter) ExtraParams(preset string, quality int) ([]string, error) {
	if a.extraParams == nil {
		return nil, nil
	}
	return a.extraParams(a, preset, quality)
}

// ResolvedArgs is the concatenation CPython's encode._resolve_codec_args
// produces: the codec slice followed by the adapter's extra params.
func (a *Adapter) ResolvedArgs(preset string, quality int) ([]string, error) {
	codec, err := a.FFmpegCodecArgs(preset, quality)
	if err != nil {
		return nil, err
	}
	extra, err := a.ExtraParams(preset, quality)
	if err != nil {
		return nil, err
	}
	return append(codec, extra...), nil
}

// HasPreset reports whether preset is in the adapter's vocabulary.
func (a *Adapter) HasPreset(preset string) bool {
	return slices.Contains(a.Presets, preset)
}

// ---------------------------------------------------------------------------
// Preset vocabularies (verbatim from the Python adapters, order preserved)
// ---------------------------------------------------------------------------

// x264Presets is libx264's nine-name vocabulary.
var x264Presets = []string{
	"ultrafast", "superfast", "veryfast", "faster", "fast",
	"medium", "slow", "slower", "veryslow",
}

// x265Presets adds "placebo" to the x264 nine.
var x265Presets = []string{
	"ultrafast", "superfast", "veryfast", "faster", "fast",
	"medium", "slow", "slower", "veryslow", "placebo",
}

// nvencPresets is the ten-name mnemonic superset NVENC collapses onto p1..p7.
var nvencPresets = []string{
	"ultrafast", "superfast", "veryfast", "faster", "fast",
	"medium", "slow", "slower", "slowest", "placebo",
}

// amfPresets matches nvencPresets; AMF collapses them onto three rungs.
var amfPresets = []string{
	"ultrafast", "superfast", "veryfast", "faster", "fast",
	"medium", "slow", "slower", "slowest", "placebo",
}

// qsvPresets is FFmpeg's QSV bridge vocabulary — no "ultrafast" rung.
var qsvPresets = []string{
	"veryslow", "slower", "slow", "medium", "fast", "faster", "veryfast",
}

// videoToolboxPresets shares the x264 nine so callers can use one preset list.
var videoToolboxPresets = []string{
	"ultrafast", "superfast", "veryfast", "faster", "fast",
	"medium", "slow", "slower", "veryslow",
}

// svtAV1Presets is SVT-AV1's eight-name vocabulary (slow-to-fast order).
var svtAV1Presets = []string{
	"placebo", "slowest", "slower", "slow", "medium", "fast", "faster", "veryfast",
}

// cpuUsedPresets is the ten-name vocabulary libaom / libvpx / VVenC declare.
var cpuUsedPresets = []string{
	"placebo", "slowest", "slower", "slow", "medium",
	"fast", "faster", "veryfast", "superfast", "ultrafast",
}

// ---------------------------------------------------------------------------
// Preset projections
// ---------------------------------------------------------------------------

// nvencPresetMap collapses the ten mnemonics onto NVENC's seven pN levels.
var nvencPresetMap = map[string]string{
	"ultrafast": "p1", "superfast": "p1", "veryfast": "p1",
	"faster": "p2", "fast": "p3", "medium": "p4",
	"slow": "p5", "slower": "p6", "slowest": "p7", "placebo": "p7",
}

// amfQualityMap collapses the ten mnemonics onto AMF's three -quality rungs.
var amfQualityMap = map[string]string{
	"placebo": "quality", "slowest": "quality", "slower": "quality", "slow": "quality",
	"medium": "balanced",
	"fast":   "speed", "faster": "speed", "veryfast": "speed",
	"superfast": "speed", "ultrafast": "speed",
}

// svtAV1PresetInt is SVT-AV1's name-to-integer preset lookup.
var svtAV1PresetInt = map[string]int{
	"placebo": 0, "slowest": 1, "slower": 3, "slow": 5,
	"medium": 7, "fast": 9, "faster": 11, "veryfast": 13,
}

// libaomCPUUsed maps the ten-name vocabulary onto libaom's -cpu-used 0..9.
var libaomCPUUsed = map[string]int{
	"placebo": 0, "slowest": 1, "slower": 2, "slow": 3, "medium": 4,
	"fast": 5, "faster": 6, "veryfast": 7, "superfast": 8, "ultrafast": 9,
}

// libvpxCPUUsed maps the ten-name vocabulary onto libvpx's narrower 0..5.
var libvpxCPUUsed = map[string]int{
	"placebo": 0, "slowest": 0, "slower": 1, "slow": 2, "medium": 3,
	"fast": 4, "faster": 5, "veryfast": 5, "superfast": 5, "ultrafast": 5,
}

// vvencNativePreset collapses the ten mnemonics onto VVenC's five native names.
var vvencNativePreset = map[string]string{
	"placebo": "slower", "slowest": "slower", "slower": "slower",
	"slow": "slow", "medium": "medium", "fast": "fast",
	"faster": "faster", "veryfast": "faster",
	"superfast": "faster", "ultrafast": "faster",
}

// vtRealtime maps a preset onto VideoToolbox's binary -realtime flag.
var vtRealtime = map[string]int{
	"ultrafast": 1, "superfast": 1, "veryfast": 1, "faster": 1, "fast": 1,
	"medium": 0, "slow": 0, "slower": 0, "veryslow": 0,
}

// proresProfileNames indexes the ProRes tier aliases by integer tier id, in
// canonical FFmpeg order (AV_PROFILE_PRORES_* in libavcodec/profiles.h).
var proresProfileNames = []string{"proxy", "lt", "standard", "hq", "4444", "xq"}

// ---------------------------------------------------------------------------
// Shared argv builders
// ---------------------------------------------------------------------------

// presetCRFArgs is the `-c:v ENC -preset P -crf Q` shape shared by libx264 and
// libx265.
func presetCRFArgs(a *Adapter, preset string, quality int) ([]string, error) {
	if !a.HasPreset(preset) {
		return nil, unknownPreset(a, preset)
	}
	return []string{"-c:v", a.Encoder, "-preset", preset, "-crf", strconv.Itoa(quality)}, nil
}

// nvencArgs is the `-c:v ENC -preset pN -cq Q` shape.
func nvencArgs(a *Adapter, preset string, quality int) ([]string, error) {
	p, ok := nvencPresetMap[preset]
	if !ok {
		return nil, unknownPreset(a, preset)
	}
	return []string{"-c:v", a.Encoder, "-preset", p, "-cq", strconv.Itoa(quality)}, nil
}

// qsvArgs is the `-c:v ENC -preset P -global_quality Q` ICQ shape. QSV's
// preset projection is the identity over its own vocabulary.
func qsvArgs(a *Adapter, preset string, quality int) ([]string, error) {
	if !a.HasPreset(preset) {
		return nil, unknownPreset(a, preset)
	}
	return []string{
		"-c:v", a.Encoder, "-preset", preset, "-global_quality", strconv.Itoa(quality),
	}, nil
}

// amfBlock is the `-quality X -rc cqp -qp_i Q -qp_p Q` block AMF uses in place
// of FFmpeg's generic -preset.
func amfBlock(preset string, quality int) ([]string, error) {
	q, ok := amfQualityMap[preset]
	if !ok {
		return nil, fmt.Errorf(
			"unknown AMF preset %q; expected one of %s", preset, strings.Join(sortedKeys(amfQualityMap), ", "))
	}
	qs := strconv.Itoa(quality)
	return []string{"-quality", q, "-rc", "cqp", "-qp_i", qs, "-qp_p", qs}, nil
}

// amfArgs is the AMF codec slice.
func amfArgs(a *Adapter, preset string, quality int) ([]string, error) {
	block, err := amfBlock(preset, quality)
	if err != nil {
		return nil, err
	}
	return append([]string{"-c:v", a.Encoder}, block...), nil
}

// amfExtraParams reproduces a CPython quirk deliberately.
//
// The AMF adapters are the only ones whose extra_params takes (preset, qp)
// rather than (). encode._resolve_codec_args inspects the signature and, for
// two-parameter adapters, appends the return value after the codec slice — so
// the emitted argv carries the `-quality/-rc/-qp_i/-qp_p` block twice, with
// identical values. FFmpeg's AVOption layer takes last-wins, so the duplicate
// is inert, but it IS in the argv the Python CLI prints under --dry-run and
// records in corpus rows. Dropping it here would make the Go dry-run output
// differ from Python's for every AMF row, so the port keeps it and flags it.
func amfExtraParams(_ *Adapter, preset string, quality int) ([]string, error) {
	return amfBlock(preset, quality)
}

// videoToolboxArgs is the `-c:v ENC -realtime {0,1} -q:v Q` shape.
func videoToolboxArgs(a *Adapter, preset string, quality int) ([]string, error) {
	rt, ok := vtRealtime[preset]
	if !ok {
		return nil, unknownPreset(a, preset)
	}
	return []string{
		"-c:v", a.Encoder, "-realtime", strconv.Itoa(rt), "-q:v", strconv.Itoa(quality),
	}, nil
}

// unknownPreset renders the adapter's preset-rejection error.
func unknownPreset(a *Adapter, preset string) error {
	return fmt.Errorf("unknown %s preset %q; expected one of %s",
		a.Name, preset, strings.Join(a.Presets, ", "))
}

// sortedKeys returns a map's keys in sorted order, for stable error text.
func sortedKeys(m map[string]string) []string {
	out := make([]string, 0, len(m))
	for k := range m {
		out = append(out, k)
	}
	sort.Strings(out)
	return out
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

// registry mirrors codec_adapters._REGISTRY.
var registry = buildRegistry()

// buildRegistry constructs the adapter table. It is a function rather than a
// composite literal so the per-codec closures can be shared.
func buildRegistry() map[string]*Adapter {
	reg := map[string]*Adapter{}

	add := func(a *Adapter) { reg[a.Name] = a }

	add(&Adapter{
		Name: "libx264", Encoder: "libx264", QualityKnob: "crf",
		Presets: x264Presets, codecArgs: presetCRFArgs,
	})
	add(&Adapter{
		Name: "libx265", Encoder: "libx265", QualityKnob: "crf",
		Presets: x265Presets, codecArgs: presetCRFArgs,
	})

	// SVT-AV1 takes an integer preset through FFmpeg's generic -preset slot.
	add(&Adapter{
		Name: "libsvtav1", Encoder: "libsvtav1", QualityKnob: "crf",
		Presets: svtAV1Presets,
		codecArgs: func(a *Adapter, preset string, quality int) ([]string, error) {
			n, ok := svtAV1PresetInt[preset]
			if !ok {
				return nil, unknownPreset(a, preset)
			}
			return []string{
				"-c:v", a.Encoder, "-preset", strconv.Itoa(n), "-crf", strconv.Itoa(quality),
			}, nil
		},
	})

	// libaom dials -cpu-used, not -preset.
	add(&Adapter{
		Name: "libaom-av1", Encoder: "libaom-av1", QualityKnob: "crf",
		Presets: cpuUsedPresets,
		codecArgs: func(a *Adapter, preset string, quality int) ([]string, error) {
			n, ok := libaomCPUUsed[preset]
			if !ok {
				return nil, unknownPreset(a, preset)
			}
			return []string{
				"-c:v", a.Encoder, "-cpu-used", strconv.Itoa(n), "-crf", strconv.Itoa(quality),
			}, nil
		},
	})

	// libvpx-vp9 pins the "good" deadline and VBR (-b:v 0), and enables row
	// multithreading through extra_params.
	add(&Adapter{
		Name: "libvpx-vp9", Encoder: "libvpx-vp9", QualityKnob: "crf",
		Presets: cpuUsedPresets,
		codecArgs: func(a *Adapter, preset string, quality int) ([]string, error) {
			n, ok := libvpxCPUUsed[preset]
			if !ok {
				return nil, unknownPreset(a, preset)
			}
			return []string{
				"-c:v", a.Encoder,
				"-deadline", "good",
				"-cpu-used", strconv.Itoa(n),
				"-crf", strconv.Itoa(quality),
				"-b:v", "0",
			}, nil
		},
		extraParams: func(_ *Adapter, _ string, _ int) ([]string, error) {
			return []string{"-row-mt", "1"}, nil
		},
	})

	// VVenC dials -qp and collapses onto five native preset names. Its
	// extra_params emits `-vvenc-params k=v:...` only when a tuning knob is
	// set away from the library default; a default-constructed adapter emits
	// nothing, which is the only shape reachable through encode-profile.
	add(&Adapter{
		Name: "libvvenc", Encoder: "libvvenc", QualityKnob: "qp",
		Presets: cpuUsedPresets,
		codecArgs: func(a *Adapter, preset string, quality int) ([]string, error) {
			p, ok := vvencNativePreset[preset]
			if !ok {
				return nil, unknownPreset(a, preset)
			}
			return []string{
				"-c:v", a.Encoder, "-preset", p, "-qp", strconv.Itoa(quality),
			}, nil
		},
	})

	for _, enc := range []string{"h264_nvenc", "hevc_nvenc", "av1_nvenc"} {
		add(&Adapter{
			Name: enc, Encoder: enc, QualityKnob: "cq",
			Presets: nvencPresets, codecArgs: nvencArgs,
		})
	}

	for _, enc := range []string{"h264_qsv", "hevc_qsv", "av1_qsv"} {
		add(&Adapter{
			Name: enc, Encoder: enc, QualityKnob: "global_quality",
			Presets: qsvPresets, codecArgs: qsvArgs,
		})
	}

	for _, enc := range []string{"h264_amf", "hevc_amf", "av1_amf"} {
		add(&Adapter{
			Name: enc, Encoder: enc, QualityKnob: "qp",
			Presets: amfPresets, codecArgs: amfArgs, extraParams: amfExtraParams,
		})
	}

	for _, enc := range []string{"h264_videotoolbox", "hevc_videotoolbox"} {
		add(&Adapter{
			Name: enc, Encoder: enc, QualityKnob: "q:v",
			Presets: videoToolboxPresets, codecArgs: videoToolboxArgs,
		})
	}

	// av1_videotoolbox is a placeholder: upstream FFmpeg ships no such
	// encoder yet (ADR-0339). CPython gates argv emission on a live
	// `ffmpeg -encoders` probe and raises Av1VideoToolboxUnavailableError
	// when it misses. The Go port keeps the same fail-closed behaviour but
	// routes the probe through AV1VideoToolboxAvailable so a caller that has
	// already enumerated the build's encoders can supply the answer instead
	// of re-shelling out.
	add(&Adapter{
		Name: "av1_videotoolbox", Encoder: "av1_videotoolbox", QualityKnob: "q:v",
		Presets: videoToolboxPresets,
		codecArgs: func(a *Adapter, preset string, quality int) ([]string, error) {
			if !AV1VideoToolboxAvailable() {
				return nil, ErrAV1VideoToolboxUnavailable
			}
			return videoToolboxArgs(a, preset, quality)
		},
	})

	// ProRes has no quality scalar: the harness's quality slot carries the
	// integer tier id, emitted as the named FFmpeg alias.
	add(&Adapter{
		Name: "prores_videotoolbox", Encoder: "prores_videotoolbox", QualityKnob: "profile",
		Presets: videoToolboxPresets,
		codecArgs: func(a *Adapter, preset string, quality int) ([]string, error) {
			rt, ok := vtRealtime[preset]
			if !ok {
				return nil, unknownPreset(a, preset)
			}
			if quality < 0 || quality >= len(proresProfileNames) {
				return nil, fmt.Errorf(
					"unknown ProRes profile %d; expected an integer in [0, %d]",
					quality, len(proresProfileNames)-1)
			}
			return []string{
				"-c:v", a.Encoder,
				"-realtime", strconv.Itoa(rt),
				"-profile:v", proresProfileNames[quality],
			}, nil
		},
	})

	return reg
}

// ErrAV1VideoToolboxUnavailable mirrors Python's
// Av1VideoToolboxUnavailableError: the adapter refuses to invent an argv shape
// for an encoder upstream FFmpeg has not shipped.
var ErrAV1VideoToolboxUnavailable = fmt.Errorf(
	"av1_videotoolbox awaiting upstream FFmpeg encoder support — see ADR-0339")

// AV1VideoToolboxAvailable reports whether this host's FFmpeg advertises the
// av1_videotoolbox encoder. It is a package variable so callers that already
// enumerated `ffmpeg -encoders` (and tests) can substitute their own answer.
// The default is a conservative false: no Go caller has enumerated encoders
// yet, and inventing argv for an encoder that does not exist would produce a
// command that fails deep inside FFmpeg instead of at the CLI boundary.
var AV1VideoToolboxAvailable = func() bool { return false }

// Get returns the adapter registered under name.
func Get(name string) (*Adapter, error) {
	a, ok := registry[name]
	if !ok {
		return nil, fmt.Errorf("unknown codec %q; known codecs: %s",
			name, strings.Join(Known(), ", "))
	}
	return a, nil
}

// Known returns every registered codec name, sorted — the Go equivalent of
// codec_adapters.known_codecs().
func Known() []string {
	out := make([]string, 0, len(registry))
	for k := range registry {
		out = append(out, k)
	}
	sort.Strings(out)
	return out
}

// DefaultPreset picks the preset encoder_profile._default_preset would pick
// for a codec: "medium" when the adapter has it, otherwise the middle element
// of the adapter's preset tuple, and "medium" for an unregistered codec.
func DefaultPreset(codec string) string {
	a, err := Get(codec)
	if err != nil {
		return "medium"
	}
	if len(a.Presets) == 0 {
		return "medium"
	}
	if a.HasPreset("medium") {
		return "medium"
	}
	return a.Presets[len(a.Presets)/2]
}

// LegacyCodecArgs is encode._legacy_codec_args: the historic libx264-shaped
// fallback used when an EncodeRequest names an encoder that is not in the
// registry. Keeping it means an unregistered codec still produces an
// invocable command rather than a hard failure, matching CPython.
func LegacyCodecArgs(encoder, preset string, quality int) []string {
	return []string{"-c:v", encoder, "-preset", preset, "-crf", strconv.Itoa(quality)}
}

// ResolveCodecArgs is the Go port of encode._resolve_codec_args: route through
// the registry when the encoder is known, otherwise fall back to the legacy
// x264-shaped argv.
func ResolveCodecArgs(encoder, preset string, quality int) ([]string, error) {
	a, err := Get(encoder)
	if err != nil {
		return LegacyCodecArgs(encoder, preset, quality), nil
	}
	return a.ResolvedArgs(preset, quality)
}
