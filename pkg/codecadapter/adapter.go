// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// pkg/codecadapter/adapter.go — Go port of vmaftune.codec_adapters.
//
// Every codec exposes a different parameter shape; the harness must not branch
// on codec identity in the search loop (ADR-0237). Each adapter declares its
// quality knob, range, defaults, and the FFmpeg encoder name, plus the argv
// slices the encode driver splices together.
//
// The argv strings emitted here are byte-for-byte identical to the Python
// adapters under tools/vmaf-tune/src/vmaftune/codec_adapters/ — corpus JSONL
// rows and reproducer command lines have to match across the two
// implementations for the duration of the ADR-0703 / ADR-0704 migration.
package codecadapter

import (
	"fmt"
	"sort"
	"strconv"
	"strings"
)

// Adapter is the codec-adapter contract (ADR-0237 Phase A + ADR-0294
// dispatcher). The encode dispatcher consumes the runtime-shaped subset
// (Encoder, FFmpegCodecArgs, ExtraParams) and never branches on Name.
type Adapter interface {
	// Name is the registry key (usually identical to Encoder).
	Name() string
	// Encoder is the FFmpeg encoder token passed to -c:v.
	Encoder() string
	// QualityKnob names the codec's quality parameter ("crf", "cq", ...).
	QualityKnob() string
	// QualityRange is the inclusive (lo, hi) window the harness sweeps.
	QualityRange() (int, int)
	// QualityDefault is the codec's canonical mid-scale quality value.
	QualityDefault() int
	// InvertQuality reports whether a higher quality value means lower
	// visual quality (true for every CRF-like knob).
	InvertQuality() bool
	// Presets is the adapter's named preset tuple, fastest-to-slowest or
	// slowest-to-fastest depending on the codec's own convention.
	Presets() []string
	// AdapterVersion bumps when the argv shape / preset list / quality
	// window changes (ADR-0298 cache key). Empty when the Python adapter
	// does not declare one.
	AdapterVersion() string
	// ProbePreset / ProbeQuality are the predictor probe-encode knobs.
	ProbePreset() string
	ProbeQuality() int
	// SupportsEncoderStats reports whether the encoder writes a parseable
	// pass-1 text stats file (ADR-0332).
	SupportsEncoderStats() bool
	// SupportsTwoPass reports whether the encoder implements a true
	// two-invocation 2-pass with a stats sidecar (ADR-0333 / ADR-0546).
	SupportsTwoPass() bool
	// Validate rejects an unsupported (preset, quality) pair.
	Validate(preset string, quality int) error
	// FFmpegCodecArgs is the "-c:v ..." argv slice for one encode.
	FFmpegCodecArgs(preset string, quality int) ([]string, error)
	// ExtraParams is the adapter-level argv tail appended after
	// FFmpegCodecArgs. Most adapters return nil.
	//
	// The (preset, quality) parameters mirror the Python AMF adapter's
	// two-argument extra_params signature; encode._resolve_codec_args
	// inspects the Python signature and forwards the cell coordinates
	// only for adapters that declare them. Adapters whose Python
	// counterpart takes no arguments ignore both here.
	ExtraParams(preset string, quality int) []string
	// GopArgs pins the GOP / keyint. Nil means "leave the codec default".
	GopArgs(keyint int, minKeyint *int) ([]string, error)
	// ForceKeyframesArgs pins keyframes at the given second offsets.
	ForceKeyframesArgs(timestamps []float64) []string
	// ProbeArgs is the FFmpeg argv slice for a fast probe encode.
	ProbeArgs() ([]string, error)
	// TwoPassArgs is the argv slice for the Nth pass of a 2-pass encode.
	TwoPassArgs(passNumber int, statsPath string) ([]string, error)
}

// DefaultGopArgs emits the FFmpeg-generic GOP knobs honoured by libx264,
// libx265, libsvtav1, libaom-av1, libvvenc and the NVENC / AMF / QSV families.
func DefaultGopArgs(keyint int, minKeyint *int) ([]string, error) {
	if keyint < 1 {
		return nil, fmt.Errorf("keyint must be >= 1, got %d", keyint)
	}
	args := []string{"-g", strconv.Itoa(keyint)}
	if minKeyint != nil {
		if *minKeyint < 1 || *minKeyint > keyint {
			return nil, fmt.Errorf("min_keyint must be in [1, keyint=%d], got %d",
				keyint, *minKeyint)
		}
		args = append(args, "-keyint_min", strconv.Itoa(*minKeyint))
	}
	return args, nil
}

// DefaultForceKeyframesArgs emits -force_key_frames with comma-separated
// seconds at microsecond precision. An empty timestamp slice emits no flag.
func DefaultForceKeyframesArgs(timestamps []float64) []string {
	if len(timestamps) == 0 {
		return nil
	}
	parts := make([]string, len(timestamps))
	for i, t := range timestamps {
		parts[i] = strconv.FormatFloat(t, 'f', 6, 64)
	}
	return []string{"-force_key_frames", strings.Join(parts, ",")}
}

// defaultProbeArgs delegates to the adapter's own codec-args builder using the
// adapter's probe preset / quality, mirroring _gop_common.default_probe_args.
func defaultProbeArgs(a Adapter) ([]string, error) {
	return a.FFmpegCodecArgs(a.ProbePreset(), a.ProbeQuality())
}

// containsString reports whether needle is present in haystack.
func containsString(haystack []string, needle string) bool {
	for _, v := range haystack {
		if v == needle {
			return true
		}
	}
	return false
}

// registry maps the adapter name to its singleton instance. Mirrors
// codec_adapters._REGISTRY.
var registry = map[string]Adapter{
	"libx264":             x264Adapter{},
	"libaom-av1":          libaomAdapter{},
	"libx265":             x265Adapter{},
	"h264_nvenc":          nvencAdapter{name: "h264_nvenc", encoder: "h264_nvenc"},
	"hevc_nvenc":          nvencAdapter{name: "hevc_nvenc", encoder: "hevc_nvenc"},
	"av1_nvenc":           nvencAdapter{name: "av1_nvenc", encoder: "av1_nvenc"},
	"h264_amf":            amfAdapter{name: "h264_amf", encoder: "h264_amf"},
	"hevc_amf":            amfAdapter{name: "hevc_amf", encoder: "hevc_amf"},
	"av1_amf":             amfAdapter{name: "av1_amf", encoder: "av1_amf"},
	"h264_qsv":            qsvAdapter{name: "h264_qsv", encoder: "h264_qsv"},
	"hevc_qsv":            qsvAdapter{name: "hevc_qsv", encoder: "hevc_qsv"},
	"av1_qsv":             qsvAdapter{name: "av1_qsv", encoder: "av1_qsv"},
	"h264_videotoolbox":   videoToolboxAdapter{name: "h264_videotoolbox", encoder: "h264_videotoolbox"},
	"hevc_videotoolbox":   videoToolboxAdapter{name: "hevc_videotoolbox", encoder: "hevc_videotoolbox"},
	"prores_videotoolbox": proresVideoToolboxAdapter{},
	"av1_videotoolbox":    av1VideoToolboxAdapter{},
	"libvvenc":            vvencAdapter{},
	"libsvtav1":           svtAV1Adapter{},
	"libvpx-vp9":          libvpxVP9Adapter{},
}

// Get returns the adapter registered under name.
func Get(name string) (Adapter, error) {
	a, ok := registry[name]
	if !ok {
		return nil, fmt.Errorf("unknown codec %q; known codecs: %v", name, KnownCodecs())
	}
	return a, nil
}

// KnownCodecs returns every registered adapter name, sorted.
func KnownCodecs() []string {
	names := make([]string, 0, len(registry))
	for k := range registry {
		names = append(names, k)
	}
	sort.Strings(names)
	return names
}

// ParseAvailableCodecs parses the output of "ffmpeg -hide_banner -encoders"
// into the set of encoder names the build advertises.
//
// When restrictToKnown is true (the Python default) only names present in the
// adapter registry are returned.
func ParseAvailableCodecs(encodersStdout string, restrictToKnown bool) map[string]bool {
	found := map[string]bool{}
	for _, raw := range strings.Split(encodersStdout, "\n") {
		line := strings.TrimSpace(raw)
		if line == "" || strings.Contains(line, "------") || strings.HasPrefix(line, "Encoders") {
			continue
		}
		tokens := strings.Fields(line)
		if len(tokens) < 2 {
			continue
		}
		flags, name := tokens[0], tokens[1]
		if flags == "" {
			continue
		}
		switch flags[0] {
		case 'V', 'A', 'S':
			found[name] = true
		}
	}
	if !restrictToKnown {
		return found
	}
	out := map[string]bool{}
	for _, known := range KnownCodecs() {
		if found[known] {
			out[known] = true
		}
	}
	return out
}
