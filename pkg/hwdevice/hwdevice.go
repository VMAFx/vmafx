// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

// Package hwdevice holds the Intel QSV device chain every vmafx-tune encode
// uses, the Go side of vmaftune.hw_devices and vmaftune.codec_adapters.
// _qsv_common (ADR-0601).
//
// FFmpeg's QSV bridge on Linux needs a VA-API device and a QSV device derived
// from it before the first -i, and the QSV device as the filter device, so the
// upload filter produces frames the encoder takes. With the VA-API device as
// the filter device (-filter_hw_device va, the chain first recorded) hwupload
// produces vaapi frames and the filter graph fails before the encoder opens
// (measured on an Arc A380 with the iHD driver, 2026-10-04).
package hwdevice

import (
	"os"
	"path/filepath"
	"sort"
	"strings"
)

// VAAPIDeviceEnv overrides the render node wherever a QSV encode resolves it.
const VAAPIDeviceEnv = "VMAFTUNE_VAAPI_DEVICE"

// FallbackVAAPIDevice is used when no Intel render node is discoverable.
const FallbackVAAPIDevice = "/dev/dri/renderD128"

// UploadFilter is the 8-bit QSV upload filter. QSVUploadFilter selects the
// depth-aware form, which must end the -vf chain of every QSV encode.
const UploadFilter = "format=nv12,hwupload=extra_hw_frames=64"

// QSVUploadFilter selects NV12 or P010 from the target format and output argv.
// The last output -pix_fmt overrides pixFmt; -profile:v main10 also needs P010.
// Keep this selection identical to Python's _qsv_common.qsv_upload_filter.
func QSVUploadFilter(pixFmt string, extraParams []string) string {
	profile := ""
	for i := 0; i+1 < len(extraParams); i++ {
		switch extraParams[i] {
		case "-pix_fmt", "-pix_fmt:v":
			pixFmt = extraParams[i+1]
		case "-profile:v", "-profile":
			profile = extraParams[i+1]
		}
	}
	if pixFmt == "p010le" || pixFmt == "yuv420p10le" || profile == "main10" {
		return "format=p010le,hwupload=extra_hw_frames=64"
	}
	return UploadFilter
}

const intelVendorID = "0x8086"

// NeedsQSVChain reports whether encoder is an Intel QSV encoder.
func NeedsQSVChain(encoder string) bool {
	return strings.HasSuffix(encoder, "_qsv")
}

// ResolveVAAPIDevice returns the render node a QSV encode initialises: an
// explicit requested path, else $VMAFTUNE_VAAPI_DEVICE, else the first Intel
// render node under /dev/dri (udev by-path entries first), else
// FallbackVAAPIDevice. "" and "auto" mean no explicit path.
func ResolveVAAPIDevice(requested string) string {
	return resolveIn(requested, "/dev/dri", "/sys/class/drm")
}

func resolveIn(requested, driDir, sysClassDRM string) string {
	for _, candidate := range []string{requested, os.Getenv(VAAPIDeviceEnv)} {
		if value := strings.TrimSpace(candidate); value != "" && value != "auto" {
			return value
		}
	}
	if node := discoverIntelRenderNode(driDir, sysClassDRM); node != "" {
		return node
	}
	return FallbackVAAPIDevice
}

// discoverIntelRenderNode mirrors vmaftune.hw_devices.discover_intel_vaapi_device.
func discoverIntelRenderNode(driDir, sysClassDRM string) string {
	for _, node := range candidateRenderNodes(driDir) {
		raw, err := os.ReadFile(filepath.Join(sysClassDRM, filepath.Base(node), "device", "vendor")) // #nosec G304 -- sysfs path built from a render-node name
		if err == nil && strings.ToLower(strings.TrimSpace(string(raw))) == intelVendorID {
			return node
		}
	}
	return ""
}

// candidateRenderNodes lists render nodes, stable by-path entries first.
func candidateRenderNodes(driDir string) []string {
	var out []string
	seen := map[string]bool{}
	add := func(pattern string) {
		matches, err := filepath.Glob(pattern)
		if err != nil {
			return
		}
		sort.Strings(matches)
		for _, entry := range matches {
			target, evalErr := filepath.EvalSymlinks(entry)
			if evalErr != nil {
				target = entry
			}
			if strings.HasPrefix(filepath.Base(target), "renderD") && !seen[target] {
				seen[target] = true
				out = append(out, target)
			}
		}
	}
	add(filepath.Join(driDir, "by-path", "*-render"))
	add(filepath.Join(driDir, "renderD*"))
	return out
}

// QSVInitArgs is the pre-input device argv of a QSV encode.
func QSVInitArgs(vaapiDevice string) []string {
	return []string{
		"-init_hw_device", "vaapi=va:" + vaapiDevice,
		"-init_hw_device", "qsv=qsv_dev@va",
		"-filter_hw_device", "qsv_dev",
	}
}

// AppendVideoFilter adds filter to the end of the caller's "-vf" chain, or
// prepends "-vf filter" when there is none. ffmpeg keeps only the last "-vf"
// of an output, so a second flag would drop the caller's chain (a ladder
// rung's scale) or the upload.
func AppendVideoFilter(args []string, filter string) []string {
	out := append([]string(nil), args...)
	for i := 0; i+1 < len(out); i++ {
		if out[i] == "-vf" || out[i] == "-filter:v" {
			out[i+1] = out[i+1] + "," + filter
			return out
		}
	}
	return append([]string{"-vf", filter}, out...)
}
