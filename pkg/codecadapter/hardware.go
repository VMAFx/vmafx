// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// pkg/codecadapter/hardware.go — hardware encoder adapters.
//
// Ports codec_adapters/{_nvenc_common,_amf_common,_qsv_common,
// _videotoolbox_common,prores_videotoolbox,av1_videotoolbox}.py. The three
// NVENC / AMF / QSV per-codec files in Python are field-only subclasses of a
// shared base, so the Go port keeps one struct per family parameterised by
// (name, encoder).

package codecadapter

import (
	"errors"
	"fmt"
	"strconv"
)

// ---------------------------------------------------------------------------
// NVIDIA NVENC.
// ---------------------------------------------------------------------------

// nvencPresets is the mnemonic vocabulary every NVENC adapter accepts.
var nvencPresets = []string{
	"ultrafast", "superfast", "veryfast", "faster", "fast",
	"medium", "slow", "slower", "slowest", "placebo",
}

// nvencPresetMap collapses the ten mnemonic names onto NVENC's seven levels.
var nvencPresetMap = map[string]string{
	"ultrafast": "p1", "superfast": "p1", "veryfast": "p1",
	"faster": "p2", "fast": "p3", "medium": "p4",
	"slow": "p5", "slower": "p6", "slowest": "p7", "placebo": "p7",
}

// NVENC hardware CQ window vs. the narrower Phase-A sweep window.
const (
	nvencCQHardLo = 0
	nvencCQHardHi = 51
	nvencCQLo     = 15
	nvencCQHi     = 40
)

// NVENCPreset translates a mnemonic preset name to an NVENC "pN" token.
func NVENCPreset(name string) (string, error) {
	v, ok := nvencPresetMap[name]
	if !ok {
		return "", fmt.Errorf("unknown NVENC preset %q; expected one of %v", name, nvencPresets)
	}
	return v, nil
}

type nvencAdapter struct {
	name    string
	encoder string
}

func (a nvencAdapter) Name() string                   { return a.name }
func (a nvencAdapter) Encoder() string                { return a.encoder }
func (nvencAdapter) QualityKnob() string              { return "cq" }
func (nvencAdapter) QualityRange() (int, int)         { return nvencCQLo, nvencCQHi }
func (nvencAdapter) QualityDefault() int              { return 23 }
func (nvencAdapter) InvertQuality() bool              { return true }
func (nvencAdapter) Presets() []string                { return nvencPresets }
func (nvencAdapter) AdapterVersion() string           { return "" }
func (nvencAdapter) ProbePreset() string              { return "ultrafast" }
func (nvencAdapter) ProbeQuality() int                { return 28 }
func (nvencAdapter) SupportsEncoderStats() bool       { return false }
func (nvencAdapter) SupportsTwoPass() bool            { return false }
func (nvencAdapter) ExtraParams(string, int) []string { return nil }

func (nvencAdapter) Validate(preset string, cq int) error {
	if !containsString(nvencPresets, preset) {
		return fmt.Errorf("unknown NVENC preset %q; expected one of %v", preset, nvencPresets)
	}
	if cq < nvencCQHardLo || cq > nvencCQHardHi {
		return fmt.Errorf("cq %d outside NVENC range [%d, %d]", cq, nvencCQHardLo, nvencCQHardHi)
	}
	return nil
}

func (a nvencAdapter) FFmpegCodecArgs(preset string, quality int) ([]string, error) {
	p, err := NVENCPreset(preset)
	if err != nil {
		return nil, err
	}
	return []string{"-c:v", a.encoder, "-preset", p, "-cq", strconv.Itoa(quality)}, nil
}

func (nvencAdapter) GopArgs(keyint int, minKeyint *int) ([]string, error) {
	return DefaultGopArgs(keyint, minKeyint)
}

// ForceKeyframesArgs adds NVENC's -forced-idr 1: without it the encoder may
// emit non-IDR keyframes that downstream players treat as non-seekable.
func (nvencAdapter) ForceKeyframesArgs(ts []float64) []string {
	if len(ts) == 0 {
		return nil
	}
	return append(DefaultForceKeyframesArgs(ts), "-forced-idr", "1")
}

func (a nvencAdapter) ProbeArgs() ([]string, error) {
	p, err := NVENCPreset(a.ProbePreset())
	if err != nil {
		return nil, err
	}
	return []string{"-c:v", a.encoder, "-preset", p, "-cq",
		strconv.Itoa(a.ProbeQuality())}, nil
}

// TwoPassArgs returns NVENC's in-encoder multipass flags (ADR-0546). NVENC has
// no on-disk first-pass stats file, so SupportsTwoPass stays false and the
// 2-pass driver falls back to single-pass.
func (nvencAdapter) TwoPassArgs(passNumber int, _ string) ([]string, error) {
	switch passNumber {
	case 0, 2:
		return nil, nil
	case 1:
		return []string{"-multipass", "fullres"}, nil
	default:
		return nil, fmt.Errorf("NVENC two_pass_args: pass_number must be 0, 1, or 2, got %d",
			passNumber)
	}
}

// ---------------------------------------------------------------------------
// AMD AMF.
// ---------------------------------------------------------------------------

var amfPresets = []string{
	"ultrafast", "superfast", "veryfast", "faster", "fast",
	"medium", "slow", "slower", "slowest", "placebo",
}

// amfQualityMap compresses the ten mnemonic names onto AMF's three rungs.
var amfQualityMap = map[string]string{
	"placebo": "quality", "slowest": "quality", "slower": "quality", "slow": "quality",
	"medium": "balanced",
	"fast":   "speed", "faster": "speed", "veryfast": "speed",
	"superfast": "speed", "ultrafast": "speed",
}

// MapPresetToAMFQuality compresses a preset name onto AMF's 3 quality rungs.
func MapPresetToAMFQuality(preset string) (string, error) {
	v, ok := amfQualityMap[preset]
	if !ok {
		return "", fmt.Errorf("unknown AMF preset %q; expected one of %v", preset, amfPresets)
	}
	return v, nil
}

type amfAdapter struct {
	name    string
	encoder string
}

func (a amfAdapter) Name() string             { return a.name }
func (a amfAdapter) Encoder() string          { return a.encoder }
func (amfAdapter) QualityKnob() string        { return "qp" }
func (amfAdapter) QualityRange() (int, int)   { return 15, 40 }
func (amfAdapter) QualityDefault() int        { return 23 }
func (amfAdapter) InvertQuality() bool        { return true }
func (amfAdapter) Presets() []string          { return amfPresets }
func (amfAdapter) AdapterVersion() string     { return "" }
func (amfAdapter) ProbePreset() string        { return "ultrafast" }
func (amfAdapter) ProbeQuality() int          { return 28 }
func (amfAdapter) SupportsEncoderStats() bool { return false }
func (amfAdapter) SupportsTwoPass() bool      { return false }

func (a amfAdapter) Validate(preset string, qp int) error {
	if !containsString(amfPresets, preset) {
		return fmt.Errorf("unknown AMF preset %q; expected one of %v", preset, amfPresets)
	}
	lo, hi := a.QualityRange()
	if qp < lo || qp > hi {
		return fmt.Errorf("qp %d outside Phase A range [%d, %d] for %s", qp, lo, hi, a.encoder)
	}
	return nil
}

func (a amfAdapter) FFmpegCodecArgs(preset string, quality int) ([]string, error) {
	q, err := MapPresetToAMFQuality(preset)
	if err != nil {
		return nil, err
	}
	return []string{
		"-c:v", a.encoder,
		"-quality", q,
		"-rc", "cqp",
		"-qp_i", strconv.Itoa(quality),
		"-qp_p", strconv.Itoa(quality),
	}, nil
}

// ExtraParams mirrors the Python AMF adapter's two-argument extra_params.
//
// Parity note: encode._resolve_codec_args appends extra_params after
// ffmpeg_codec_args, and the AMF adapter is the only one whose extra_params
// repeats the -quality / -rc / -qp_i / -qp_p block that ffmpeg_codec_args
// already emitted. FFmpeg tolerates the repetition (last option wins), and the
// Go port reproduces it so the two implementations emit identical argv. It is
// a latent redundancy in the Python adapter, not an intentional flag.
func (a amfAdapter) ExtraParams(preset string, quality int) []string {
	q, err := MapPresetToAMFQuality(preset)
	if err != nil {
		// Unknown presets are rejected by Validate before the encode
		// driver reaches this point; emit nothing rather than panicking.
		return nil
	}
	return []string{
		"-quality", q,
		"-rc", "cqp",
		"-qp_i", strconv.Itoa(quality),
		"-qp_p", strconv.Itoa(quality),
	}
}

func (amfAdapter) GopArgs(keyint int, minKeyint *int) ([]string, error) {
	return DefaultGopArgs(keyint, minKeyint)
}

func (amfAdapter) ForceKeyframesArgs(ts []float64) []string {
	return DefaultForceKeyframesArgs(ts)
}

func (a amfAdapter) ProbeArgs() ([]string, error) {
	q, err := MapPresetToAMFQuality(a.ProbePreset())
	if err != nil {
		return nil, err
	}
	return []string{
		"-c:v", a.encoder,
		"-quality", q,
		"-rc", "cqp",
		"-qp_i", strconv.Itoa(a.ProbeQuality()),
		"-qp_p", strconv.Itoa(a.ProbeQuality()),
	}, nil
}

// TwoPassArgs returns AMF's in-encoder pre-analysis flag (ADR-0546).
func (amfAdapter) TwoPassArgs(passNumber int, _ string) ([]string, error) {
	switch passNumber {
	case 0, 2:
		return nil, nil
	case 1:
		return []string{"-preanalysis", "true"}, nil
	default:
		return nil, fmt.Errorf("AMF two_pass_args: pass_number must be 0, 1, or 2, got %d",
			passNumber)
	}
}

// ---------------------------------------------------------------------------
// Intel QSV.
// ---------------------------------------------------------------------------

// qsvPresets is identical to x264's medium/fast/... subset; FFmpeg's QSV bridge
// accepts the seven names verbatim.
var qsvPresets = []string{"veryslow", "slower", "slow", "medium", "fast", "faster", "veryfast"}

const (
	qsvQualityLo      = 1
	qsvQualityHi      = 51
	qsvQualityDefault = 23
)

// PresetToQSV identity-maps a preset name, erroring on unknown inputs.
func PresetToQSV(preset string) (string, error) {
	if !containsString(qsvPresets, preset) {
		return "", fmt.Errorf("unknown QSV preset %q; expected one of %v", preset, qsvPresets)
	}
	return preset, nil
}

type qsvAdapter struct {
	name    string
	encoder string
}

func (a qsvAdapter) Name() string                   { return a.name }
func (a qsvAdapter) Encoder() string                { return a.encoder }
func (qsvAdapter) QualityKnob() string              { return "global_quality" }
func (qsvAdapter) QualityRange() (int, int)         { return qsvQualityLo, qsvQualityHi }
func (qsvAdapter) QualityDefault() int              { return qsvQualityDefault }
func (qsvAdapter) InvertQuality() bool              { return true }
func (qsvAdapter) Presets() []string                { return qsvPresets }
func (qsvAdapter) AdapterVersion() string           { return "" }
func (qsvAdapter) ProbePreset() string              { return "veryfast" }
func (qsvAdapter) ProbeQuality() int                { return 23 }
func (qsvAdapter) SupportsEncoderStats() bool       { return false }
func (qsvAdapter) SupportsTwoPass() bool            { return false }
func (qsvAdapter) ExtraParams(string, int) []string { return nil }

func (qsvAdapter) Validate(preset string, quality int) error {
	if _, err := PresetToQSV(preset); err != nil {
		return err
	}
	if quality < qsvQualityLo || quality > qsvQualityHi {
		return fmt.Errorf("global_quality %d outside ICQ range [%d, %d]",
			quality, qsvQualityLo, qsvQualityHi)
	}
	return nil
}

func (a qsvAdapter) FFmpegCodecArgs(preset string, quality int) ([]string, error) {
	p, err := PresetToQSV(preset)
	if err != nil {
		return nil, err
	}
	return []string{"-c:v", a.encoder, "-preset", p, "-global_quality",
		strconv.Itoa(quality)}, nil
}

func (qsvAdapter) GopArgs(keyint int, minKeyint *int) ([]string, error) {
	return DefaultGopArgs(keyint, minKeyint)
}

func (qsvAdapter) ForceKeyframesArgs(ts []float64) []string {
	return DefaultForceKeyframesArgs(ts)
}

func (a qsvAdapter) ProbeArgs() ([]string, error) {
	p, err := PresetToQSV(a.ProbePreset())
	if err != nil {
		return nil, err
	}
	return []string{"-c:v", a.encoder, "-preset", p, "-global_quality",
		strconv.Itoa(a.ProbeQuality())}, nil
}

// TwoPassArgs returns QSV's extended-BRC look-ahead flags (ADR-0546).
func (qsvAdapter) TwoPassArgs(passNumber int, _ string) ([]string, error) {
	switch passNumber {
	case 0, 2:
		return nil, nil
	case 1:
		return []string{"-extbrc", "1", "-look_ahead_depth", "40"}, nil
	default:
		return nil, fmt.Errorf("QSV two_pass_args: pass_number must be 0, 1, or 2, got %d",
			passNumber)
	}
}

// ---------------------------------------------------------------------------
// Apple VideoToolbox.
// ---------------------------------------------------------------------------

var videoToolboxPresets = []string{
	"ultrafast", "superfast", "veryfast", "faster", "fast",
	"medium", "slow", "slower", "veryslow",
}

// videoToolboxRealtime maps the nine-name preset vocabulary onto VT's binary
// -realtime flag.
var videoToolboxRealtime = map[string]int{
	"ultrafast": 1, "superfast": 1, "veryfast": 1, "faster": 1, "fast": 1,
	"medium": 0, "slow": 0, "slower": 0, "veryslow": 0,
}

const (
	videoToolboxQualityLo      = 0
	videoToolboxQualityHi      = 100
	videoToolboxQualityDefault = 50
)

// ErrVideoToolboxTwoPassUnsupported is returned by every *_videotoolbox
// adapter's TwoPassArgs. The VTCompressionSession C API exposes no multi-pass
// encode interface (ADR-0546).
var ErrVideoToolboxTwoPassUnsupported = errors.New(
	"Apple VideoToolbox is single-pass only — the VTCompressionSession C API " +
		"does not expose a multi-pass encode interface (see ADR-0546); switch to a " +
		"software encoder (libx264 / libx265 / libsvtav1 / libaom-av1 / libvvenc) " +
		"for true 2-pass on macOS")

// ErrAV1VideoToolboxUnavailable is returned while av1_videotoolbox is still a
// placeholder — FFmpeg upstream has not shipped the encoder (ADR-0339).
var ErrAV1VideoToolboxUnavailable = errors.New(
	"av1_videotoolbox awaiting upstream FFmpeg encoder support — see ADR-0339")

// validateVideoToolbox is the shared H.264 / HEVC VT validator.
func validateVideoToolbox(preset string, q int) error {
	if !containsString(videoToolboxPresets, preset) {
		return fmt.Errorf("unknown VideoToolbox preset %q; expected one of %v",
			preset, videoToolboxPresets)
	}
	if q < videoToolboxQualityLo || q > videoToolboxQualityHi {
		return fmt.Errorf("q:v %d outside VideoToolbox range [%d, %d]",
			q, videoToolboxQualityLo, videoToolboxQualityHi)
	}
	return nil
}

type videoToolboxAdapter struct {
	name    string
	encoder string
}

func (a videoToolboxAdapter) Name() string      { return a.name }
func (a videoToolboxAdapter) Encoder() string   { return a.encoder }
func (videoToolboxAdapter) QualityKnob() string { return "q:v" }
func (videoToolboxAdapter) QualityRange() (int, int) {
	return videoToolboxQualityLo, videoToolboxQualityHi
}
func (videoToolboxAdapter) QualityDefault() int              { return videoToolboxQualityDefault }
func (videoToolboxAdapter) InvertQuality() bool              { return false }
func (videoToolboxAdapter) Presets() []string                { return videoToolboxPresets }
func (videoToolboxAdapter) AdapterVersion() string           { return "" }
func (videoToolboxAdapter) ProbePreset() string              { return "ultrafast" }
func (videoToolboxAdapter) ProbeQuality() int                { return 60 }
func (videoToolboxAdapter) SupportsEncoderStats() bool       { return false }
func (videoToolboxAdapter) SupportsTwoPass() bool            { return false }
func (videoToolboxAdapter) ExtraParams(string, int) []string { return nil }

func (videoToolboxAdapter) Validate(preset string, crf int) error {
	return validateVideoToolbox(preset, crf)
}

func (a videoToolboxAdapter) FFmpegCodecArgs(preset string, quality int) ([]string, error) {
	rt, ok := videoToolboxRealtime[preset]
	if !ok {
		return nil, fmt.Errorf("unknown VideoToolbox preset %q; expected one of %v",
			preset, videoToolboxPresets)
	}
	return []string{"-c:v", a.encoder, "-realtime", strconv.Itoa(rt), "-q:v",
		strconv.Itoa(quality)}, nil
}

func (videoToolboxAdapter) GopArgs(keyint int, minKeyint *int) ([]string, error) {
	return DefaultGopArgs(keyint, minKeyint)
}

func (videoToolboxAdapter) ForceKeyframesArgs(ts []float64) []string {
	return DefaultForceKeyframesArgs(ts)
}

func (a videoToolboxAdapter) ProbeArgs() ([]string, error) { return defaultProbeArgs(a) }

func (a videoToolboxAdapter) TwoPassArgs(passNumber int, _ string) ([]string, error) {
	return nil, fmt.Errorf("%q: %w (pass_number=%d)",
		a.encoder, ErrVideoToolboxTwoPassUnsupported, passNumber)
}

// ---------------------------------------------------------------------------
// ProRes VideoToolbox — fixed-tier intermediate codec.
// ---------------------------------------------------------------------------

// proresProfileNames is indexed by the integer tier id (0..5).
var proresProfileNames = []string{"proxy", "lt", "standard", "hq", "4444", "xq"}

const (
	proresProfileLo      = 0
	proresProfileHi      = 5
	proresProfileDefault = 3 // 422 HQ — the professional acquisition standard.
)

// ProResProfileName returns the FFmpeg tier alias for an integer profile id.
func ProResProfileName(profile int) (string, error) {
	if profile < proresProfileLo || profile > proresProfileHi {
		return "", fmt.Errorf("unknown ProRes profile %d; expected an integer in [%d, %d]",
			profile, proresProfileLo, proresProfileHi)
	}
	return proresProfileNames[profile], nil
}

type proresVideoToolboxAdapter struct{}

func (proresVideoToolboxAdapter) Name() string        { return "prores_videotoolbox" }
func (proresVideoToolboxAdapter) Encoder() string     { return "prores_videotoolbox" }
func (proresVideoToolboxAdapter) QualityKnob() string { return "profile:v" }
func (proresVideoToolboxAdapter) QualityRange() (int, int) {
	return proresProfileLo, proresProfileHi
}
func (proresVideoToolboxAdapter) QualityDefault() int              { return proresProfileDefault }
func (proresVideoToolboxAdapter) InvertQuality() bool              { return false }
func (proresVideoToolboxAdapter) Presets() []string                { return videoToolboxPresets }
func (proresVideoToolboxAdapter) AdapterVersion() string           { return "1" }
func (proresVideoToolboxAdapter) ProbePreset() string              { return "ultrafast" }
func (proresVideoToolboxAdapter) ProbeQuality() int                { return 0 } // proxy tier
func (proresVideoToolboxAdapter) SupportsEncoderStats() bool       { return false }
func (proresVideoToolboxAdapter) SupportsTwoPass() bool            { return false }
func (proresVideoToolboxAdapter) ExtraParams(string, int) []string { return nil }

func (proresVideoToolboxAdapter) Validate(preset string, profile int) error {
	if !containsString(videoToolboxPresets, preset) {
		return fmt.Errorf("unknown VideoToolbox preset %q; expected one of %v",
			preset, videoToolboxPresets)
	}
	if profile < proresProfileLo || profile > proresProfileHi {
		return fmt.Errorf("ProRes profile %d outside tier range [%d, %d] (see PRORES_PROFILE_NAMES)",
			profile, proresProfileLo, proresProfileHi)
	}
	return nil
}

func (a proresVideoToolboxAdapter) FFmpegCodecArgs(preset string, quality int) ([]string, error) {
	rt, ok := videoToolboxRealtime[preset]
	if !ok {
		return nil, fmt.Errorf("unknown VideoToolbox preset %q; expected one of %v",
			preset, videoToolboxPresets)
	}
	name, err := ProResProfileName(quality)
	if err != nil {
		return nil, err
	}
	return []string{"-c:v", a.Encoder(), "-realtime", strconv.Itoa(rt), "-profile:v", name}, nil
}

func (proresVideoToolboxAdapter) GopArgs(keyint int, minKeyint *int) ([]string, error) {
	return DefaultGopArgs(keyint, minKeyint)
}

func (proresVideoToolboxAdapter) ForceKeyframesArgs(ts []float64) []string {
	return DefaultForceKeyframesArgs(ts)
}

func (a proresVideoToolboxAdapter) ProbeArgs() ([]string, error) { return defaultProbeArgs(a) }

func (a proresVideoToolboxAdapter) TwoPassArgs(passNumber int, _ string) ([]string, error) {
	return nil, fmt.Errorf("%q: %w (pass_number=%d)",
		a.Encoder(), ErrVideoToolboxTwoPassUnsupported, passNumber)
}

// ---------------------------------------------------------------------------
// AV1 VideoToolbox — placeholder until FFmpeg ships the encoder (ADR-0339).
// ---------------------------------------------------------------------------

// AV1VideoToolboxAvailable is the runtime activation switch for the
// av1_videotoolbox placeholder. The Python adapter probes
// "ffmpeg -h encoder=av1_videotoolbox" on every validate() call; the Go port
// exposes the same decision as a package-level hook so callers (and tests) can
// drive it without spawning a subprocess from inside the adapter registry.
//
// It stays nil — meaning "unavailable" — until a caller installs a probe.
var AV1VideoToolboxAvailable func() bool

func av1VideoToolboxRuntimeAvailable() bool {
	if AV1VideoToolboxAvailable == nil {
		return false
	}
	return AV1VideoToolboxAvailable()
}

type av1VideoToolboxAdapter struct{}

func (av1VideoToolboxAdapter) Name() string        { return "av1_videotoolbox" }
func (av1VideoToolboxAdapter) Encoder() string     { return "av1_videotoolbox" }
func (av1VideoToolboxAdapter) QualityKnob() string { return "q:v" }
func (av1VideoToolboxAdapter) QualityRange() (int, int) {
	return videoToolboxQualityLo, videoToolboxQualityHi
}
func (av1VideoToolboxAdapter) QualityDefault() int              { return videoToolboxQualityDefault }
func (av1VideoToolboxAdapter) InvertQuality() bool              { return false }
func (av1VideoToolboxAdapter) Presets() []string                { return videoToolboxPresets }
func (av1VideoToolboxAdapter) AdapterVersion() string           { return "0-placeholder" }
func (av1VideoToolboxAdapter) ProbePreset() string              { return "ultrafast" }
func (av1VideoToolboxAdapter) ProbeQuality() int                { return 60 }
func (av1VideoToolboxAdapter) SupportsEncoderStats() bool       { return false }
func (av1VideoToolboxAdapter) SupportsTwoPass() bool            { return false }
func (av1VideoToolboxAdapter) ExtraParams(string, int) []string { return nil }

func (av1VideoToolboxAdapter) Validate(preset string, crf int) error {
	if !av1VideoToolboxRuntimeAvailable() {
		return ErrAV1VideoToolboxUnavailable
	}
	return validateVideoToolbox(preset, crf)
}

func (a av1VideoToolboxAdapter) FFmpegCodecArgs(preset string, quality int) ([]string, error) {
	if !av1VideoToolboxRuntimeAvailable() {
		return nil, ErrAV1VideoToolboxUnavailable
	}
	rt, ok := videoToolboxRealtime[preset]
	if !ok {
		return nil, fmt.Errorf("unknown VideoToolbox preset %q; expected one of %v",
			preset, videoToolboxPresets)
	}
	return []string{"-c:v", a.Encoder(), "-realtime", strconv.Itoa(rt), "-q:v",
		strconv.Itoa(quality)}, nil
}

func (av1VideoToolboxAdapter) GopArgs(keyint int, minKeyint *int) ([]string, error) {
	return DefaultGopArgs(keyint, minKeyint)
}

func (av1VideoToolboxAdapter) ForceKeyframesArgs(ts []float64) []string {
	return DefaultForceKeyframesArgs(ts)
}

func (a av1VideoToolboxAdapter) ProbeArgs() ([]string, error) { return defaultProbeArgs(a) }

func (a av1VideoToolboxAdapter) TwoPassArgs(passNumber int, _ string) ([]string, error) {
	return nil, fmt.Errorf("%q: %w (pass_number=%d)",
		a.Encoder(), ErrVideoToolboxTwoPassUnsupported, passNumber)
}
