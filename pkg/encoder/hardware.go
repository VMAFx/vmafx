// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// pkg/encoder/hardware.go — hardware and AV1 encoder implementations.
//
// Stage 1 constraints (per ADR-0713):
//   - Hardware encoders (NVENC, QSV, AMF) are wired through the same
//     runEncode helper as the software encoders.
//   - All hardware paths require the GPU driver and the corresponding ffmpeg
//     HW-accel plugin to be present in the container image.
//   - QSV requires VA-API device initialisation (ADR-0601); the init chain
//     is injected via ExtraArgs when the VMAFTUNE_VAAPI_DEVICE env var is set.
//   - AV1 encoders (libsvtav1, libaom-av1) are treated as software encoders
//     but shipped here because they are AOM ecosystem, not libx26x.
//
// ADR-0601: vmaf-tune QSV/AMF hw-init + probe fix.
// ADR-0713: vmafx-node Go worker binary.

package encoder

import (
	"fmt"
	"os"
	"strings"
)

// ---------------------------------------------------------------------------
// NVENC
// ---------------------------------------------------------------------------

// H264NVENCEncoder implements Encoder for h264_nvenc.
type H264NVENCEncoder struct{}

// Name returns "h264_nvenc".
func (e H264NVENCEncoder) Name() string { return "h264_nvenc" }

// CRFRange returns [0, 51] — NVENC uses the same -cq range as CRF semantics.
func (e H264NVENCEncoder) CRFRange() (int, int) { return 0, 51 }

// Encode encodes src using NVENC H.264 at the given quality level.
// NVENC uses -cq instead of -crf.
func (e H264NVENCEncoder) Encode(src string, params EncodeParams) (EncodeResult, error) {
	return runEncode(src, params, "h264_nvenc", "-cq")
}

// HEVCNVENCEncoder implements Encoder for hevc_nvenc.
type HEVCNVENCEncoder struct{}

// Name returns "hevc_nvenc".
func (e HEVCNVENCEncoder) Name() string { return "hevc_nvenc" }

// CRFRange returns [0, 51].
func (e HEVCNVENCEncoder) CRFRange() (int, int) { return 0, 51 }

// Encode encodes src using NVENC HEVC at the given quality level.
func (e HEVCNVENCEncoder) Encode(src string, params EncodeParams) (EncodeResult, error) {
	return runEncode(src, params, "hevc_nvenc", "-cq")
}

// ---------------------------------------------------------------------------
// QSV (Intel Quick Sync Video)
// ---------------------------------------------------------------------------

// H264QSVEncoder implements Encoder for h264_qsv.
// Per ADR-0601, QSV encodes require a VA-API device chain initialised before
// the input.  When VMAFTUNE_VAAPI_DEVICE is set the init chain is injected as
// ExtraArgs at the beginning of the ffmpeg command.
type H264QSVEncoder struct{}

// Name returns "h264_qsv".
func (e H264QSVEncoder) Name() string { return "h264_qsv" }

// CRFRange returns [1, 51] — QSV global_quality range.
func (e H264QSVEncoder) CRFRange() (int, int) { return 1, 51 }

// Encode encodes src using Intel QSV H.264.
func (e H264QSVEncoder) Encode(src string, params EncodeParams) (EncodeResult, error) {
	params = injectQSVInitChain(params)
	return runEncode(src, params, "h264_qsv", "-global_quality")
}

// HEVCQSVEncoder implements Encoder for hevc_qsv.
type HEVCQSVEncoder struct{}

// Name returns "hevc_qsv".
func (e HEVCQSVEncoder) Name() string { return "hevc_qsv" }

// CRFRange returns [1, 51].
func (e HEVCQSVEncoder) CRFRange() (int, int) { return 1, 51 }

// Encode encodes src using Intel QSV HEVC.
func (e HEVCQSVEncoder) Encode(src string, params EncodeParams) (EncodeResult, error) {
	params = injectQSVInitChain(params)
	return runEncode(src, params, "hevc_qsv", "-global_quality")
}

// injectQSVInitChain prepends the VA-API device init chain to ExtraArgs when
// VMAFTUNE_VAAPI_DEVICE is set (typically /dev/dri/renderD128).
func injectQSVInitChain(params EncodeParams) EncodeParams {
	vaapiDev := os.Getenv("VMAFTUNE_VAAPI_DEVICE")
	if vaapiDev == "" {
		vaapiDev = "/dev/dri/renderD128"
	}
	// Insert before -c:v:
	//   -init_hw_device vaapi=va:<device>
	//   -init_hw_device qsv=qsv_dev@va
	//   -filter_hw_device va
	//   -vf format=nv12,hwupload=extra_hw_frames=64
	extra := []string{
		"-init_hw_device", "vaapi=va:" + vaapiDev,
		"-init_hw_device", "qsv=qsv_dev@va",
		"-filter_hw_device", "va",
		"-vf", "format=nv12,hwupload=extra_hw_frames=64",
	}
	params.ExtraArgs = append(extra, params.ExtraArgs...)
	return params
}

// ---------------------------------------------------------------------------
// AMF (AMD Advanced Media Framework)
// ---------------------------------------------------------------------------

// H264AMFEncoder implements Encoder for h264_amf.
type H264AMFEncoder struct{}

// Name returns "h264_amf".
func (e H264AMFEncoder) Name() string { return "h264_amf" }

// CRFRange returns [0, 51].
func (e H264AMFEncoder) CRFRange() (int, int) { return 0, 51 }

// Encode encodes src using AMD AMF H.264.  AMF uses -qp_i/-qp_p/-qp_b for
// quality control; we map CRF to the average QP via ExtraArgs.
func (e H264AMFEncoder) Encode(src string, params EncodeParams) (EncodeResult, error) {
	return runEncode(src, params, "h264_amf", "-qp_i")
}

// HEVCAMFEncoder implements Encoder for hevc_amf.
type HEVCAMFEncoder struct{}

// Name returns "hevc_amf".
func (e HEVCAMFEncoder) Name() string { return "hevc_amf" }

// CRFRange returns [0, 51].
func (e HEVCAMFEncoder) CRFRange() (int, int) { return 0, 51 }

// Encode encodes src using AMD AMF HEVC.
func (e HEVCAMFEncoder) Encode(src string, params EncodeParams) (EncodeResult, error) {
	return runEncode(src, params, "hevc_amf", "-qp_i")
}

// ---------------------------------------------------------------------------
// AV1 software encoders
// ---------------------------------------------------------------------------

// LibSVTAV1Encoder implements Encoder for libsvtav1.
type LibSVTAV1Encoder struct{}

// Name returns "libsvtav1".
func (e LibSVTAV1Encoder) Name() string { return "libsvtav1" }

// CRFRange returns [0, 63] — SVT-AV1 CRF range.
func (e LibSVTAV1Encoder) CRFRange() (int, int) { return 0, 63 }

// Encode encodes src using SVT-AV1.
func (e LibSVTAV1Encoder) Encode(src string, params EncodeParams) (EncodeResult, error) {
	return runEncode(src, params, "libsvtav1", "-crf")
}

// LibAOMEncoder implements Encoder for libaom-av1.
type LibAOMEncoder struct{}

// Name returns "libaom-av1".
func (e LibAOMEncoder) Name() string { return "libaom-av1" }

// CRFRange returns [0, 63] — libaom CRF range.
func (e LibAOMEncoder) CRFRange() (int, int) { return 0, 63 }

// Encode encodes src using libaom-av1.  libaom is slow by default; Stage 1
// tests inject -cpu-used 8 via ExtraArgs to keep runtime tolerable.
func (e LibAOMEncoder) Encode(src string, params EncodeParams) (EncodeResult, error) {
	return runEncode(src, params, "libaom-av1", "-crf")
}

// ---------------------------------------------------------------------------
// Extended New factory
// ---------------------------------------------------------------------------

// NewExtended returns an Encoder for the given codec name, covering both
// Stage-1 software and Stage-2 hardware encoders.  Returns an error if
// codecName is not in AllKnownEncoders.
func NewExtended(codec string) (Encoder, error) {
	// Try the Stage-1 factory first.
	if enc, err := New(codec); err == nil {
		return enc, nil
	}
	switch codec {
	case "h264_nvenc":
		return H264NVENCEncoder{}, nil
	case "hevc_nvenc":
		return HEVCNVENCEncoder{}, nil
	case "h264_qsv":
		return H264QSVEncoder{}, nil
	case "hevc_qsv":
		return HEVCQSVEncoder{}, nil
	case "h264_amf":
		return H264AMFEncoder{}, nil
	case "hevc_amf":
		return HEVCAMFEncoder{}, nil
	case "libsvtav1":
		return LibSVTAV1Encoder{}, nil
	case "libaom-av1":
		return LibAOMEncoder{}, nil
	default:
		return nil, fmt.Errorf("unknown encoder %q (supported: %s)",
			codec, strings.Join(AllKnownEncoders(), ", "))
	}
}
