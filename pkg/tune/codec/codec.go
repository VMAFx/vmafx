// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent

// Package codec is the Go port of vmaftune.codec_adapters — the per-encoder
// metadata registry the tuning harness consults instead of branching on codec
// identity (ADR-0237).
//
// Every adapter declares its quality knob and window, its default and
// probe-encode quality, its mnemonic preset vocabulary, and whether it
// supports two-pass encoding. The auto planner reads QualityRange /
// QualityDefault (CRF inversion), ProbeQuality (bitrate estimation), and
// SupportsTwoPass (short-circuit #10); the executor reads FFmpegCodecArgs and
// ExtraParams to build an ffmpeg argv.
//
// The tables here are transcribed from the Python adapters and pinned by
// TestParityWithPythonAdapters-style table tests. Adding a codec means adding
// one entry — never touching a caller.
package codec

import (
	"fmt"
	"sort"
	"strconv"
)

// ErrUnavailable marks an adapter that is registered (so its metadata is
// queryable) but cannot currently emit an ffmpeg argv — av1_videotoolbox is
// awaiting upstream FFmpeg encoder support per ADR-0339.
type ErrUnavailable struct {
	Codec  string
	Reason string
}

func (e *ErrUnavailable) Error() string {
	return fmt.Sprintf("%s %s", e.Codec, e.Reason)
}

// Adapter is the per-codec contract. It mirrors the CodecAdapter Protocol in
// vmaftune/codec_adapters/__init__.py; the Python method surface collapses to
// two closures here because Go has no structural typing over data fields.
type Adapter struct {
	// Name is the registry key and the ffmpeg encoder name (they coincide
	// for every shipped adapter).
	Name string
	// Encoder is the ffmpeg "-c:v" value.
	Encoder string
	// QualityKnob names the rate-control parameter (crf / cq / qp /
	// global_quality / q:v / profile:v) for diagnostics.
	QualityKnob string
	// QualityLo / QualityHi bound the search window used by PickCRF.
	QualityLo int
	QualityHi int
	// QualityDefault is the codec's neutral quality setting.
	QualityDefault int
	// InvertQuality is true when a higher knob value means lower quality
	// (every CRF/QP-style encoder). VideoToolbox's -q:v runs the other way.
	InvertQuality bool
	// Presets is the mnemonic preset vocabulary, ordered fastest-first for
	// most adapters (libaom / libsvtav1 / libvvenc list slowest-first, as
	// their Python adapters do).
	Presets []string
	// ProbePreset / ProbeQuality are the knobs the per-shot predictor's
	// probe encode uses as a complexity barometer.
	ProbePreset  string
	ProbeQuality int
	// SupportsTwoPass gates the two-pass calibration stage (ADR-0333).
	SupportsTwoPass bool

	// codecArgs renders the "-c:v ..." argv slice for one encode.
	codecArgs func(a *Adapter, preset string, quality int) ([]string, error)
	// extraArgs renders any non-codec argv tail (e.g. libvpx's -row-mt).
	extraArgs func(a *Adapter, preset string, quality int) []string
}

// FFmpegCodecArgs returns the "-c:v ..." argv slice for one encode.
//
// Like the Python adapters, this is lenient about out-of-vocabulary presets
// and out-of-window quality values: adapters with a preset lookup table fall
// back to their nearest documented entry, and adapters that pass the preset
// through emit it verbatim. Validate is the strict gate.
func (a Adapter) FFmpegCodecArgs(preset string, quality int) ([]string, error) {
	if a.codecArgs == nil {
		return nil, &ErrUnavailable{Codec: a.Name, Reason: "has no ffmpeg argv mapping"}
	}
	return a.codecArgs(&a, preset, quality)
}

// ExtraParams returns additional non-codec argv appended after the codec
// slice. Empty for every adapter except libvpx-vp9 and the AMF family.
func (a Adapter) ExtraParams(preset string, quality int) []string {
	if a.extraArgs == nil {
		return nil
	}
	return a.extraArgs(&a, preset, quality)
}

// Validate reports whether (preset, quality) is inside the adapter's declared
// vocabulary and window.
func (a Adapter) Validate(preset string, quality int) error {
	found := false
	for _, p := range a.Presets {
		if p == preset {
			found = true
			break
		}
	}
	if !found {
		return fmt.Errorf("unknown %s preset %q; expected one of %v", a.Name, preset, a.Presets)
	}
	if quality < a.QualityLo || quality > a.QualityHi {
		return fmt.Errorf("%s %d outside range [%d, %d]",
			a.QualityKnob, quality, a.QualityLo, a.QualityHi)
	}
	return nil
}

// registry is keyed by adapter name. Package-level and read-only after init.
var registry = map[string]Adapter{}

// Get returns the adapter registered under name.
func Get(name string) (Adapter, error) {
	a, ok := registry[name]
	if !ok {
		return Adapter{}, fmt.Errorf("unknown codec %q; known: %v", name, Known())
	}
	return a, nil
}

// Known returns every registered codec name, sorted — the same ordering
// vmaftune.codec_adapters.known_codecs() produces.
func Known() []string {
	out := make([]string, 0, len(registry))
	for name := range registry {
		out = append(out, name)
	}
	sort.Strings(out)
	return out
}

// ---------------------------------------------------------------------------
// Preset vocabularies.
// ---------------------------------------------------------------------------

var (
	// tenPresets is the AMF / NVENC mnemonic ladder (fastest-first).
	tenPresets = []string{
		"ultrafast", "superfast", "veryfast", "faster", "fast",
		"medium", "slow", "slower", "slowest", "placebo",
	}
	x264Presets = []string{
		"ultrafast", "superfast", "veryfast", "faster", "fast",
		"medium", "slow", "slower", "veryslow",
	}
	x265Presets = []string{
		"ultrafast", "superfast", "veryfast", "faster", "fast",
		"medium", "slow", "slower", "veryslow", "placebo",
	}
	qsvPresets = []string{
		"veryslow", "slower", "slow", "medium", "fast", "faster", "veryfast",
	}
	videoToolboxPresets = []string{
		"ultrafast", "superfast", "veryfast", "faster", "fast",
		"medium", "slow", "slower", "veryslow",
	}
	// libaomPresets / svtPresets / vvencPresets are declared slowest-first
	// by their Python adapters; the ordering is user-visible via --help.
	libaomPresets = []string{
		"placebo", "slowest", "slower", "slow", "medium",
		"fast", "faster", "veryfast", "superfast", "ultrafast",
	}
	svtPresets = []string{
		"placebo", "slowest", "slower", "slow", "medium", "fast", "faster", "veryfast",
	}
	vvencPresets = []string{
		"placebo", "slowest", "slower", "slow", "medium",
		"fast", "faster", "veryfast", "superfast", "ultrafast",
	}
	libvpxPresets = []string{
		"placebo", "slowest", "slower", "slow", "medium",
		"fast", "faster", "veryfast", "superfast", "ultrafast",
	}
)

// ---------------------------------------------------------------------------
// Preset -> encoder-native knob maps.
// ---------------------------------------------------------------------------

// nvencPresetMap collapses the ten mnemonics onto NVENC's seven p1..p7
// hardware presets (see _nvenc_common).
var nvencPresetMap = map[string]string{
	"ultrafast": "p1", "superfast": "p1", "veryfast": "p1",
	"faster": "p2", "fast": "p3", "medium": "p4",
	"slow": "p5", "slower": "p6", "slowest": "p7", "placebo": "p7",
}

// amfQualityMap collapses the ten mnemonics onto AMF's three -quality modes.
var amfQualityMap = map[string]string{
	"ultrafast": "speed", "superfast": "speed", "veryfast": "speed",
	"faster": "speed", "fast": "speed", "medium": "balanced",
	"slow": "quality", "slower": "quality", "slowest": "quality", "placebo": "quality",
}

// libaomCPUUsedMap maps the mnemonic ladder onto libaom's -cpu-used 0..9.
var libaomCPUUsedMap = map[string]int{
	"placebo": 0, "slowest": 1, "slower": 2, "slow": 3, "medium": 4,
	"fast": 5, "faster": 6, "veryfast": 7, "superfast": 8, "ultrafast": 9,
}

// svtPresetMap maps the mnemonic ladder onto SVT-AV1's numeric -preset.
var svtPresetMap = map[string]int{
	"placebo": 0, "slowest": 1, "slower": 3, "slow": 5,
	"medium": 7, "fast": 9, "faster": 11, "veryfast": 13,
}

// libvpxCPUUsedMap maps the mnemonic ladder onto libvpx-vp9's -cpu-used 0..5.
var libvpxCPUUsedMap = map[string]int{
	"placebo": 0, "slowest": 0, "slower": 1, "slow": 2, "medium": 3,
	"fast": 4, "faster": 5, "veryfast": 5, "superfast": 5, "ultrafast": 5,
}

// vvencPresetMap collapses the mnemonic ladder onto VVenC's five presets.
var vvencPresetMap = map[string]string{
	"placebo": "slower", "slowest": "slower", "slower": "slower",
	"slow": "slow", "medium": "medium", "fast": "fast",
	"faster": "faster", "veryfast": "faster",
	"superfast": "faster", "ultrafast": "faster",
}

// videoToolboxRealtimeMap: the fast half of the ladder runs in realtime mode.
var videoToolboxRealtimeMap = map[string]string{
	"ultrafast": "1", "superfast": "1", "veryfast": "1", "faster": "1", "fast": "1",
	"medium": "0", "slow": "0", "slower": "0", "veryslow": "0",
}

// proresProfileNames indexes prores_videotoolbox's -profile:v vocabulary by
// the adapter's integer "quality" knob.
var proresProfileNames = []string{"proxy", "lt", "standard", "hq", "4444", "xq"}

// ---------------------------------------------------------------------------
// Argv builders.
// ---------------------------------------------------------------------------

// presetPassthroughArgs emits "-c:v ENC -preset PRESET -KNOB QUALITY" with the
// mnemonic preset handed to the encoder verbatim (x264 / x265 / QSV).
func presetPassthroughArgs(a *Adapter, preset string, quality int) ([]string, error) {
	return []string{
		"-c:v", a.Encoder,
		"-preset", preset,
		"-" + a.QualityKnob, strconv.Itoa(quality),
	}, nil
}

// mappedPresetArgs emits "-c:v ENC -preset MAPPED -KNOB QUALITY" through a
// string preset lookup, falling back to the adapter's probe preset mapping
// when the mnemonic is unknown.
func mappedPresetArgs(table map[string]string, fallback string) func(*Adapter, string, int) ([]string, error) {
	return func(a *Adapter, preset string, quality int) ([]string, error) {
		mapped, ok := table[preset]
		if !ok {
			mapped = fallback
		}
		return []string{
			"-c:v", a.Encoder,
			"-preset", mapped,
			"-" + a.QualityKnob, strconv.Itoa(quality),
		}, nil
	}
}

// numericPresetArgs is mappedPresetArgs for encoders whose preset is an int
// (SVT-AV1's -preset, libaom's -cpu-used).
func numericPresetArgs(flag string, table map[string]int, fallback int) func(*Adapter, string, int) ([]string, error) {
	return func(a *Adapter, preset string, quality int) ([]string, error) {
		mapped, ok := table[preset]
		if !ok {
			mapped = fallback
		}
		return []string{
			"-c:v", a.Encoder,
			flag, strconv.Itoa(mapped),
			"-" + a.QualityKnob, strconv.Itoa(quality),
		}, nil
	}
}

func amfArgs(a *Adapter, preset string, quality int) ([]string, error) {
	return append([]string{"-c:v", a.Encoder}, amfExtra(a, preset, quality)...), nil
}

func amfExtra(_ *Adapter, preset string, quality int) []string {
	mode, ok := amfQualityMap[preset]
	if !ok {
		mode = "balanced"
	}
	q := strconv.Itoa(quality)
	return []string{"-quality", mode, "-rc", "cqp", "-qp_i", q, "-qp_p", q}
}

func libvpxArgs(a *Adapter, preset string, quality int) ([]string, error) {
	cpuUsed, ok := libvpxCPUUsedMap[preset]
	if !ok {
		cpuUsed = 5
	}
	return []string{
		"-c:v", a.Encoder,
		"-deadline", "good",
		"-cpu-used", strconv.Itoa(cpuUsed),
		"-crf", strconv.Itoa(quality),
		"-b:v", "0",
	}, nil
}

func libvpxExtra(_ *Adapter, _ string, _ int) []string {
	return []string{"-row-mt", "1"}
}

func videoToolboxArgs(a *Adapter, preset string, quality int) ([]string, error) {
	realtime, ok := videoToolboxRealtimeMap[preset]
	if !ok {
		realtime = "0"
	}
	return []string{
		"-c:v", a.Encoder,
		"-realtime", realtime,
		"-q:v", strconv.Itoa(quality),
	}, nil
}

func proresArgs(a *Adapter, preset string, quality int) ([]string, error) {
	realtime, ok := videoToolboxRealtimeMap[preset]
	if !ok {
		realtime = "0"
	}
	profile := "hq"
	if quality >= 0 && quality < len(proresProfileNames) {
		profile = proresProfileNames[quality]
	}
	return []string{
		"-c:v", a.Encoder,
		"-realtime", realtime,
		"-profile:v", profile,
	}, nil
}

func unavailableArgs(reason string) func(*Adapter, string, int) ([]string, error) {
	return func(a *Adapter, _ string, _ int) ([]string, error) {
		return nil, &ErrUnavailable{Codec: a.Name, Reason: reason}
	}
}

// ---------------------------------------------------------------------------
// Registry population.
// ---------------------------------------------------------------------------

func register(a Adapter) {
	registry[a.Name] = a
}

// of the registry — splitting it into helper functions would hide the very
// per-codec constants a reviewer needs to diff against the Python adapters.
//
//nolint:funlen // ADR-0237: one flat table entry per codec is the whole point
func init() {
	register(Adapter{
		Name: "libx264", Encoder: "libx264", QualityKnob: "crf",
		QualityLo: 0, QualityHi: 51, QualityDefault: 23, InvertQuality: true,
		Presets: x264Presets, ProbePreset: "ultrafast", ProbeQuality: 28,
		SupportsTwoPass: true, codecArgs: presetPassthroughArgs,
	})
	register(Adapter{
		Name: "libx265", Encoder: "libx265", QualityKnob: "crf",
		QualityLo: 15, QualityHi: 40, QualityDefault: 28, InvertQuality: true,
		Presets: x265Presets, ProbePreset: "ultrafast", ProbeQuality: 28,
		SupportsTwoPass: true, codecArgs: presetPassthroughArgs,
	})
	register(Adapter{
		Name: "libsvtav1", Encoder: "libsvtav1", QualityKnob: "crf",
		QualityLo: 20, QualityHi: 50, QualityDefault: 35, InvertQuality: true,
		Presets: svtPresets, ProbePreset: "veryfast", ProbeQuality: 35,
		SupportsTwoPass: false,
		codecArgs:       numericPresetArgs("-preset", svtPresetMap, 7),
	})
	register(Adapter{
		Name: "libaom-av1", Encoder: "libaom-av1", QualityKnob: "crf",
		QualityLo: 0, QualityHi: 63, QualityDefault: 35, InvertQuality: true,
		Presets: libaomPresets, ProbePreset: "ultrafast", ProbeQuality: 35,
		SupportsTwoPass: true,
		codecArgs:       numericPresetArgs("-cpu-used", libaomCPUUsedMap, 4),
	})
	register(Adapter{
		Name: "libvpx-vp9", Encoder: "libvpx-vp9", QualityKnob: "crf",
		QualityLo: 0, QualityHi: 63, QualityDefault: 32, InvertQuality: true,
		Presets: libvpxPresets, ProbePreset: "ultrafast", ProbeQuality: 32,
		SupportsTwoPass: true, codecArgs: libvpxArgs, extraArgs: libvpxExtra,
	})
	register(Adapter{
		Name: "libvvenc", Encoder: "libvvenc", QualityKnob: "qp",
		QualityLo: 17, QualityHi: 50, QualityDefault: 32, InvertQuality: true,
		Presets: vvencPresets, ProbePreset: "faster", ProbeQuality: 32,
		SupportsTwoPass: true,
		codecArgs:       mappedPresetArgs(vvencPresetMap, "medium"),
	})

	// NVENC family — mnemonic presets collapse onto p1..p7.
	for _, name := range []string{"h264_nvenc", "hevc_nvenc", "av1_nvenc"} {
		register(Adapter{
			Name: name, Encoder: name, QualityKnob: "cq",
			QualityLo: 15, QualityHi: 40, QualityDefault: 23, InvertQuality: true,
			Presets: tenPresets, ProbePreset: "ultrafast", ProbeQuality: 28,
			SupportsTwoPass: false,
			codecArgs:       mappedPresetArgs(nvencPresetMap, "p4"),
		})
	}

	// QSV family — mnemonic presets pass through verbatim.
	for _, name := range []string{"h264_qsv", "hevc_qsv", "av1_qsv"} {
		register(Adapter{
			Name: name, Encoder: name, QualityKnob: "global_quality",
			QualityLo: 1, QualityHi: 51, QualityDefault: 23, InvertQuality: true,
			Presets: qsvPresets, ProbePreset: "veryfast", ProbeQuality: 23,
			SupportsTwoPass: false, codecArgs: presetPassthroughArgs,
		})
	}

	// AMF family — mnemonic presets collapse onto speed/balanced/quality.
	for _, name := range []string{"h264_amf", "hevc_amf", "av1_amf"} {
		register(Adapter{
			Name: name, Encoder: name, QualityKnob: "qp",
			QualityLo: 15, QualityHi: 40, QualityDefault: 23, InvertQuality: true,
			Presets: tenPresets, ProbePreset: "ultrafast", ProbeQuality: 28,
			SupportsTwoPass: false, codecArgs: amfArgs, extraArgs: amfExtra,
		})
	}

	// VideoToolbox family — -q:v runs the other way (higher = better).
	for _, name := range []string{"h264_videotoolbox", "hevc_videotoolbox"} {
		register(Adapter{
			Name: name, Encoder: name, QualityKnob: "q:v",
			QualityLo: 0, QualityHi: 100, QualityDefault: 50, InvertQuality: false,
			Presets: videoToolboxPresets, ProbePreset: "ultrafast", ProbeQuality: 60,
			SupportsTwoPass: false, codecArgs: videoToolboxArgs,
		})
	}
	register(Adapter{
		Name: "prores_videotoolbox", Encoder: "prores_videotoolbox", QualityKnob: "profile:v",
		QualityLo: 0, QualityHi: 5, QualityDefault: 3, InvertQuality: false,
		Presets: videoToolboxPresets, ProbePreset: "ultrafast", ProbeQuality: 0,
		SupportsTwoPass: false, codecArgs: proresArgs,
	})
	register(Adapter{
		Name: "av1_videotoolbox", Encoder: "av1_videotoolbox", QualityKnob: "q:v",
		QualityLo: 0, QualityHi: 100, QualityDefault: 50, InvertQuality: false,
		Presets: videoToolboxPresets, ProbePreset: "ultrafast", ProbeQuality: 60,
		SupportsTwoPass: false,
		codecArgs: unavailableArgs(
			"awaiting upstream FFmpeg encoder support — see ADR-0339"),
	})
}
