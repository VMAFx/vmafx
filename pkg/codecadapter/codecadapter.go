// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

// Package codecadapter is the Go port of tools/vmaf-tune/src/vmaftune/
// codec_adapters/ (ADR-0237 Phase A, ADR-0294 dispatcher, ADR-0326 HP-1).
//
// Every codec exposes a different parameter shape; the search loop must not
// branch on codec identity. Each adapter declares its quality knob, range,
// defaults, and FFmpeg encoder name, plus a mnemonic-preset table that
// normalises the ultrafast..placebo vocabulary onto the encoder's native
// preset axis (NVENC p1..p7, SVT-AV1 0..13, libaom -cpu-used 0..9, ...).
//
// The Python original is nineteen dataclasses across a package; the Go port
// collapses them into one data-driven table because every adapter differs
// only in (a) the flag names, (b) the preset mapping and (c) whether the
// preset token is emitted at all. Behaviour matches the Python argv for
// every registered codec — see codecadapter_test.go, whose golden table is
// transcribed from a live dump of the Python registry.
package codecadapter

import (
	"fmt"
	"sort"
	"strconv"
)

// QualityFlagStyle selects how the adapter emits its quality knob.
type QualityFlagStyle int

const (
	// StyleSingleFlag emits "<flag> <value>" (e.g. "-crf 23", "-cq 23").
	StyleSingleFlag QualityFlagStyle = iota
	// StyleAMFQP emits "-rc cqp -qp_i <v> -qp_p <v>" (AMD AMF family).
	StyleAMFQP
	// StyleProResProfile emits "-profile:v <name>" from a profile table.
	StyleProResProfile
)

// PresetStyle selects how the mnemonic preset maps onto the encoder's own
// preset axis.
type PresetStyle int

const (
	// PresetFlagValue emits "<presetFlag> <mapped>" (e.g. "-preset medium",
	// "-preset p4", "-preset 7", "-cpu-used 5").
	PresetFlagValue PresetStyle = iota
	// PresetAMFQuality emits "-quality <speed|balanced|quality>".
	PresetAMFQuality
	// PresetRealtime emits "-realtime <0|1>" (Apple VideoToolbox).
	PresetRealtime
	// PresetVPXDeadline emits "-deadline good -cpu-used <n>".
	PresetVPXDeadline
)

// Adapter is one codec's frozen contract. Fields mirror the Python
// CodecAdapter Protocol one-for-one.
type Adapter struct {
	Name         string
	Encoder      string
	QualityKnob  string
	QualityRange [2]int
	// QualityDefault is the codec's default quality value.
	QualityDefault int
	// InvertQuality reports whether a higher quality value means lower
	// visual quality (true for every shipped adapter).
	InvertQuality bool
	Presets       []string
	// AdapterVersion bumps when the argv shape / preset list / quality
	// window changes (ADR-0298 cache key).
	AdapterVersion string

	ProbePreset  string
	ProbeQuality int

	SupportsQPFile       bool
	SupportsEncoderStats bool
	SupportsTwoPass      bool

	// quality emission
	qualityStyle QualityFlagStyle
	qualityFlag  string

	// preset emission
	presetStyle PresetStyle
	presetFlag  string
	// presetMap maps a mnemonic preset onto the token emitted after
	// presetFlag. A nil map means the mnemonic passes through verbatim.
	presetMap map[string]string

	// qualityTail is appended immediately after the quality tokens inside
	// FFmpegCodecArgs (libvpx-vp9's "-b:v 0", which pins pure-CRF mode and
	// is part of the codec argv proper, not an extra param).
	qualityTail []string
	// twoPassStyle selects the 2-pass argv shape; see TwoPassStyle.
	twoPassStyle TwoPassStyle

	// extraParams are codec-level flags orthogonal to quality/preset. They
	// are NOT part of FFmpegCodecArgs — the encode driver appends them, the
	// same layering vmaftune.encode._resolve_codec_args uses.
	extraParams []string

	// proresProfiles maps a quality index onto the ProRes profile name.
	proresProfiles []string

	// unavailable, when non-empty, makes every argv call fail with this
	// message (av1_videotoolbox awaits upstream FFmpeg support, ADR-0339).
	unavailable string
}

// swPresets is the nine-name libx264 mnemonic table.
var swPresets = []string{
	"ultrafast", "superfast", "veryfast", "faster", "fast",
	"medium", "slow", "slower", "veryslow",
}

// x265Presets adds "placebo" on top of the x264 nine.
var x265Presets = []string{
	"ultrafast", "superfast", "veryfast", "faster", "fast",
	"medium", "slow", "slower", "veryslow", "placebo",
}

// hwPresets is the ten-name hardware-encoder mnemonic table (NVENC / AMF).
var hwPresets = []string{
	"ultrafast", "superfast", "veryfast", "faster", "fast",
	"medium", "slow", "slower", "slowest", "placebo",
}

// aomPresets is the libaom / VVenC / libvpx slow-to-fast ordering.
var aomPresets = []string{
	"placebo", "slowest", "slower", "slow", "medium",
	"fast", "faster", "veryfast", "superfast", "ultrafast",
}

// qsvPresets is the seven-name Intel QSV table (no ultrafast/superfast).
var qsvPresets = []string{
	"veryslow", "slower", "slow", "medium", "fast", "faster", "veryfast",
}

// svtPresets is SVT-AV1's eight-name table.
var svtPresets = []string{
	"placebo", "slowest", "slower", "slow", "medium", "fast", "faster", "veryfast",
}

// nvencPresetMap collapses the ten mnemonics onto NVENC's p1..p7.
var nvencPresetMap = map[string]string{
	"ultrafast": "p1", "superfast": "p1", "veryfast": "p1",
	"faster": "p2", "fast": "p3", "medium": "p4",
	"slow": "p5", "slower": "p6", "slowest": "p7", "placebo": "p7",
}

// amfQualityMap collapses the ten mnemonics onto AMF's three -quality values.
var amfQualityMap = map[string]string{
	"ultrafast": "speed", "superfast": "speed", "veryfast": "speed",
	"faster": "speed", "fast": "speed", "medium": "balanced",
	"slow": "quality", "slower": "quality", "slowest": "quality",
	"placebo": "quality",
}

// aomCPUUsedMap maps mnemonics onto libaom's -cpu-used 0..9.
var aomCPUUsedMap = map[string]string{
	"placebo": "0", "slowest": "1", "slower": "2", "slow": "3", "medium": "4",
	"fast": "5", "faster": "6", "veryfast": "7", "superfast": "8", "ultrafast": "9",
}

// vpxCPUUsedMap maps mnemonics onto libvpx-vp9's -cpu-used 0..5.
var vpxCPUUsedMap = map[string]string{
	"placebo": "0", "slowest": "0", "slower": "1", "slow": "2", "medium": "3",
	"fast": "4", "faster": "5", "veryfast": "5", "superfast": "5", "ultrafast": "5",
}

// svtPresetMap maps mnemonics onto SVT-AV1's numeric -preset axis.
var svtPresetMap = map[string]string{
	"placebo": "0", "slowest": "1", "slower": "3", "slow": "5",
	"medium": "7", "fast": "9", "faster": "11", "veryfast": "13",
}

// vvencPresetMap maps mnemonics onto VVenC's five named presets.
var vvencPresetMap = map[string]string{
	"placebo": "slower", "slowest": "slower", "slower": "slower",
	"slow": "slow", "medium": "medium", "fast": "fast", "faster": "faster",
	"veryfast": "faster", "superfast": "faster", "ultrafast": "faster",
}

// vtRealtimeMap maps mnemonics onto VideoToolbox's -realtime 0/1 switch.
var vtRealtimeMap = map[string]string{
	"ultrafast": "1", "superfast": "1", "veryfast": "1", "faster": "1", "fast": "1",
	"medium": "0", "slow": "0", "slower": "0", "veryslow": "0",
}

// proresProfileNames indexes ProRes profiles by the quality knob value.
// Tier 5 is "xq", not "4444xq": FFmpeg's prores_videotoolbox -profile:v
// vocabulary names it that way, and the Python adapter emits "xq".
var proresProfileNames = []string{"proxy", "lt", "standard", "hq", "4444", "xq"}

// TwoPassStyle selects how an adapter spells its 2-pass arguments.
type TwoPassStyle int

const (
	// TwoPassGeneric is ffmpeg's -pass N -passlogfile <path> pair, used by
	// libx264, libaom-av1, libvpx-vp9 and libvvenc.
	TwoPassGeneric TwoPassStyle = iota
	// TwoPassX265Params is libx265's -x265-params pass=N:stats=<path>.
	TwoPassX265Params
)

// registry is the frozen adapter table, keyed by FFmpeg encoder name.
var registry = map[string]*Adapter{}

// nvenc builds one NVENC-family adapter.
func nvenc(name string) *Adapter {
	return &Adapter{
		Name: name, Encoder: name, QualityKnob: "cq",
		QualityRange: [2]int{15, 40}, QualityDefault: 23, InvertQuality: true,
		Presets: hwPresets, ProbePreset: "ultrafast", ProbeQuality: 28,
		qualityStyle: StyleSingleFlag, qualityFlag: "-cq",
		presetStyle: PresetFlagValue, presetFlag: "-preset", presetMap: nvencPresetMap,
	}
}

// amf builds one AMD AMF-family adapter.
func amf(name string) *Adapter {
	return &Adapter{
		Name: name, Encoder: name, QualityKnob: "qp",
		QualityRange: [2]int{15, 40}, QualityDefault: 23, InvertQuality: true,
		Presets: hwPresets, ProbePreset: "ultrafast", ProbeQuality: 28,
		qualityStyle: StyleAMFQP,
		presetStyle:  PresetAMFQuality, presetMap: amfQualityMap,
	}
}

// qsvAdapter builds one Intel QSV-family adapter.
func qsvAdapter(name string) *Adapter {
	return &Adapter{
		Name: name, Encoder: name, QualityKnob: "global_quality",
		QualityRange: [2]int{1, 51}, QualityDefault: 23, InvertQuality: true,
		Presets: qsvPresets, ProbePreset: "veryfast", ProbeQuality: 23,
		qualityStyle: StyleSingleFlag, qualityFlag: "-global_quality",
		presetStyle: PresetFlagValue, presetFlag: "-preset",
	}
}

// videotoolbox builds one Apple VideoToolbox adapter.
func videotoolbox(name string) *Adapter {
	return &Adapter{
		Name: name, Encoder: name, QualityKnob: "q:v",
		QualityRange: [2]int{0, 100}, QualityDefault: 50, InvertQuality: false,
		Presets: swPresets, ProbePreset: "ultrafast", ProbeQuality: 60,
		qualityStyle: StyleSingleFlag, qualityFlag: "-q:v",
		presetStyle: PresetRealtime, presetMap: vtRealtimeMap,
	}
}

func init() {
	for _, a := range []*Adapter{
		{
			Name: "libx264", Encoder: "libx264", QualityKnob: "crf",
			QualityRange: [2]int{0, 51}, QualityDefault: 23, InvertQuality: true,
			Presets: swPresets, AdapterVersion: "2",
			ProbePreset: "ultrafast", ProbeQuality: 28,
			SupportsQPFile: true, SupportsEncoderStats: true, SupportsTwoPass: true,
			qualityStyle: StyleSingleFlag, qualityFlag: "-crf",
			presetStyle: PresetFlagValue, presetFlag: "-preset",
		},
		{
			Name: "libx265", Encoder: "libx265", QualityKnob: "crf",
			QualityRange: [2]int{15, 40}, QualityDefault: 28, InvertQuality: true,
			Presets: x265Presets, ProbePreset: "ultrafast", ProbeQuality: 28,
			SupportsEncoderStats: true, SupportsTwoPass: true,
			twoPassStyle: TwoPassX265Params,
			qualityStyle: StyleSingleFlag, qualityFlag: "-crf",
			presetStyle: PresetFlagValue, presetFlag: "-preset",
		},
		{
			Name: "libaom-av1", Encoder: "libaom-av1", QualityKnob: "crf",
			QualityRange: [2]int{0, 63}, QualityDefault: 35, InvertQuality: true,
			Presets: aomPresets, ProbePreset: "ultrafast", ProbeQuality: 35,
			SupportsQPFile: true, SupportsTwoPass: true,
			qualityStyle: StyleSingleFlag, qualityFlag: "-crf",
			presetStyle: PresetFlagValue, presetFlag: "-cpu-used", presetMap: aomCPUUsedMap,
		},
		nvenc("h264_nvenc"), nvenc("hevc_nvenc"), nvenc("av1_nvenc"),
		amf("h264_amf"), amf("hevc_amf"), amf("av1_amf"),
		qsvAdapter("h264_qsv"), qsvAdapter("hevc_qsv"), qsvAdapter("av1_qsv"),
		videotoolbox("h264_videotoolbox"), videotoolbox("hevc_videotoolbox"),
		{
			Name: "prores_videotoolbox", Encoder: "prores_videotoolbox",
			QualityKnob: "profile:v", QualityRange: [2]int{0, 5}, QualityDefault: 3,
			InvertQuality: false, Presets: swPresets,
			ProbePreset: "ultrafast", ProbeQuality: 0,
			qualityStyle: StyleProResProfile, proresProfiles: proresProfileNames,
			presetStyle: PresetRealtime, presetMap: vtRealtimeMap,
		},
		{
			Name: "av1_videotoolbox", Encoder: "av1_videotoolbox", QualityKnob: "q:v",
			QualityRange: [2]int{0, 100}, QualityDefault: 50, InvertQuality: false,
			Presets: swPresets, ProbePreset: "ultrafast", ProbeQuality: 60,
			qualityStyle: StyleSingleFlag, qualityFlag: "-q:v",
			presetStyle: PresetRealtime, presetMap: vtRealtimeMap,
			unavailable: "av1_videotoolbox awaiting upstream FFmpeg encoder support — see ADR-0339",
		},
		{
			Name: "libvvenc", Encoder: "libvvenc", QualityKnob: "qp",
			QualityRange: [2]int{17, 50}, QualityDefault: 32, InvertQuality: true,
			Presets: aomPresets, ProbePreset: "faster", ProbeQuality: 32,
			SupportsTwoPass: true,
			qualityStyle:    StyleSingleFlag, qualityFlag: "-qp",
			presetStyle: PresetFlagValue, presetFlag: "-preset", presetMap: vvencPresetMap,
		},
		{
			Name: "libsvtav1", Encoder: "libsvtav1", QualityKnob: "crf",
			QualityRange: [2]int{20, 50}, QualityDefault: 35, InvertQuality: true,
			Presets: svtPresets, ProbePreset: "veryfast", ProbeQuality: 35,
			qualityStyle: StyleSingleFlag, qualityFlag: "-crf",
			presetStyle: PresetFlagValue, presetFlag: "-preset", presetMap: svtPresetMap,
		},
		{
			Name: "libvpx-vp9", Encoder: "libvpx-vp9", QualityKnob: "crf",
			QualityRange: [2]int{0, 63}, QualityDefault: 32, InvertQuality: true,
			Presets: aomPresets, ProbePreset: "ultrafast", ProbeQuality: 32,
			SupportsTwoPass: true,
			qualityStyle:    StyleSingleFlag, qualityFlag: "-crf",
			presetStyle: PresetVPXDeadline, presetMap: vpxCPUUsedMap,
			qualityTail: []string{"-b:v", "0"},
			extraParams: []string{"-row-mt", "1"},
		},
	} {
		registry[a.Name] = a
	}
}

// Get returns the adapter registered under name.
func Get(name string) (*Adapter, error) {
	a, ok := registry[name]
	if !ok {
		return nil, fmt.Errorf("unknown codec %q; known codecs: %v", name, Known())
	}
	return a, nil
}

// Known returns the sorted list of registered codec names, matching the
// Python known_codecs() ordering that drives the CLI's choices= lists.
func Known() []string {
	out := make([]string, 0, len(registry))
	for k := range registry {
		out = append(out, k)
	}
	sort.Strings(out)
	return out
}

// HasPreset reports whether preset is in the adapter's mnemonic table.
func (a *Adapter) HasPreset(preset string) bool {
	for _, p := range a.Presets {
		if p == preset {
			return true
		}
	}
	return false
}

// Validate mirrors the Python adapter's validate(): unknown preset or an
// out-of-range quality value is an error.
func (a *Adapter) Validate(preset string, quality int) error {
	if !a.HasPreset(preset) {
		return fmt.Errorf("unknown %s preset %q; expected one of %v", a.Name, preset, a.Presets)
	}
	if quality < a.QualityRange[0] || quality > a.QualityRange[1] {
		return fmt.Errorf("quality %d outside range [%d, %d]",
			quality, a.QualityRange[0], a.QualityRange[1])
	}
	return nil
}

// presetTokens returns the argv slice that pins the encoder preset.
func (a *Adapter) presetTokens(preset string) []string {
	mapped := preset
	if a.presetMap != nil {
		if v, ok := a.presetMap[preset]; ok {
			mapped = v
		}
	}
	switch a.presetStyle {
	case PresetAMFQuality:
		return []string{"-quality", mapped}
	case PresetRealtime:
		return []string{"-realtime", mapped}
	case PresetVPXDeadline:
		return []string{"-deadline", "good", "-cpu-used", mapped}
	case PresetFlagValue:
		fallthrough
	default:
		return []string{a.presetFlag, mapped}
	}
}

// qualityTokens returns the argv slice that pins the encoder quality knob.
func (a *Adapter) qualityTokens(quality int) []string {
	switch a.qualityStyle {
	case StyleAMFQP:
		q := strconv.Itoa(quality)
		return []string{"-rc", "cqp", "-qp_i", q, "-qp_p", q}
	case StyleProResProfile:
		idx := quality
		if idx < 0 || idx >= len(a.proresProfiles) {
			idx = a.QualityDefault
		}
		return []string{"-profile:v", a.proresProfiles[idx]}
	case StyleSingleFlag:
		fallthrough
	default:
		return []string{a.qualityFlag, strconv.Itoa(quality)}
	}
}

// FFmpegCodecArgs returns the "-c:v ..." argv slice for one encode. It is the
// exact analogue of the Python adapter's ffmpeg_codec_args(preset, quality)
// and does NOT include ExtraParams — see ResolveCodecArgs for the combined
// slice the encode driver actually uses.
func (a *Adapter) FFmpegCodecArgs(preset string, quality int) ([]string, error) {
	if a.unavailable != "" {
		return nil, fmt.Errorf("%s", a.unavailable)
	}
	out := []string{"-c:v", a.Encoder}
	out = append(out, a.presetTokens(preset)...)
	out = append(out, a.qualityTokens(quality)...)
	out = append(out, a.qualityTail...)
	return out, nil
}

// ExtraParams returns codec-level flags orthogonal to quality and preset
// (libvpx-vp9's "-row-mt 1"; empty for every other shipped codec).
func (a *Adapter) ExtraParams() []string {
	if len(a.extraParams) == 0 {
		return nil
	}
	out := make([]string, len(a.extraParams))
	copy(out, a.extraParams)
	return out
}

// ResolveCodecArgs returns FFmpegCodecArgs followed by ExtraParams — the
// full codec argv slice the encode driver splices into the ffmpeg command.
// Mirrors vmaftune.encode._resolve_codec_args.
//
// Deliberate deviation from the Python original: the three AMF adapters'
// extra_params(preset, qp) returns the same tokens ffmpeg_codec_args already
// produced, so _resolve_codec_args emits "-quality balanced -rc cqp -qp_i 23
// -qp_p 23" twice for every AMF encode. FFmpeg takes the last occurrence so
// the encode is unaffected, but every logged command line carries the
// duplicate. The Go port emits each AMF token once; every other codec's argv
// is byte-identical to the Python original.
func (a *Adapter) ResolveCodecArgs(preset string, quality int) ([]string, error) {
	args, err := a.FFmpegCodecArgs(preset, quality)
	if err != nil {
		return nil, err
	}
	return append(args, a.extraParams...), nil
}

// ProbeArgs returns the fast probe-encode argv used by the per-shot
// predictor's complexity barometer.
func (a *Adapter) ProbeArgs() ([]string, error) {
	return a.FFmpegCodecArgs(a.ProbePreset, a.ProbeQuality)
}

// GOPArgs pins the GOP / keyint. minKeyint <= 0 omits -keyint_min.
func (a *Adapter) GOPArgs(keyint, minKeyint int) []string {
	if keyint <= 0 {
		return nil
	}
	out := []string{"-g", strconv.Itoa(keyint)}
	if minKeyint > 0 {
		out = append(out, "-keyint_min", strconv.Itoa(minKeyint))
	}
	return out
}

// TwoPassArgs returns the FFmpeg argv for the Nth pass of a 2-pass encode.
// passNumber 0 is single-pass and returns nil.
func (a *Adapter) TwoPassArgs(passNumber int, statsPath string) ([]string, error) {
	if passNumber == 0 {
		return nil, nil
	}
	if !a.SupportsTwoPass {
		return nil, fmt.Errorf("encoder %q does not support 2-pass encoding", a.Name)
	}
	if passNumber != 1 && passNumber != 2 {
		return nil, fmt.Errorf("%s two_pass_args: pass_number must be 1 or 2, got %d",
			a.Name, passNumber)
	}
	// libx265 carries its 2-pass state inside -x265-params; ffmpeg's generic
	// -pass/-passlogfile pair does not reach x265's internal rate control, so
	// emitting it produces a first pass whose stats the second pass ignores.
	// Mirrors codec_adapters/x265.py::two_pass_args.
	if a.twoPassStyle == TwoPassX265Params {
		return []string{"-x265-params",
			fmt.Sprintf("pass=%d:stats=%s", passNumber, statsPath)}, nil
	}
	return []string{"-pass", strconv.Itoa(passNumber), "-passlogfile", statsPath}, nil
}

// LegacyCodecArgs is encode._legacy_codec_args: the historic libx264-shaped
// fallback used when a request names an encoder that is not in the registry.
// Keeping it means an unregistered codec still produces an invocable command
// rather than a hard failure, matching CPython.
func LegacyCodecArgs(encoder, preset string, quality int) []string {
	return []string{"-c:v", encoder, "-preset", preset, "-crf", strconv.Itoa(quality)}
}

// ResolveCodecArgs resolves by encoder name, falling back to LegacyCodecArgs
// for an unregistered encoder.
//
// This is the package-level form of (*Adapter).ResolveCodecArgs, kept because
// callers that only hold an encoder string would otherwise repeat the
// Get-then-fall-back dance at every call site.
func ResolveCodecArgs(encoder, preset string, quality int) ([]string, error) {
	a, err := Get(encoder)
	if err != nil {
		return LegacyCodecArgs(encoder, preset, quality), nil
	}
	// Validate before building. The (*Adapter) method deliberately does not
	// validate — it is the low-level token builder — but this package-level
	// entry point carries the Python contract, where an out-of-vocabulary
	// preset is an error rather than something spliced into the argv.
	if err := a.Validate(preset, quality); err != nil {
		return nil, err
	}
	return a.ResolveCodecArgs(preset, quality)
}

// DefaultPreset is encoder_profile._default_preset: "medium" when the codec
// offers it (or is unknown), otherwise the middle of its preset ladder.
func DefaultPreset(codec string) string {
	a, err := Get(codec)
	if err != nil || len(a.Presets) == 0 || a.HasPreset("medium") {
		return "medium"
	}
	return a.Presets[len(a.Presets)/2]
}
