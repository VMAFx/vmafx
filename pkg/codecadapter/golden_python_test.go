// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package codecadapter_test

// goldenArgv is a verbatim dump of the Python codec-adapter registry
// (tools/vmaf-tune/src/vmaftune/codec_adapters/) taken at the commit that
// introduced this file. Each entry maps codec -> mnemonic preset -> the
// argv slice ffmpeg_codec_args(preset, quality_default) returned.
//
// It is the parity oracle for the Go registry: drift between the two
// implementations fails TestFFmpegCodecArgs_matchesPythonGolden.
//
// Two codecs are deliberately absent:
//   - av1_videotoolbox — the Python adapter raises
//     Av1VideoToolboxUnavailableError for every preset (ADR-0339); the Go
//     port returns an error and is covered by its own test.
//   - the AMF trio's duplicated tail — see the Adapter.FFmpegCodecArgs
//     doc comment; the Go port emits each AMF token once, and this table
//     records ffmpeg_codec_args() (single copy), not the doubled argv
//     _resolve_codec_args() actually hands to FFmpeg.
var goldenArgv = map[string]map[string][]string{
	"libx264": {
		"ultrafast": {"-c:v", "libx264", "-preset", "ultrafast", "-crf", "23"},
		"superfast": {"-c:v", "libx264", "-preset", "superfast", "-crf", "23"},
		"veryfast":  {"-c:v", "libx264", "-preset", "veryfast", "-crf", "23"},
		"faster":    {"-c:v", "libx264", "-preset", "faster", "-crf", "23"},
		"fast":      {"-c:v", "libx264", "-preset", "fast", "-crf", "23"},
		"medium":    {"-c:v", "libx264", "-preset", "medium", "-crf", "23"},
		"slow":      {"-c:v", "libx264", "-preset", "slow", "-crf", "23"},
		"slower":    {"-c:v", "libx264", "-preset", "slower", "-crf", "23"},
		"veryslow":  {"-c:v", "libx264", "-preset", "veryslow", "-crf", "23"},
	},
	"libaom-av1": {
		"placebo":   {"-c:v", "libaom-av1", "-cpu-used", "0", "-crf", "35"},
		"slowest":   {"-c:v", "libaom-av1", "-cpu-used", "1", "-crf", "35"},
		"slower":    {"-c:v", "libaom-av1", "-cpu-used", "2", "-crf", "35"},
		"slow":      {"-c:v", "libaom-av1", "-cpu-used", "3", "-crf", "35"},
		"medium":    {"-c:v", "libaom-av1", "-cpu-used", "4", "-crf", "35"},
		"fast":      {"-c:v", "libaom-av1", "-cpu-used", "5", "-crf", "35"},
		"faster":    {"-c:v", "libaom-av1", "-cpu-used", "6", "-crf", "35"},
		"veryfast":  {"-c:v", "libaom-av1", "-cpu-used", "7", "-crf", "35"},
		"superfast": {"-c:v", "libaom-av1", "-cpu-used", "8", "-crf", "35"},
		"ultrafast": {"-c:v", "libaom-av1", "-cpu-used", "9", "-crf", "35"},
	},
	"libx265": {
		"ultrafast": {"-c:v", "libx265", "-preset", "ultrafast", "-crf", "28"},
		"superfast": {"-c:v", "libx265", "-preset", "superfast", "-crf", "28"},
		"veryfast":  {"-c:v", "libx265", "-preset", "veryfast", "-crf", "28"},
		"faster":    {"-c:v", "libx265", "-preset", "faster", "-crf", "28"},
		"fast":      {"-c:v", "libx265", "-preset", "fast", "-crf", "28"},
		"medium":    {"-c:v", "libx265", "-preset", "medium", "-crf", "28"},
		"slow":      {"-c:v", "libx265", "-preset", "slow", "-crf", "28"},
		"slower":    {"-c:v", "libx265", "-preset", "slower", "-crf", "28"},
		"veryslow":  {"-c:v", "libx265", "-preset", "veryslow", "-crf", "28"},
		"placebo":   {"-c:v", "libx265", "-preset", "placebo", "-crf", "28"},
	},
	"h264_nvenc": {
		"ultrafast": {"-c:v", "h264_nvenc", "-preset", "p1", "-cq", "23"},
		"superfast": {"-c:v", "h264_nvenc", "-preset", "p1", "-cq", "23"},
		"veryfast":  {"-c:v", "h264_nvenc", "-preset", "p1", "-cq", "23"},
		"faster":    {"-c:v", "h264_nvenc", "-preset", "p2", "-cq", "23"},
		"fast":      {"-c:v", "h264_nvenc", "-preset", "p3", "-cq", "23"},
		"medium":    {"-c:v", "h264_nvenc", "-preset", "p4", "-cq", "23"},
		"slow":      {"-c:v", "h264_nvenc", "-preset", "p5", "-cq", "23"},
		"slower":    {"-c:v", "h264_nvenc", "-preset", "p6", "-cq", "23"},
		"slowest":   {"-c:v", "h264_nvenc", "-preset", "p7", "-cq", "23"},
		"placebo":   {"-c:v", "h264_nvenc", "-preset", "p7", "-cq", "23"},
	},
	"hevc_nvenc": {
		"ultrafast": {"-c:v", "hevc_nvenc", "-preset", "p1", "-cq", "23"},
		"superfast": {"-c:v", "hevc_nvenc", "-preset", "p1", "-cq", "23"},
		"veryfast":  {"-c:v", "hevc_nvenc", "-preset", "p1", "-cq", "23"},
		"faster":    {"-c:v", "hevc_nvenc", "-preset", "p2", "-cq", "23"},
		"fast":      {"-c:v", "hevc_nvenc", "-preset", "p3", "-cq", "23"},
		"medium":    {"-c:v", "hevc_nvenc", "-preset", "p4", "-cq", "23"},
		"slow":      {"-c:v", "hevc_nvenc", "-preset", "p5", "-cq", "23"},
		"slower":    {"-c:v", "hevc_nvenc", "-preset", "p6", "-cq", "23"},
		"slowest":   {"-c:v", "hevc_nvenc", "-preset", "p7", "-cq", "23"},
		"placebo":   {"-c:v", "hevc_nvenc", "-preset", "p7", "-cq", "23"},
	},
	"av1_nvenc": {
		"ultrafast": {"-c:v", "av1_nvenc", "-preset", "p1", "-cq", "23"},
		"superfast": {"-c:v", "av1_nvenc", "-preset", "p1", "-cq", "23"},
		"veryfast":  {"-c:v", "av1_nvenc", "-preset", "p1", "-cq", "23"},
		"faster":    {"-c:v", "av1_nvenc", "-preset", "p2", "-cq", "23"},
		"fast":      {"-c:v", "av1_nvenc", "-preset", "p3", "-cq", "23"},
		"medium":    {"-c:v", "av1_nvenc", "-preset", "p4", "-cq", "23"},
		"slow":      {"-c:v", "av1_nvenc", "-preset", "p5", "-cq", "23"},
		"slower":    {"-c:v", "av1_nvenc", "-preset", "p6", "-cq", "23"},
		"slowest":   {"-c:v", "av1_nvenc", "-preset", "p7", "-cq", "23"},
		"placebo":   {"-c:v", "av1_nvenc", "-preset", "p7", "-cq", "23"},
	},
	"h264_amf": {
		"ultrafast": {"-c:v", "h264_amf", "-quality", "speed", "-rc", "cqp", "-qp_i", "23", "-qp_p", "23"},
		"superfast": {"-c:v", "h264_amf", "-quality", "speed", "-rc", "cqp", "-qp_i", "23", "-qp_p", "23"},
		"veryfast":  {"-c:v", "h264_amf", "-quality", "speed", "-rc", "cqp", "-qp_i", "23", "-qp_p", "23"},
		"faster":    {"-c:v", "h264_amf", "-quality", "speed", "-rc", "cqp", "-qp_i", "23", "-qp_p", "23"},
		"fast":      {"-c:v", "h264_amf", "-quality", "speed", "-rc", "cqp", "-qp_i", "23", "-qp_p", "23"},
		"medium":    {"-c:v", "h264_amf", "-quality", "balanced", "-rc", "cqp", "-qp_i", "23", "-qp_p", "23"},
		"slow":      {"-c:v", "h264_amf", "-quality", "quality", "-rc", "cqp", "-qp_i", "23", "-qp_p", "23"},
		"slower":    {"-c:v", "h264_amf", "-quality", "quality", "-rc", "cqp", "-qp_i", "23", "-qp_p", "23"},
		"slowest":   {"-c:v", "h264_amf", "-quality", "quality", "-rc", "cqp", "-qp_i", "23", "-qp_p", "23"},
		"placebo":   {"-c:v", "h264_amf", "-quality", "quality", "-rc", "cqp", "-qp_i", "23", "-qp_p", "23"},
	},
	"hevc_amf": {
		"ultrafast": {"-c:v", "hevc_amf", "-quality", "speed", "-rc", "cqp", "-qp_i", "23", "-qp_p", "23"},
		"superfast": {"-c:v", "hevc_amf", "-quality", "speed", "-rc", "cqp", "-qp_i", "23", "-qp_p", "23"},
		"veryfast":  {"-c:v", "hevc_amf", "-quality", "speed", "-rc", "cqp", "-qp_i", "23", "-qp_p", "23"},
		"faster":    {"-c:v", "hevc_amf", "-quality", "speed", "-rc", "cqp", "-qp_i", "23", "-qp_p", "23"},
		"fast":      {"-c:v", "hevc_amf", "-quality", "speed", "-rc", "cqp", "-qp_i", "23", "-qp_p", "23"},
		"medium":    {"-c:v", "hevc_amf", "-quality", "balanced", "-rc", "cqp", "-qp_i", "23", "-qp_p", "23"},
		"slow":      {"-c:v", "hevc_amf", "-quality", "quality", "-rc", "cqp", "-qp_i", "23", "-qp_p", "23"},
		"slower":    {"-c:v", "hevc_amf", "-quality", "quality", "-rc", "cqp", "-qp_i", "23", "-qp_p", "23"},
		"slowest":   {"-c:v", "hevc_amf", "-quality", "quality", "-rc", "cqp", "-qp_i", "23", "-qp_p", "23"},
		"placebo":   {"-c:v", "hevc_amf", "-quality", "quality", "-rc", "cqp", "-qp_i", "23", "-qp_p", "23"},
	},
	"av1_amf": {
		"ultrafast": {"-c:v", "av1_amf", "-quality", "speed", "-rc", "cqp", "-qp_i", "23", "-qp_p", "23"},
		"superfast": {"-c:v", "av1_amf", "-quality", "speed", "-rc", "cqp", "-qp_i", "23", "-qp_p", "23"},
		"veryfast":  {"-c:v", "av1_amf", "-quality", "speed", "-rc", "cqp", "-qp_i", "23", "-qp_p", "23"},
		"faster":    {"-c:v", "av1_amf", "-quality", "speed", "-rc", "cqp", "-qp_i", "23", "-qp_p", "23"},
		"fast":      {"-c:v", "av1_amf", "-quality", "speed", "-rc", "cqp", "-qp_i", "23", "-qp_p", "23"},
		"medium":    {"-c:v", "av1_amf", "-quality", "balanced", "-rc", "cqp", "-qp_i", "23", "-qp_p", "23"},
		"slow":      {"-c:v", "av1_amf", "-quality", "quality", "-rc", "cqp", "-qp_i", "23", "-qp_p", "23"},
		"slower":    {"-c:v", "av1_amf", "-quality", "quality", "-rc", "cqp", "-qp_i", "23", "-qp_p", "23"},
		"slowest":   {"-c:v", "av1_amf", "-quality", "quality", "-rc", "cqp", "-qp_i", "23", "-qp_p", "23"},
		"placebo":   {"-c:v", "av1_amf", "-quality", "quality", "-rc", "cqp", "-qp_i", "23", "-qp_p", "23"},
	},
	"h264_qsv": {
		"veryslow": {"-c:v", "h264_qsv", "-preset", "veryslow", "-global_quality", "23"},
		"slower":   {"-c:v", "h264_qsv", "-preset", "slower", "-global_quality", "23"},
		"slow":     {"-c:v", "h264_qsv", "-preset", "slow", "-global_quality", "23"},
		"medium":   {"-c:v", "h264_qsv", "-preset", "medium", "-global_quality", "23"},
		"fast":     {"-c:v", "h264_qsv", "-preset", "fast", "-global_quality", "23"},
		"faster":   {"-c:v", "h264_qsv", "-preset", "faster", "-global_quality", "23"},
		"veryfast": {"-c:v", "h264_qsv", "-preset", "veryfast", "-global_quality", "23"},
	},
	"hevc_qsv": {
		"veryslow": {"-c:v", "hevc_qsv", "-preset", "veryslow", "-global_quality", "23"},
		"slower":   {"-c:v", "hevc_qsv", "-preset", "slower", "-global_quality", "23"},
		"slow":     {"-c:v", "hevc_qsv", "-preset", "slow", "-global_quality", "23"},
		"medium":   {"-c:v", "hevc_qsv", "-preset", "medium", "-global_quality", "23"},
		"fast":     {"-c:v", "hevc_qsv", "-preset", "fast", "-global_quality", "23"},
		"faster":   {"-c:v", "hevc_qsv", "-preset", "faster", "-global_quality", "23"},
		"veryfast": {"-c:v", "hevc_qsv", "-preset", "veryfast", "-global_quality", "23"},
	},
	"av1_qsv": {
		"veryslow": {"-c:v", "av1_qsv", "-preset", "veryslow", "-global_quality", "23"},
		"slower":   {"-c:v", "av1_qsv", "-preset", "slower", "-global_quality", "23"},
		"slow":     {"-c:v", "av1_qsv", "-preset", "slow", "-global_quality", "23"},
		"medium":   {"-c:v", "av1_qsv", "-preset", "medium", "-global_quality", "23"},
		"fast":     {"-c:v", "av1_qsv", "-preset", "fast", "-global_quality", "23"},
		"faster":   {"-c:v", "av1_qsv", "-preset", "faster", "-global_quality", "23"},
		"veryfast": {"-c:v", "av1_qsv", "-preset", "veryfast", "-global_quality", "23"},
	},
	"h264_videotoolbox": {
		"ultrafast": {"-c:v", "h264_videotoolbox", "-realtime", "1", "-q:v", "50"},
		"superfast": {"-c:v", "h264_videotoolbox", "-realtime", "1", "-q:v", "50"},
		"veryfast":  {"-c:v", "h264_videotoolbox", "-realtime", "1", "-q:v", "50"},
		"faster":    {"-c:v", "h264_videotoolbox", "-realtime", "1", "-q:v", "50"},
		"fast":      {"-c:v", "h264_videotoolbox", "-realtime", "1", "-q:v", "50"},
		"medium":    {"-c:v", "h264_videotoolbox", "-realtime", "0", "-q:v", "50"},
		"slow":      {"-c:v", "h264_videotoolbox", "-realtime", "0", "-q:v", "50"},
		"slower":    {"-c:v", "h264_videotoolbox", "-realtime", "0", "-q:v", "50"},
		"veryslow":  {"-c:v", "h264_videotoolbox", "-realtime", "0", "-q:v", "50"},
	},
	"hevc_videotoolbox": {
		"ultrafast": {"-c:v", "hevc_videotoolbox", "-realtime", "1", "-q:v", "50"},
		"superfast": {"-c:v", "hevc_videotoolbox", "-realtime", "1", "-q:v", "50"},
		"veryfast":  {"-c:v", "hevc_videotoolbox", "-realtime", "1", "-q:v", "50"},
		"faster":    {"-c:v", "hevc_videotoolbox", "-realtime", "1", "-q:v", "50"},
		"fast":      {"-c:v", "hevc_videotoolbox", "-realtime", "1", "-q:v", "50"},
		"medium":    {"-c:v", "hevc_videotoolbox", "-realtime", "0", "-q:v", "50"},
		"slow":      {"-c:v", "hevc_videotoolbox", "-realtime", "0", "-q:v", "50"},
		"slower":    {"-c:v", "hevc_videotoolbox", "-realtime", "0", "-q:v", "50"},
		"veryslow":  {"-c:v", "hevc_videotoolbox", "-realtime", "0", "-q:v", "50"},
	},
	"prores_videotoolbox": {
		"ultrafast": {"-c:v", "prores_videotoolbox", "-realtime", "1", "-profile:v", "hq"},
		"superfast": {"-c:v", "prores_videotoolbox", "-realtime", "1", "-profile:v", "hq"},
		"veryfast":  {"-c:v", "prores_videotoolbox", "-realtime", "1", "-profile:v", "hq"},
		"faster":    {"-c:v", "prores_videotoolbox", "-realtime", "1", "-profile:v", "hq"},
		"fast":      {"-c:v", "prores_videotoolbox", "-realtime", "1", "-profile:v", "hq"},
		"medium":    {"-c:v", "prores_videotoolbox", "-realtime", "0", "-profile:v", "hq"},
		"slow":      {"-c:v", "prores_videotoolbox", "-realtime", "0", "-profile:v", "hq"},
		"slower":    {"-c:v", "prores_videotoolbox", "-realtime", "0", "-profile:v", "hq"},
		"veryslow":  {"-c:v", "prores_videotoolbox", "-realtime", "0", "-profile:v", "hq"},
	},
	"libvvenc": {
		"placebo":   {"-c:v", "libvvenc", "-preset", "slower", "-qp", "32"},
		"slowest":   {"-c:v", "libvvenc", "-preset", "slower", "-qp", "32"},
		"slower":    {"-c:v", "libvvenc", "-preset", "slower", "-qp", "32"},
		"slow":      {"-c:v", "libvvenc", "-preset", "slow", "-qp", "32"},
		"medium":    {"-c:v", "libvvenc", "-preset", "medium", "-qp", "32"},
		"fast":      {"-c:v", "libvvenc", "-preset", "fast", "-qp", "32"},
		"faster":    {"-c:v", "libvvenc", "-preset", "faster", "-qp", "32"},
		"veryfast":  {"-c:v", "libvvenc", "-preset", "faster", "-qp", "32"},
		"superfast": {"-c:v", "libvvenc", "-preset", "faster", "-qp", "32"},
		"ultrafast": {"-c:v", "libvvenc", "-preset", "faster", "-qp", "32"},
	},
	"libsvtav1": {
		"placebo":  {"-c:v", "libsvtav1", "-preset", "0", "-crf", "35"},
		"slowest":  {"-c:v", "libsvtav1", "-preset", "1", "-crf", "35"},
		"slower":   {"-c:v", "libsvtav1", "-preset", "3", "-crf", "35"},
		"slow":     {"-c:v", "libsvtav1", "-preset", "5", "-crf", "35"},
		"medium":   {"-c:v", "libsvtav1", "-preset", "7", "-crf", "35"},
		"fast":     {"-c:v", "libsvtav1", "-preset", "9", "-crf", "35"},
		"faster":   {"-c:v", "libsvtav1", "-preset", "11", "-crf", "35"},
		"veryfast": {"-c:v", "libsvtav1", "-preset", "13", "-crf", "35"},
	},
	"libvpx-vp9": {
		"placebo":   {"-c:v", "libvpx-vp9", "-deadline", "good", "-cpu-used", "0", "-crf", "32", "-b:v", "0"},
		"slowest":   {"-c:v", "libvpx-vp9", "-deadline", "good", "-cpu-used", "0", "-crf", "32", "-b:v", "0"},
		"slower":    {"-c:v", "libvpx-vp9", "-deadline", "good", "-cpu-used", "1", "-crf", "32", "-b:v", "0"},
		"slow":      {"-c:v", "libvpx-vp9", "-deadline", "good", "-cpu-used", "2", "-crf", "32", "-b:v", "0"},
		"medium":    {"-c:v", "libvpx-vp9", "-deadline", "good", "-cpu-used", "3", "-crf", "32", "-b:v", "0"},
		"fast":      {"-c:v", "libvpx-vp9", "-deadline", "good", "-cpu-used", "4", "-crf", "32", "-b:v", "0"},
		"faster":    {"-c:v", "libvpx-vp9", "-deadline", "good", "-cpu-used", "5", "-crf", "32", "-b:v", "0"},
		"veryfast":  {"-c:v", "libvpx-vp9", "-deadline", "good", "-cpu-used", "5", "-crf", "32", "-b:v", "0"},
		"superfast": {"-c:v", "libvpx-vp9", "-deadline", "good", "-cpu-used", "5", "-crf", "32", "-b:v", "0"},
		"ultrafast": {"-c:v", "libvpx-vp9", "-deadline", "good", "-cpu-used", "5", "-crf", "32", "-b:v", "0"},
	},
}

// goldenProbeArgv mirrors the Python adapters' probe_args().
var goldenProbeArgv = map[string][]string{
	"libx264":             {"-c:v", "libx264", "-preset", "ultrafast", "-crf", "28"},
	"libaom-av1":          {"-c:v", "libaom-av1", "-cpu-used", "9", "-crf", "35"},
	"libx265":             {"-c:v", "libx265", "-preset", "ultrafast", "-crf", "28"},
	"h264_nvenc":          {"-c:v", "h264_nvenc", "-preset", "p1", "-cq", "28"},
	"hevc_nvenc":          {"-c:v", "hevc_nvenc", "-preset", "p1", "-cq", "28"},
	"av1_nvenc":           {"-c:v", "av1_nvenc", "-preset", "p1", "-cq", "28"},
	"h264_amf":            {"-c:v", "h264_amf", "-quality", "speed", "-rc", "cqp", "-qp_i", "28", "-qp_p", "28"},
	"hevc_amf":            {"-c:v", "hevc_amf", "-quality", "speed", "-rc", "cqp", "-qp_i", "28", "-qp_p", "28"},
	"av1_amf":             {"-c:v", "av1_amf", "-quality", "speed", "-rc", "cqp", "-qp_i", "28", "-qp_p", "28"},
	"h264_qsv":            {"-c:v", "h264_qsv", "-preset", "veryfast", "-global_quality", "23"},
	"hevc_qsv":            {"-c:v", "hevc_qsv", "-preset", "veryfast", "-global_quality", "23"},
	"av1_qsv":             {"-c:v", "av1_qsv", "-preset", "veryfast", "-global_quality", "23"},
	"h264_videotoolbox":   {"-c:v", "h264_videotoolbox", "-realtime", "1", "-q:v", "60"},
	"hevc_videotoolbox":   {"-c:v", "hevc_videotoolbox", "-realtime", "1", "-q:v", "60"},
	"prores_videotoolbox": {"-c:v", "prores_videotoolbox", "-realtime", "1", "-profile:v", "proxy"},
	"libvvenc":            {"-c:v", "libvvenc", "-preset", "faster", "-qp", "32"},
	"libsvtav1":           {"-c:v", "libsvtav1", "-preset", "13", "-crf", "35"},
	"libvpx-vp9":          {"-c:v", "libvpx-vp9", "-deadline", "good", "-cpu-used", "5", "-crf", "32", "-b:v", "0"},
}

// goldenMeta mirrors the Python adapters' scalar contract fields.
var goldenMeta = map[string]struct {
	QualityKnob     string
	QualityLo       int
	QualityHi       int
	QualityDefault  int
	Presets         []string
	ProbePreset     string
	ProbeQuality    int
	SupportsQPFile  bool
	SupportsStats   bool
	SupportsTwoPass bool
}{
	"libx264":             {"crf", 0, 51, 23, []string{"ultrafast", "superfast", "veryfast", "faster", "fast", "medium", "slow", "slower", "veryslow"}, "ultrafast", 28, true, true, true},
	"libaom-av1":          {"crf", 0, 63, 35, []string{"placebo", "slowest", "slower", "slow", "medium", "fast", "faster", "veryfast", "superfast", "ultrafast"}, "ultrafast", 35, true, false, true},
	"libx265":             {"crf", 15, 40, 28, []string{"ultrafast", "superfast", "veryfast", "faster", "fast", "medium", "slow", "slower", "veryslow", "placebo"}, "ultrafast", 28, false, true, true},
	"h264_nvenc":          {"cq", 15, 40, 23, []string{"ultrafast", "superfast", "veryfast", "faster", "fast", "medium", "slow", "slower", "slowest", "placebo"}, "ultrafast", 28, false, false, false},
	"hevc_nvenc":          {"cq", 15, 40, 23, []string{"ultrafast", "superfast", "veryfast", "faster", "fast", "medium", "slow", "slower", "slowest", "placebo"}, "ultrafast", 28, false, false, false},
	"av1_nvenc":           {"cq", 15, 40, 23, []string{"ultrafast", "superfast", "veryfast", "faster", "fast", "medium", "slow", "slower", "slowest", "placebo"}, "ultrafast", 28, false, false, false},
	"h264_amf":            {"qp", 15, 40, 23, []string{"ultrafast", "superfast", "veryfast", "faster", "fast", "medium", "slow", "slower", "slowest", "placebo"}, "ultrafast", 28, false, false, false},
	"hevc_amf":            {"qp", 15, 40, 23, []string{"ultrafast", "superfast", "veryfast", "faster", "fast", "medium", "slow", "slower", "slowest", "placebo"}, "ultrafast", 28, false, false, false},
	"av1_amf":             {"qp", 15, 40, 23, []string{"ultrafast", "superfast", "veryfast", "faster", "fast", "medium", "slow", "slower", "slowest", "placebo"}, "ultrafast", 28, false, false, false},
	"h264_qsv":            {"global_quality", 1, 51, 23, []string{"veryslow", "slower", "slow", "medium", "fast", "faster", "veryfast"}, "veryfast", 23, false, false, false},
	"hevc_qsv":            {"global_quality", 1, 51, 23, []string{"veryslow", "slower", "slow", "medium", "fast", "faster", "veryfast"}, "veryfast", 23, false, false, false},
	"av1_qsv":             {"global_quality", 1, 51, 23, []string{"veryslow", "slower", "slow", "medium", "fast", "faster", "veryfast"}, "veryfast", 23, false, false, false},
	"h264_videotoolbox":   {"q:v", 0, 100, 50, []string{"ultrafast", "superfast", "veryfast", "faster", "fast", "medium", "slow", "slower", "veryslow"}, "ultrafast", 60, false, false, false},
	"hevc_videotoolbox":   {"q:v", 0, 100, 50, []string{"ultrafast", "superfast", "veryfast", "faster", "fast", "medium", "slow", "slower", "veryslow"}, "ultrafast", 60, false, false, false},
	"prores_videotoolbox": {"profile:v", 0, 5, 3, []string{"ultrafast", "superfast", "veryfast", "faster", "fast", "medium", "slow", "slower", "veryslow"}, "ultrafast", 0, false, false, false},
	"av1_videotoolbox":    {"q:v", 0, 100, 50, []string{"ultrafast", "superfast", "veryfast", "faster", "fast", "medium", "slow", "slower", "veryslow"}, "ultrafast", 60, false, false, false},
	"libvvenc":            {"qp", 17, 50, 32, []string{"placebo", "slowest", "slower", "slow", "medium", "fast", "faster", "veryfast", "superfast", "ultrafast"}, "faster", 32, false, false, true},
	"libsvtav1":           {"crf", 20, 50, 35, []string{"placebo", "slowest", "slower", "slow", "medium", "fast", "faster", "veryfast"}, "veryfast", 35, false, false, false},
	"libvpx-vp9":          {"crf", 0, 63, 32, []string{"placebo", "slowest", "slower", "slow", "medium", "fast", "faster", "veryfast", "superfast", "ultrafast"}, "ultrafast", 32, false, false, true},
}
