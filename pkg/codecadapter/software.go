// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// pkg/codecadapter/software.go — software encoder adapters.
//
// Ports codec_adapters/{x264,x265,libaom,libvpx,svtav1,vvenc}.py. Field values
// and argv shapes are copied verbatim from the Python adapters; the comments
// cite the ADR that pinned each choice.

package codecadapter

import (
	"fmt"
	"strconv"
)

// ---------------------------------------------------------------------------
// libx264 — ADR-0237 Phase A + ADR-0333.
// ---------------------------------------------------------------------------

type x264Adapter struct{}

var x264Presets = []string{
	"ultrafast", "superfast", "veryfast", "faster", "fast",
	"medium", "slow", "slower", "veryslow",
}

func (x264Adapter) Name() string                       { return "libx264" }
func (x264Adapter) Encoder() string                    { return "libx264" }
func (x264Adapter) QualityKnob() string                { return "crf" }
func (x264Adapter) QualityRange() (int, int)           { return 0, 51 }
func (x264Adapter) QualityDefault() int                { return 23 }
func (x264Adapter) InvertQuality() bool                { return true }
func (x264Adapter) Presets() []string                  { return x264Presets }
func (x264Adapter) AdapterVersion() string             { return "2" }
func (x264Adapter) ProbePreset() string                { return "ultrafast" }
func (x264Adapter) ProbeQuality() int                  { return 28 }
func (x264Adapter) SupportsEncoderStats() bool         { return true }
func (x264Adapter) SupportsTwoPass() bool              { return true }
func (a x264Adapter) ExtraParams(string, int) []string { return nil }

func (a x264Adapter) Validate(preset string, crf int) error {
	if !containsString(x264Presets, preset) {
		return fmt.Errorf("unknown x264 preset %q; expected one of %v", preset, x264Presets)
	}
	lo, hi := a.QualityRange()
	if crf < lo || crf > hi {
		return fmt.Errorf("crf %d outside Phase A range [%d, %d]", crf, lo, hi)
	}
	return nil
}

func (a x264Adapter) FFmpegCodecArgs(preset string, quality int) ([]string, error) {
	return []string{"-c:v", a.Encoder(), "-preset", preset, "-crf", strconv.Itoa(quality)}, nil
}

func (x264Adapter) GopArgs(keyint int, minKeyint *int) ([]string, error) {
	return DefaultGopArgs(keyint, minKeyint)
}

func (x264Adapter) ForceKeyframesArgs(ts []float64) []string {
	return DefaultForceKeyframesArgs(ts)
}

func (a x264Adapter) ProbeArgs() ([]string, error) { return defaultProbeArgs(a) }

func (a x264Adapter) TwoPassArgs(passNumber int, statsPath string) ([]string, error) {
	if passNumber == 0 {
		return nil, nil
	}
	if passNumber != 1 && passNumber != 2 {
		return nil, fmt.Errorf("libx264 two_pass_args: pass_number must be 1 or 2, got %d", passNumber)
	}
	return []string{"-pass", strconv.Itoa(passNumber), "-passlogfile", statsPath}, nil
}

// ---------------------------------------------------------------------------
// libx265 — ADR-0288 + ADR-0333.
// ---------------------------------------------------------------------------

type x265Adapter struct{}

var x265Presets = []string{
	"ultrafast", "superfast", "veryfast", "faster", "fast",
	"medium", "slow", "slower", "veryslow", "placebo",
}

// x265ProfileByPixFmt maps a pixel format to the canonical x265 profile.
var x265ProfileByPixFmt = map[string]string{
	"yuv420p":     "main",
	"yuv422p":     "main422-8",
	"yuv444p":     "main444-8",
	"yuv420p10le": "main10",
	"yuv422p10le": "main422-10",
	"yuv444p10le": "main444-10",
	"yuv420p12le": "main12",
}

func (x265Adapter) Name() string                     { return "libx265" }
func (x265Adapter) Encoder() string                  { return "libx265" }
func (x265Adapter) QualityKnob() string              { return "crf" }
func (x265Adapter) QualityRange() (int, int)         { return 15, 40 }
func (x265Adapter) QualityDefault() int              { return 28 }
func (x265Adapter) InvertQuality() bool              { return true }
func (x265Adapter) Presets() []string                { return x265Presets }
func (x265Adapter) AdapterVersion() string           { return "" }
func (x265Adapter) ProbePreset() string              { return "ultrafast" }
func (x265Adapter) ProbeQuality() int                { return 28 }
func (x265Adapter) SupportsEncoderStats() bool       { return true }
func (x265Adapter) SupportsTwoPass() bool            { return true }
func (x265Adapter) ExtraParams(string, int) []string { return nil }

func (a x265Adapter) Validate(preset string, crf int) error {
	if !containsString(x265Presets, preset) {
		return fmt.Errorf("unknown x265 preset %q; expected one of %v", preset, x265Presets)
	}
	lo, hi := a.QualityRange()
	if crf < lo || crf > hi {
		return fmt.Errorf("crf %d outside Phase A range [%d, %d]", crf, lo, hi)
	}
	return nil
}

func (a x265Adapter) FFmpegCodecArgs(preset string, quality int) ([]string, error) {
	return []string{"-c:v", a.Encoder(), "-preset", preset, "-crf", strconv.Itoa(quality)}, nil
}

func (x265Adapter) GopArgs(keyint int, minKeyint *int) ([]string, error) {
	return DefaultGopArgs(keyint, minKeyint)
}

func (x265Adapter) ForceKeyframesArgs(ts []float64) []string {
	return DefaultForceKeyframesArgs(ts)
}

func (a x265Adapter) ProbeArgs() ([]string, error) {
	return []string{"-c:v", a.Encoder(), "-preset", a.ProbePreset(), "-crf",
		strconv.Itoa(a.ProbeQuality())}, nil
}

func (a x265Adapter) TwoPassArgs(passNumber int, statsPath string) ([]string, error) {
	if passNumber == 0 {
		return nil, nil
	}
	if passNumber != 1 && passNumber != 2 {
		return nil, fmt.Errorf("libx265 two_pass_args: pass_number must be 1 or 2, got %d", passNumber)
	}
	return []string{"-x265-params", fmt.Sprintf("pass=%d:stats=%s", passNumber, statsPath)}, nil
}

// ProfileFor returns the canonical x265 profile string for pixFmt, defaulting
// to "main" (8-bit 4:2:0) for unknown formats.
func (x265Adapter) ProfileFor(pixFmt string) string {
	if p, ok := x265ProfileByPixFmt[pixFmt]; ok {
		return p
	}
	return "main"
}

// ---------------------------------------------------------------------------
// libaom-av1 — ADR-0546.
// ---------------------------------------------------------------------------

type libaomAdapter struct{}

var libaomPresets = []string{
	"placebo", "slowest", "slower", "slow", "medium",
	"fast", "faster", "veryfast", "superfast", "ultrafast",
}

var libaomCPUUsed = map[string]int{
	"placebo": 0, "slowest": 1, "slower": 2, "slow": 3, "medium": 4,
	"fast": 5, "faster": 6, "veryfast": 7, "superfast": 8, "ultrafast": 9,
}

func (libaomAdapter) Name() string                     { return "libaom-av1" }
func (libaomAdapter) Encoder() string                  { return "libaom-av1" }
func (libaomAdapter) QualityKnob() string              { return "crf" }
func (libaomAdapter) QualityRange() (int, int)         { return 0, 63 }
func (libaomAdapter) QualityDefault() int              { return 35 }
func (libaomAdapter) InvertQuality() bool              { return true }
func (libaomAdapter) Presets() []string                { return libaomPresets }
func (libaomAdapter) AdapterVersion() string           { return "" }
func (libaomAdapter) ProbePreset() string              { return "ultrafast" }
func (libaomAdapter) ProbeQuality() int                { return 35 }
func (libaomAdapter) SupportsEncoderStats() bool       { return false }
func (libaomAdapter) SupportsTwoPass() bool            { return true }
func (libaomAdapter) ExtraParams(string, int) []string { return nil }

// cpuUsed maps a human preset name onto libaom's -cpu-used integer.
func (a libaomAdapter) cpuUsed(preset string) (int, error) {
	v, ok := libaomCPUUsed[preset]
	if !ok {
		return 0, fmt.Errorf("unknown libaom preset %q; expected one of %v", preset, libaomPresets)
	}
	return v, nil
}

func (a libaomAdapter) Validate(preset string, crf int) error {
	if !containsString(libaomPresets, preset) {
		return fmt.Errorf("unknown libaom preset %q; expected one of %v", preset, libaomPresets)
	}
	lo, hi := a.QualityRange()
	if crf < lo || crf > hi {
		return fmt.Errorf("crf %d outside libaom range [%d, %d]", crf, lo, hi)
	}
	return nil
}

func (a libaomAdapter) FFmpegCodecArgs(preset string, crf int) ([]string, error) {
	cpu, err := a.cpuUsed(preset)
	if err != nil {
		return nil, err
	}
	return []string{"-c:v", a.Encoder(), "-cpu-used", strconv.Itoa(cpu), "-crf",
		strconv.Itoa(crf)}, nil
}

func (libaomAdapter) GopArgs(keyint int, minKeyint *int) ([]string, error) {
	return DefaultGopArgs(keyint, minKeyint)
}

func (libaomAdapter) ForceKeyframesArgs(ts []float64) []string {
	return DefaultForceKeyframesArgs(ts)
}

func (a libaomAdapter) ProbeArgs() ([]string, error) {
	cpu, err := a.cpuUsed(a.ProbePreset())
	if err != nil {
		return nil, err
	}
	return []string{"-c:v", a.Encoder(), "-cpu-used", strconv.Itoa(cpu), "-crf",
		strconv.Itoa(a.ProbeQuality())}, nil
}

func (a libaomAdapter) TwoPassArgs(passNumber int, statsPath string) ([]string, error) {
	if passNumber == 0 {
		return nil, nil
	}
	if passNumber != 1 && passNumber != 2 {
		return nil, fmt.Errorf("libaom-av1 two_pass_args: pass_number must be 1 or 2, got %d",
			passNumber)
	}
	return []string{"-pass", strconv.Itoa(passNumber), "-passlogfile", statsPath}, nil
}

// ---------------------------------------------------------------------------
// libvpx-vp9.
// ---------------------------------------------------------------------------

type libvpxVP9Adapter struct{}

var libvpxPresets = []string{
	"placebo", "slowest", "slower", "slow", "medium",
	"fast", "faster", "veryfast", "superfast", "ultrafast",
}

var libvpxCPUUsed = map[string]int{
	"placebo": 0, "slowest": 0, "slower": 1, "slow": 2, "medium": 3,
	"fast": 4, "faster": 5, "veryfast": 5, "superfast": 5, "ultrafast": 5,
}

func (libvpxVP9Adapter) Name() string               { return "libvpx-vp9" }
func (libvpxVP9Adapter) Encoder() string            { return "libvpx-vp9" }
func (libvpxVP9Adapter) QualityKnob() string        { return "crf" }
func (libvpxVP9Adapter) QualityRange() (int, int)   { return 0, 63 }
func (libvpxVP9Adapter) QualityDefault() int        { return 32 }
func (libvpxVP9Adapter) InvertQuality() bool        { return true }
func (libvpxVP9Adapter) Presets() []string          { return libvpxPresets }
func (libvpxVP9Adapter) AdapterVersion() string     { return "1" }
func (libvpxVP9Adapter) ProbePreset() string        { return "ultrafast" }
func (libvpxVP9Adapter) ProbeQuality() int          { return 32 }
func (libvpxVP9Adapter) SupportsEncoderStats() bool { return false }
func (libvpxVP9Adapter) SupportsTwoPass() bool      { return true }

// ExtraParams enables VP9 row multithreading on hosts whose libvpx supports it.
func (libvpxVP9Adapter) ExtraParams(string, int) []string { return []string{"-row-mt", "1"} }

func (a libvpxVP9Adapter) cpuUsed(preset string) (int, error) {
	v, ok := libvpxCPUUsed[preset]
	if !ok {
		return 0, fmt.Errorf("unknown libvpx-vp9 preset %q; expected one of %v",
			preset, libvpxPresets)
	}
	return v, nil
}

func (a libvpxVP9Adapter) Validate(preset string, crf int) error {
	if !containsString(libvpxPresets, preset) {
		return fmt.Errorf("unknown libvpx-vp9 preset %q; expected one of %v", preset, libvpxPresets)
	}
	lo, hi := a.QualityRange()
	if crf < lo || crf > hi {
		return fmt.Errorf("crf %d outside libvpx-vp9 range [%d, %d]", crf, lo, hi)
	}
	return nil
}

func (a libvpxVP9Adapter) FFmpegCodecArgs(preset string, quality int) ([]string, error) {
	cpu, err := a.cpuUsed(preset)
	if err != nil {
		return nil, err
	}
	return []string{
		"-c:v", a.Encoder(),
		"-deadline", "good",
		"-cpu-used", strconv.Itoa(cpu),
		"-crf", strconv.Itoa(quality),
		"-b:v", "0",
	}, nil
}

func (libvpxVP9Adapter) GopArgs(keyint int, minKeyint *int) ([]string, error) {
	return DefaultGopArgs(keyint, minKeyint)
}

func (libvpxVP9Adapter) ForceKeyframesArgs(ts []float64) []string {
	return DefaultForceKeyframesArgs(ts)
}

func (a libvpxVP9Adapter) ProbeArgs() ([]string, error) {
	return a.FFmpegCodecArgs(a.ProbePreset(), a.ProbeQuality())
}

func (a libvpxVP9Adapter) TwoPassArgs(passNumber int, statsPath string) ([]string, error) {
	if passNumber == 0 {
		return nil, nil
	}
	if passNumber != 1 && passNumber != 2 {
		return nil, fmt.Errorf("libvpx-vp9 two_pass_args: pass_number must be 1 or 2, got %d",
			passNumber)
	}
	return []string{"-pass", strconv.Itoa(passNumber), "-passlogfile", statsPath}, nil
}

// ---------------------------------------------------------------------------
// libsvtav1 — ADR-0277 + ADR-0546.
// ---------------------------------------------------------------------------

type svtAV1Adapter struct{}

var svtAV1Presets = []string{
	"placebo", "slowest", "slower", "slow", "medium", "fast", "faster", "veryfast",
}

// svtAV1PresetToInt maps an x264-style preset name to SVT-AV1's integer preset.
var svtAV1PresetToInt = map[string]int{
	"placebo": 0, "slowest": 1, "slower": 3, "slow": 5,
	"medium": 7, "fast": 9, "faster": 11, "veryfast": 13,
}

const (
	svtAV1CRFMin = 0
	svtAV1CRFMax = 63
)

func (svtAV1Adapter) Name() string                     { return "libsvtav1" }
func (svtAV1Adapter) Encoder() string                  { return "libsvtav1" }
func (svtAV1Adapter) QualityKnob() string              { return "crf" }
func (svtAV1Adapter) QualityRange() (int, int)         { return 20, 50 }
func (svtAV1Adapter) QualityDefault() int              { return 35 }
func (svtAV1Adapter) InvertQuality() bool              { return true }
func (svtAV1Adapter) Presets() []string                { return svtAV1Presets }
func (svtAV1Adapter) AdapterVersion() string           { return "" }
func (svtAV1Adapter) ProbePreset() string              { return "veryfast" }
func (svtAV1Adapter) ProbeQuality() int                { return 35 }
func (svtAV1Adapter) SupportsEncoderStats() bool       { return false }
func (svtAV1Adapter) SupportsTwoPass() bool            { return false }
func (svtAV1Adapter) ExtraParams(string, int) []string { return nil }

// PresetToken returns the string the FFmpeg -preset slot expects for SVT-AV1
// (an integer rendered as a decimal string).
func (svtAV1Adapter) PresetToken(preset string) (string, error) {
	v, ok := svtAV1PresetToInt[preset]
	if !ok {
		return "", fmt.Errorf("unknown svtav1 preset %q; expected one of %v", preset, svtAV1Presets)
	}
	return strconv.Itoa(v), nil
}

func (a svtAV1Adapter) Validate(preset string, crf int) error {
	if !containsString(svtAV1Presets, preset) {
		return fmt.Errorf("unknown svtav1 preset %q; expected one of %v", preset, svtAV1Presets)
	}
	if crf < svtAV1CRFMin || crf > svtAV1CRFMax {
		return fmt.Errorf("crf %d outside SVT-AV1 absolute range [%d, %d]",
			crf, svtAV1CRFMin, svtAV1CRFMax)
	}
	lo, hi := a.QualityRange()
	if crf < lo || crf > hi {
		return fmt.Errorf("crf %d outside Phase A range [%d, %d]", crf, lo, hi)
	}
	return nil
}

func (a svtAV1Adapter) FFmpegCodecArgs(preset string, quality int) ([]string, error) {
	tok, err := a.PresetToken(preset)
	if err != nil {
		return nil, err
	}
	return []string{"-c:v", a.Encoder(), "-preset", tok, "-crf", strconv.Itoa(quality)}, nil
}

func (svtAV1Adapter) GopArgs(keyint int, minKeyint *int) ([]string, error) {
	return DefaultGopArgs(keyint, minKeyint)
}

func (svtAV1Adapter) ForceKeyframesArgs(ts []float64) []string {
	return DefaultForceKeyframesArgs(ts)
}

func (a svtAV1Adapter) ProbeArgs() ([]string, error) {
	tok, err := a.PresetToken(a.ProbePreset())
	if err != nil {
		return nil, err
	}
	return []string{"-c:v", a.Encoder(), "-preset", tok, "-crf",
		strconv.Itoa(a.ProbeQuality())}, nil
}

// TwoPassArgs returns the VBR-mode 2-pass argv. SupportsTwoPass is false
// because SVT-AV1 refuses multi-pass in CRF mode ("CRF does not support
// multi-pass. Use single pass." — SVT-AV1 v4.1.0), so the encode driver falls
// back to single-pass; callers that have switched the adapter to a
// bitrate-targeted mode can still splice these args in (ADR-0546).
func (svtAV1Adapter) TwoPassArgs(passNumber int, statsPath string) ([]string, error) {
	if passNumber == 0 {
		return nil, nil
	}
	if passNumber != 1 && passNumber != 2 {
		return nil, fmt.Errorf("libsvtav1 two_pass_args: pass_number must be 1 or 2, got %d",
			passNumber)
	}
	return []string{"-pass", strconv.Itoa(passNumber), "-passlogfile", statsPath}, nil
}

// ---------------------------------------------------------------------------
// libvvenc (VVC / H.266) — ADR-0285 + ADR-0546.
// ---------------------------------------------------------------------------

type vvencAdapter struct{}

var vvencPresets = []string{
	"placebo", "slowest", "slower", "slow", "medium",
	"fast", "faster", "veryfast", "superfast", "ultrafast",
}

// vvencPresetMap compresses the canonical 10-name vocabulary onto VVenC's five
// native presets.
var vvencPresetMap = map[string]string{
	"placebo": "slower", "slowest": "slower", "slower": "slower",
	"slow": "slow", "medium": "medium", "fast": "fast",
	"faster": "faster", "veryfast": "faster", "superfast": "faster",
	"ultrafast": "faster",
}

func (vvencAdapter) Name() string               { return "libvvenc" }
func (vvencAdapter) Encoder() string            { return "libvvenc" }
func (vvencAdapter) QualityKnob() string        { return "qp" }
func (vvencAdapter) QualityRange() (int, int)   { return 17, 50 }
func (vvencAdapter) QualityDefault() int        { return 32 }
func (vvencAdapter) InvertQuality() bool        { return true }
func (vvencAdapter) Presets() []string          { return vvencPresets }
func (vvencAdapter) AdapterVersion() string     { return "2" }
func (vvencAdapter) ProbePreset() string        { return "faster" }
func (vvencAdapter) ProbeQuality() int          { return 32 }
func (vvencAdapter) SupportsEncoderStats() bool { return false }
func (vvencAdapter) SupportsTwoPass() bool      { return true }

// ExtraParams is empty: every VVenC tuning knob defaults to the library
// default in the Python adapter, so _build_kv_pairs yields no pairs and
// extra_params returns (). Callers that need the tuning surface should
// construct the argv explicitly rather than mutating the registry singleton.
func (vvencAdapter) ExtraParams(string, int) []string { return nil }

// NativePreset compresses a canonical preset name onto VVenC's vocabulary.
func (vvencAdapter) NativePreset(preset string) (string, error) {
	v, ok := vvencPresetMap[preset]
	if !ok {
		return "", fmt.Errorf("unknown libvvenc preset %q; expected one of %v",
			preset, vvencPresets)
	}
	return v, nil
}

func (a vvencAdapter) Validate(preset string, qp int) error {
	if _, ok := vvencPresetMap[preset]; !ok {
		return fmt.Errorf("unknown libvvenc preset %q; expected one of %v", preset, vvencPresets)
	}
	lo, hi := a.QualityRange()
	if qp < lo || qp > hi {
		return fmt.Errorf("qp %d outside libvvenc range [%d, %d]", qp, lo, hi)
	}
	return nil
}

func (a vvencAdapter) FFmpegCodecArgs(preset string, quality int) ([]string, error) {
	native, err := a.NativePreset(preset)
	if err != nil {
		return nil, err
	}
	return []string{"-c:v", a.Encoder(), "-preset", native, "-qp", strconv.Itoa(quality)}, nil
}

func (vvencAdapter) GopArgs(keyint int, minKeyint *int) ([]string, error) {
	return DefaultGopArgs(keyint, minKeyint)
}

func (vvencAdapter) ForceKeyframesArgs(ts []float64) []string {
	return DefaultForceKeyframesArgs(ts)
}

func (a vvencAdapter) ProbeArgs() ([]string, error) {
	native, err := a.NativePreset(a.ProbePreset())
	if err != nil {
		return nil, err
	}
	return []string{"-c:v", a.Encoder(), "-preset", native, "-qp",
		strconv.Itoa(a.ProbeQuality())}, nil
}

func (vvencAdapter) TwoPassArgs(passNumber int, statsPath string) ([]string, error) {
	if passNumber == 0 {
		return nil, nil
	}
	if passNumber != 1 && passNumber != 2 {
		return nil, fmt.Errorf("libvvenc two_pass_args: pass_number must be 1 or 2, got %d",
			passNumber)
	}
	return []string{"-pass", strconv.Itoa(passNumber), "-passlogfile", statsPath}, nil
}
