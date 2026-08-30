// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// pkg/encoder/adapter.go — codec-adapter policy table.
//
// Go port of the codec-adapter contract in
// tools/vmaf-tune/src/vmaftune/codec_adapters/ (ADR-0237 Phase A,
// ADR-0294 dispatcher, ADR-0297 / HP-1 argv delegation).
//
// The Encoder interface answers "how do I run one encode?"; the Adapter
// answers "what argv shape, preset vocabulary and quality window does this
// codec use?". The per-shot tuner (pkg/pershot) needs the second question
// answered for two reasons the plain Encoder interface cannot cover:
//
//  1. The emitted FFmpeg encoding plan carries a per-segment argv slice, and
//     that slice is codec-specific — "-crf" for x264/x265/AV1 software,
//     "-cq" for NVENC, "-global_quality" for QSV, "-rc cqp -qp_i N -qp_p N"
//     for AMF, "-cpu-used N" for libaom.  Emitting the x264 shape for every
//     codec (the pre-HP-1 behaviour) produces a plan that does not run.
//  2. The per-shot recommendation clamps the bisect's chosen CRF into the
//     codec's *informative* quality window, which differs from the absolute
//     window the bisect searches (ADR-0538).
//
// Scope: the ten codecs pkg/encoder can actually construct via NewExtended.
// The Python registry carries seven more (av1_nvenc, av1_qsv, av1_amf, the
// four VideoToolbox adapters, libvvenc, libvpx-vp9); those have no Go
// Encoder implementation yet, so Adapter() rejects them rather than
// emitting an argv slice no Go code path can execute.
package encoder

import (
	"fmt"
	"sort"
	"strconv"
)

// QualityKnob names the FFmpeg flag a codec uses for constant-quality mode.
// Mirrors the Python adapter's quality_knob field.
type QualityKnob string

// Quality-knob constants, one per FFmpeg rate-control flag family.
const (
	KnobCRF           QualityKnob = "crf"
	KnobCQ            QualityKnob = "cq"
	KnobGlobalQuality QualityKnob = "global_quality"
	KnobQP            QualityKnob = "qp"
)

// presetStyle selects how a canonical mnemonic preset name is rendered into
// the codec's own preset vocabulary.
type presetStyle int

const (
	// presetVerbatim passes the mnemonic through unchanged (x264, x265, QSV).
	presetVerbatim presetStyle = iota
	// presetNVENC collapses the mnemonic onto NVENC's seven "pN" levels.
	presetNVENC
	// presetSVTAV1 maps the mnemonic onto SVT-AV1's integer preset ladder.
	presetSVTAV1
	// presetLibaomCPUUsed maps the mnemonic onto libaom's "-cpu-used" integer.
	presetLibaomCPUUsed
	// presetAMFQuality collapses the mnemonic onto AMF's three quality rungs.
	presetAMFQuality
)

// Adapter is the codec policy object: preset vocabulary, quality windows and
// the FFmpeg argv shape for one constant-quality encode.
//
// Field semantics mirror the Python CodecAdapter Protocol one-for-one so the
// two implementations can be diffed by eye during the migration.
type Adapter struct {
	// Name is the registry key and the FFmpeg encoder name (they coincide
	// for every codec in this table).
	Name string

	// Knob is the FFmpeg constant-quality flag this codec uses.
	Knob QualityKnob

	// Presets is the codec's accepted mnemonic preset vocabulary, ordered as
	// the Python adapter declares it.
	Presets []string

	// QualityLo / QualityHi bound the perceptually-informative quality
	// window (the Python adapter's quality_range). The per-shot tuner clamps
	// each recommended CRF into this window.
	QualityLo, QualityHi int

	// AbsoluteLo / AbsoluteHi bound the encoder's full accepted quality range
	// (the Python bisect's _absolute_crf_range). The CRF bisect searches this
	// wider window so high-VMAF targets stay reachable (ADR-0538).
	AbsoluteLo, AbsoluteHi int

	// QualityDefault is the codec's default quality value, used by the
	// library-level dry-run predicate when no bisect is wired.
	QualityDefault int

	// style selects the preset-token rendering for CodecArgs.
	style presetStyle
}

// nvencPresetMap collapses the ten mnemonic preset names onto NVENC's seven
// hardware levels. Mirrors _nvenc_common._NVENC_PRESET_MAP verbatim.
var nvencPresetMap = map[string]string{
	"ultrafast": "p1",
	"superfast": "p1",
	"veryfast":  "p1",
	"faster":    "p2",
	"fast":      "p3",
	"medium":    "p4",
	"slow":      "p5",
	"slower":    "p6",
	"slowest":   "p7",
	"placebo":   "p7",
}

// svtav1PresetMap maps mnemonic preset names onto SVT-AV1's integer ladder.
// Mirrors svtav1.PRESET_NAME_TO_INT verbatim.
var svtav1PresetMap = map[string]int{
	"placebo":  0,
	"slowest":  1,
	"slower":   3,
	"slow":     5,
	"medium":   7,
	"fast":     9,
	"faster":   11,
	"veryfast": 13,
}

// libaomCPUUsedMap maps mnemonic preset names onto libaom's -cpu-used
// integer. Mirrors libaom._PRESET_CPU_USED verbatim.
var libaomCPUUsedMap = map[string]int{
	"placebo":   0,
	"slowest":   1,
	"slower":    2,
	"slow":      3,
	"medium":    4,
	"fast":      5,
	"faster":    6,
	"veryfast":  7,
	"superfast": 8,
	"ultrafast": 9,
}

// amfQualityMap collapses the ten mnemonic preset names onto AMF's three
// quality rungs. Mirrors _amf_common._PRESET_TO_AMF verbatim.
var amfQualityMap = map[string]string{
	"placebo":   "quality",
	"slowest":   "quality",
	"slower":    "quality",
	"slow":      "quality",
	"medium":    "balanced",
	"fast":      "speed",
	"faster":    "speed",
	"veryfast":  "speed",
	"superfast": "speed",
	"ultrafast": "speed",
}

// Canonical preset vocabularies, shared by the adapters that declare them.
var (
	x264Presets = []string{
		"ultrafast", "superfast", "veryfast", "faster", "fast",
		"medium", "slow", "slower", "veryslow",
	}
	x265Presets = []string{
		"ultrafast", "superfast", "veryfast", "faster", "fast",
		"medium", "slow", "slower", "veryslow", "placebo",
	}
	nvencPresets = []string{
		"ultrafast", "superfast", "veryfast", "faster", "fast",
		"medium", "slow", "slower", "slowest", "placebo",
	}
	qsvPresets = []string{
		"veryslow", "slower", "slow", "medium", "fast", "faster", "veryfast",
	}
	amfPresets = []string{
		"ultrafast", "superfast", "veryfast", "faster", "fast",
		"medium", "slow", "slower", "slowest", "placebo",
	}
	svtav1Presets = []string{
		"placebo", "slowest", "slower", "slow", "medium", "fast",
		"faster", "veryfast",
	}
	libaomPresets = []string{
		"placebo", "slowest", "slower", "slow", "medium", "fast",
		"faster", "veryfast", "superfast", "ultrafast",
	}
)

// adapterRegistry is the codec-adapter table. Keys match NewExtended's
// accepted codec names exactly.
//
// Quality windows are transcribed from the Python adapters:
//
//	libx264      quality_range (0, 51)   default 23   absolute (0, 51)
//	libx265      quality_range (15, 40)  default 28   absolute (0, 51)
//	h264/hevc_nvenc  (15, 40)            default 23   absolute (15, 40)
//	h264/hevc_qsv    (1, 51)             default 23   absolute (1, 51)
//	h264/hevc_amf    (15, 40)            default 23   absolute (15, 40)
//	libsvtav1    quality_range (20, 50)  default 35   absolute (0, 63)
//	libaom-av1   quality_range (0, 63)   default 35   absolute (0, 63)
//
// The absolute windows follow bisect._absolute_crf_range: the curated
// _ABSOLUTE_CRF_RANGE_BY_NAME table for the software codecs, the adapter's
// own crf_min/crf_max for libsvtav1, and quality_range as the fallback for
// every hardware codec (which declares neither).
var adapterRegistry = map[string]Adapter{
	"libx264": {
		Name: "libx264", Knob: KnobCRF, Presets: x264Presets,
		QualityLo: 0, QualityHi: 51, AbsoluteLo: 0, AbsoluteHi: 51,
		QualityDefault: 23, style: presetVerbatim,
	},
	"libx265": {
		Name: "libx265", Knob: KnobCRF, Presets: x265Presets,
		QualityLo: 15, QualityHi: 40, AbsoluteLo: 0, AbsoluteHi: 51,
		QualityDefault: 28, style: presetVerbatim,
	},
	"h264_nvenc": {
		Name: "h264_nvenc", Knob: KnobCQ, Presets: nvencPresets,
		QualityLo: 15, QualityHi: 40, AbsoluteLo: 15, AbsoluteHi: 40,
		QualityDefault: 23, style: presetNVENC,
	},
	"hevc_nvenc": {
		Name: "hevc_nvenc", Knob: KnobCQ, Presets: nvencPresets,
		QualityLo: 15, QualityHi: 40, AbsoluteLo: 15, AbsoluteHi: 40,
		QualityDefault: 23, style: presetNVENC,
	},
	"h264_qsv": {
		Name: "h264_qsv", Knob: KnobGlobalQuality, Presets: qsvPresets,
		QualityLo: 1, QualityHi: 51, AbsoluteLo: 1, AbsoluteHi: 51,
		QualityDefault: 23, style: presetVerbatim,
	},
	"hevc_qsv": {
		Name: "hevc_qsv", Knob: KnobGlobalQuality, Presets: qsvPresets,
		QualityLo: 1, QualityHi: 51, AbsoluteLo: 1, AbsoluteHi: 51,
		QualityDefault: 23, style: presetVerbatim,
	},
	"h264_amf": {
		Name: "h264_amf", Knob: KnobQP, Presets: amfPresets,
		QualityLo: 15, QualityHi: 40, AbsoluteLo: 15, AbsoluteHi: 40,
		QualityDefault: 23, style: presetAMFQuality,
	},
	"hevc_amf": {
		Name: "hevc_amf", Knob: KnobQP, Presets: amfPresets,
		QualityLo: 15, QualityHi: 40, AbsoluteLo: 15, AbsoluteHi: 40,
		QualityDefault: 23, style: presetAMFQuality,
	},
	"libsvtav1": {
		Name: "libsvtav1", Knob: KnobCRF, Presets: svtav1Presets,
		QualityLo: 20, QualityHi: 50, AbsoluteLo: 0, AbsoluteHi: 63,
		QualityDefault: 35, style: presetSVTAV1,
	},
	"libaom-av1": {
		Name: "libaom-av1", Knob: KnobCRF, Presets: libaomPresets,
		QualityLo: 0, QualityHi: 63, AbsoluteLo: 0, AbsoluteHi: 63,
		QualityDefault: 35, style: presetLibaomCPUUsed,
	},
}

// KnownAdapters returns the codec names carried by the adapter table, sorted.
// Mirrors the Python codec_adapters.known_codecs() ordering (sorted), but
// covers only the codecs pkg/encoder can construct.
func KnownAdapters() []string {
	out := make([]string, 0, len(adapterRegistry))
	for name := range adapterRegistry {
		out = append(out, name)
	}
	sort.Strings(out)
	return out
}

// GetAdapter returns the codec adapter for name.
//
// The error message lists the supported codecs and names the Python fallback
// for the seven registry entries with no Go Encoder implementation, so an
// operator hitting the gap is told where to go rather than left guessing.
func GetAdapter(name string) (Adapter, error) {
	a, ok := adapterRegistry[name]
	if !ok {
		return Adapter{}, fmt.Errorf(
			"unknown codec %q (supported: %v); codecs in the Python adapter "+
				"registry without a Go encoder (av1_nvenc, av1_qsv, av1_amf, "+
				"h264/hevc/av1/prores_videotoolbox, libvvenc, libvpx-vp9) still "+
				"require the Python vmaf-tune binary",
			name, KnownAdapters())
	}
	return a, nil
}

// HasPreset reports whether preset is in the adapter's vocabulary.
func (a Adapter) HasPreset(preset string) bool {
	for _, p := range a.Presets {
		if p == preset {
			return true
		}
	}
	return false
}

// DefaultPreset returns the adapter's canonical mid-range preset.
//
// Mirrors bisect._default_preset: prefer "medium" when the adapter advertises
// it, otherwise the middle entry of the preset tuple. Every adapter in this
// table declares "medium", but the fallback is kept so a future adapter
// without it degrades the same way the Python does.
func (a Adapter) DefaultPreset() string {
	if len(a.Presets) == 0 {
		return "medium"
	}
	if a.HasPreset("medium") {
		return "medium"
	}
	return a.Presets[len(a.Presets)/2]
}

// SegmentPreset returns the preset the per-shot encoding plan uses for its
// segment encodes.
//
// Mirrors per_shot._default_segment_preset, which differs from
// DefaultPreset in its fallback: it takes the *first* declared preset rather
// than the middle one. The distinction is preserved so a future adapter
// without "medium" produces the same plan the Python would.
func (a Adapter) SegmentPreset() string {
	if len(a.Presets) == 0 {
		return "medium"
	}
	if a.HasPreset("medium") {
		return "medium"
	}
	return a.Presets[0]
}

// ClampQuality clamps q into the adapter's informative quality window.
// Mirrors per_shot.tune_per_shot's max(lo, min(hi, crf)) clamp.
func (a Adapter) ClampQuality(q int) int {
	if q < a.QualityLo {
		return a.QualityLo
	}
	if q > a.QualityHi {
		return a.QualityHi
	}
	return q
}

// Validate reports whether (preset, quality) is accepted by this codec.
// Mirrors the Python adapter's validate() contract: an unknown preset name
// or an out-of-informative-window quality value is an error.
func (a Adapter) Validate(preset string, quality int) error {
	if !a.HasPreset(preset) {
		return fmt.Errorf("unknown %s preset %q; expected one of %v",
			a.Name, preset, a.Presets)
	}
	if quality < a.QualityLo || quality > a.QualityHi {
		return fmt.Errorf("quality %d outside %s range [%d, %d]",
			quality, a.Name, a.QualityLo, a.QualityHi)
	}
	return nil
}

// presetToken renders a canonical mnemonic preset into the codec's own
// preset vocabulary. Returns an error for names the codec does not accept.
func (a Adapter) presetToken(preset string) (string, error) {
	if !a.HasPreset(preset) {
		return "", fmt.Errorf("unknown %s preset %q; expected one of %v",
			a.Name, preset, a.Presets)
	}
	switch a.style {
	case presetNVENC:
		tok, ok := nvencPresetMap[preset]
		if !ok {
			return "", fmt.Errorf("no NVENC preset mapping for %q", preset)
		}
		return tok, nil
	case presetSVTAV1:
		n, ok := svtav1PresetMap[preset]
		if !ok {
			return "", fmt.Errorf("no SVT-AV1 preset mapping for %q", preset)
		}
		return strconv.Itoa(n), nil
	case presetLibaomCPUUsed:
		n, ok := libaomCPUUsedMap[preset]
		if !ok {
			return "", fmt.Errorf("no libaom cpu-used mapping for %q", preset)
		}
		return strconv.Itoa(n), nil
	case presetAMFQuality:
		tok, ok := amfQualityMap[preset]
		if !ok {
			return "", fmt.Errorf("no AMF quality mapping for %q", preset)
		}
		return tok, nil
	case presetVerbatim:
		return preset, nil
	default:
		return preset, nil
	}
}

// CodecArgs returns the FFmpeg argv slice from "-c:v" onwards for one
// constant-quality encode at (preset, quality).
//
// Byte-for-byte equivalent to the Python adapter's ffmpeg_codec_args:
//
//	libx264 / libx265   -c:v NAME -preset P      -crf Q
//	libsvtav1           -c:v NAME -preset <int>  -crf Q
//	libaom-av1          -c:v NAME -cpu-used <int> -crf Q
//	NVENC               -c:v NAME -preset pN     -cq Q
//	QSV                 -c:v NAME -preset P      -global_quality Q
//	AMF                 -c:v NAME -quality R -rc cqp -qp_i Q -qp_p Q
//
// quality is emitted verbatim; callers that need it clamped call
// ClampQuality first (the per-shot planner does).
func (a Adapter) CodecArgs(preset string, quality int) ([]string, error) {
	tok, err := a.presetToken(preset)
	if err != nil {
		return nil, err
	}
	q := strconv.Itoa(quality)
	switch a.Knob {
	case KnobCRF:
		flag := "-preset"
		if a.style == presetLibaomCPUUsed {
			flag = "-cpu-used"
		}
		return []string{"-c:v", a.Name, flag, tok, "-crf", q}, nil
	case KnobCQ:
		return []string{"-c:v", a.Name, "-preset", tok, "-cq", q}, nil
	case KnobGlobalQuality:
		return []string{"-c:v", a.Name, "-preset", tok, "-global_quality", q}, nil
	case KnobQP:
		return []string{
			"-c:v", a.Name,
			"-quality", tok,
			"-rc", "cqp",
			"-qp_i", q,
			"-qp_p", q,
		}, nil
	default:
		return nil, fmt.Errorf("codec %q has no quality-knob mapping", a.Name)
	}
}

// AdapterEncoder is an Encoder that renders its ffmpeg argv through an
// Adapter instead of the fixed "-c:v NAME <flag> N" triple used by the
// built-in encoder types.
//
// It exists so the per-shot bisect encodes with the same codec argv shape
// (preset token included) that the emitted encoding plan will use, and so it
// can carry the pre-input demuxer options a raw-YUV shot reference needs.
type AdapterEncoder struct {
	adapter   Adapter
	preset    string
	inputArgs []string
}

// NewAdapterEncoder builds an AdapterEncoder for codec at preset.
//
// An empty preset selects the adapter's DefaultPreset (mirroring the Python
// bisect, where preset=None picks the adapter's mid-range default).
// inputArgs are prepended to every encode's ffmpeg argv before "-i"; pass the
// raw-video quartet for headerless YUV sources, or nil for containers.
func NewAdapterEncoder(codec, preset string, inputArgs []string) (AdapterEncoder, error) {
	a, err := GetAdapter(codec)
	if err != nil {
		return AdapterEncoder{}, err
	}
	if preset == "" {
		preset = a.DefaultPreset()
	}
	if !a.HasPreset(preset) {
		return AdapterEncoder{}, fmt.Errorf(
			"unknown %s preset %q; expected one of %v", a.Name, preset, a.Presets)
	}
	args := make([]string, len(inputArgs))
	copy(args, inputArgs)
	return AdapterEncoder{adapter: a, preset: preset, inputArgs: args}, nil
}

// Name returns the FFmpeg encoder name.
func (e AdapterEncoder) Name() string { return e.adapter.Name }

// Preset returns the resolved preset name used for every encode.
func (e AdapterEncoder) Preset() string { return e.preset }

// Adapter returns the codec adapter backing this encoder.
func (e AdapterEncoder) Adapter() Adapter { return e.adapter }

// CRFRange returns the adapter's *absolute* quality window — the search
// domain the CRF bisect walks (ADR-0538), not the narrower informative
// window used to clamp the final recommendation.
func (e AdapterEncoder) CRFRange() (int, int) {
	return e.adapter.AbsoluteLo, e.adapter.AbsoluteHi
}

// Encode encodes src at params.CRF using the adapter's codec argv shape.
func (e AdapterEncoder) Encode(src string, params EncodeParams) (EncodeResult, error) {
	codecArgs, err := e.adapter.CodecArgs(e.preset, params.CRF)
	if err != nil {
		return EncodeResult{}, err
	}
	params.InputArgs = append(append([]string{}, e.inputArgs...), params.InputArgs...)
	return runEncodeArgv(src, params, e.adapter.Name, codecArgs)
}

// Compile-time assertion that AdapterEncoder satisfies Encoder.
var _ Encoder = AdapterEncoder{}
