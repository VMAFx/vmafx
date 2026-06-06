// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// vmafx_tad — Temporal Absolute Difference feature extractor (cbindgen pilot).
//
// ADR-0707: proves the Rust-in-libvmaf integration path end-to-end.
//
// ## What this feature measures
//
// TAD (Temporal Absolute Difference) is the mean absolute difference
// of luma pixel values between the reference and distorted frames,
// normalised to the [0.0, 1.0] range by the peak luma value:
//
//   tad = (1 / (W * H * peak)) * Σ |ref_y[i,j] – dis_y[i,j]|
//
// where `peak` = 2^bpc − 1 (e.g. 255 for 8-bit, 1023 for 10-bit).
//
// Range: [0.0 … 1.0].  0.0 = identical frames.  1.0 = maximum possible
// mean difference (every pixel flipped to the opposite extreme).
//
// The metric is NOT part of the standard VMAF model set and does NOT
// affect VMAF scores — it is an additive standalone signal exposed via
// `--feature tad`.
//
// ## C ABI surface (exported via cbindgen)
//
// Three functions follow the libvmaf feature-extractor lifecycle:
//   vmafx_tad_init    — called once per sequence before any frames.
//   vmafx_tad_extract — called per frame pair; appends score to collector.
//   vmafx_tad_close   — called once after the last frame; frees state.
//
// The C wrapper `tad_rust.c` registers these as a VmafFeatureExtractor
// so users can invoke the feature via `--feature tad`.

// Every `unsafe fn` must wrap each individual unsafe operation in its own
// `unsafe {}` block so future readers can audit each operation individually.
#![deny(unsafe_op_in_unsafe_fn)]

use std::os::raw::{c_char, c_double, c_int, c_uint, c_void};

// ---------------------------------------------------------------------------
// Opaque C types forwarded by pointer — we never dereference them in Rust.
// ---------------------------------------------------------------------------

/// Opaque handle for the libvmaf picture buffer (VmafPicture).
/// We only read width, height, stride, bpc, and data[0] (luma plane).
/// The layout must match `libvmaf/include/libvmaf/picture.h`.
#[repr(C)]
pub struct VmafPicture {
    /// pix_fmt enum (ignored by TAD — we only touch luma plane 0)
    pub pix_fmt: c_int,
    /// bits per component (8 or 10 typically)
    pub bpc: c_uint,
    /// width per plane [0..3]
    pub w: [c_uint; 3],
    /// height per plane [0..3]
    pub h: [c_uint; 3],
    /// stride (in bytes) per plane [0..3]
    pub stride: [isize; 3],
    /// pixel data per plane [0..3]; plane 0 is luma
    pub data: [*mut c_void; 3],
    /// internal ref-count handle (not touched by feature extractors)
    pub ref_: *mut c_void,
    /// private extractor-context data (not touched here)
    pub priv_: *mut c_void,
}

/// Opaque handle for the libvmaf feature collector (VmafFeatureCollector).
/// The only operation performed on it from Rust is passing it through to
/// `vmaf_feature_collector_append`.
#[repr(C)]
pub struct VmafFeatureCollector {
    _opaque: [u8; 0],
}

// ---------------------------------------------------------------------------
// FFI declaration for the one libvmaf function we call back into.
// ---------------------------------------------------------------------------

unsafe extern "C" {
    /// Append a named feature score for a given frame index.
    /// Returns 0 on success, negative errno on failure.
    fn vmaf_feature_collector_append(
        feature_collector: *mut VmafFeatureCollector,
        feature_name: *const c_char,
        value: c_double,
        index: c_uint,
    ) -> c_int;
}

// ---------------------------------------------------------------------------
// Feature name constants (NUL-terminated C strings).
// ---------------------------------------------------------------------------

/// Primary score: normalised mean-absolute-difference of luma planes.
const FEATURE_TAD: &[u8] = b"tad\0";

/// Debug sub-score: raw (unnormalised) sum of absolute differences.
const FEATURE_TAD_SAD: &[u8] = b"tad_sad\0";

// ---------------------------------------------------------------------------
// Private state — allocated on init, freed on close.
// ---------------------------------------------------------------------------

/// Heap-allocated extractor state. The C wrapper stashes a pointer to this
/// in `VmafFeatureExtractor.priv` as a `*mut c_void`.
struct TadState {
    /// Cached peak luma value (2^bpc - 1) set on the first `init` call.
    peak: u32,
}

// ---------------------------------------------------------------------------
// Exported C-ABI functions (names emitted by cbindgen into vmafx_tad.h).
// ---------------------------------------------------------------------------

/// Allocate and initialise extractor state.
///
/// # Safety
/// `state_out` must be a valid pointer to a `*mut c_void` that the C wrapper
/// will store as `fex->priv`.  `bpc` must be in [1..32].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vmafx_tad_init(state_out: *mut *mut c_void, bpc: c_uint) -> c_int {
    if state_out.is_null() {
        return -22; /* EINVAL */
    }
    let peak: u32 = if bpc == 0 || bpc > 31 {
        return -22; /* EINVAL — bpc out of range */
    } else {
        (1u32 << bpc) - 1
    };

    let state = Box::new(TadState { peak });
    // SAFETY: we leak the Box intentionally; vmafx_tad_close reclaims it.
    unsafe { *state_out = Box::into_raw(state) as *mut c_void };
    0
}

/// Extract the TAD score for a single frame pair and append it to the
/// feature collector.
///
/// # Safety
/// - `state_ptr` must be the value set by `vmafx_tad_init`.
/// - `ref_pic` and `dis_pic` must be valid, fully initialised `VmafPicture`
///   structs with `data[0]` pointing to `bpc`-wide luma sample buffers.
/// - `feature_collector` must be a live `VmafFeatureCollector`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vmafx_tad_extract(
    state_ptr: *mut c_void,
    ref_pic: *const VmafPicture,
    dis_pic: *const VmafPicture,
    index: c_uint,
    feature_collector: *mut VmafFeatureCollector,
) -> c_int {
    if state_ptr.is_null() || ref_pic.is_null() || dis_pic.is_null() || feature_collector.is_null()
    {
        return -22; /* EINVAL */
    }

    let (state, ref_p, dis_p) = unsafe { (&*(state_ptr as *const TadState), &*ref_pic, &*dis_pic) };

    let w = ref_p.w[0] as usize;
    let h = ref_p.h[0] as usize;
    let peak = state.peak;

    if w == 0 || h == 0 || peak == 0 {
        return -22;
    }

    let sad: u64 = unsafe { compute_sad(ref_p, dis_p, w, h, peak) };

    // Normalise to [0.0, 1.0].
    let n_pixels = (w * h) as f64;
    let tad: f64 = sad as f64 / (n_pixels * peak as f64);

    let mut err = unsafe {
        vmaf_feature_collector_append(
            feature_collector,
            FEATURE_TAD.as_ptr() as *const c_char,
            tad,
            index,
        )
    };
    if err != 0 {
        return err;
    }

    err = unsafe {
        vmaf_feature_collector_append(
            feature_collector,
            FEATURE_TAD_SAD.as_ptr() as *const c_char,
            sad as f64,
            index,
        )
    };
    err
}

/// Free extractor state allocated by `vmafx_tad_init`.
///
/// # Safety
/// `state_ptr` must be the value set by `vmafx_tad_init`, and must not be
/// used after this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vmafx_tad_close(state_ptr: *mut c_void) -> c_int {
    if state_ptr.is_null() {
        return 0; /* nothing to do */
    }
    // SAFETY: reconstructs the Box from the raw pointer produced in init.
    unsafe { drop(Box::from_raw(state_ptr as *mut TadState)) };
    0
}

// ---------------------------------------------------------------------------
// Core computation — sum of absolute differences over the luma plane.
// ---------------------------------------------------------------------------

/// Compute the sum of absolute pixel differences for the luma plane (plane 0).
///
/// Supports 8-bit (`bpc <= 8`, data stored as `u8`) and 10/12/16-bit
/// (`bpc > 8`, data stored as `u16` little-endian native).
///
/// # Safety
/// Caller must ensure `ref_pic.data[0]` and `dis_pic.data[0]` are valid for
/// `h * stride` bytes and contain `w * h` valid samples.
unsafe fn compute_sad(
    ref_pic: &VmafPicture,
    dis_pic: &VmafPicture,
    w: usize,
    h: usize,
    peak: u32,
) -> u64 {
    let mut sad: u64 = 0;

    if peak <= 255 {
        // 8-bit path: samples are u8.
        let ref_stride = ref_pic.stride[0] as usize;
        let dis_stride = dis_pic.stride[0] as usize;
        let ref_data = ref_pic.data[0] as *const u8;
        let dis_data = dis_pic.data[0] as *const u8;

        for row in 0..h {
            for col in 0..w {
                let r = unsafe { *ref_data.add(row * ref_stride + col) } as i32;
                let d = unsafe { *dis_data.add(row * dis_stride + col) } as i32;
                sad += (r - d).unsigned_abs() as u64;
            }
        }
    } else {
        // High-bit-depth path: samples are u16 (stride is in bytes).
        let ref_stride = ref_pic.stride[0] as usize / std::mem::size_of::<u16>();
        let dis_stride = dis_pic.stride[0] as usize / std::mem::size_of::<u16>();
        let ref_data = ref_pic.data[0] as *const u16;
        let dis_data = dis_pic.data[0] as *const u16;

        for row in 0..h {
            for col in 0..w {
                let r = unsafe { *ref_data.add(row * ref_stride + col) } as i32;
                let d = unsafe { *dis_data.add(row * dis_stride + col) } as i32;
                sad += (r - d).unsigned_abs() as u64;
            }
        }
    }

    sad
}

// ---------------------------------------------------------------------------
// Rust unit tests (no C ABI needed).
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    /// Build a synthetic VmafPicture backed by a Vec<u8> luma plane.
    fn make_pic_8bit(w: usize, h: usize, fill: u8) -> (VmafPicture, Vec<u8>) {
        let buf: Vec<u8> = vec![fill; w * h];
        let pic = VmafPicture {
            pix_fmt: 1, /* YUV420P */
            bpc: 8,
            w: [w as c_uint, (w / 2) as c_uint, (w / 2) as c_uint],
            h: [h as c_uint, (h / 2) as c_uint, (h / 2) as c_uint],
            stride: [w as isize, (w / 2) as isize, (w / 2) as isize],
            data: [
                buf.as_ptr() as *mut c_void,
                std::ptr::null_mut(),
                std::ptr::null_mut(),
            ],
            ref_: std::ptr::null_mut(),
            priv_: std::ptr::null_mut(),
        };
        (pic, buf)
    }

    #[test]
    fn identical_frames_give_zero_sad() {
        let (ref_pic, _r_buf) = make_pic_8bit(4, 4, 128);
        let (dis_pic, _d_buf) = make_pic_8bit(4, 4, 128);
        let sad = unsafe { compute_sad(&ref_pic, &dis_pic, 4, 4, 255) };
        assert_eq!(sad, 0);
    }

    #[test]
    fn max_diff_gives_sad_equal_to_w_times_h_times_255() {
        // ref all-0, dis all-255 → each pixel contributes 255.
        let (ref_pic, _r_buf) = make_pic_8bit(4, 4, 0);
        let (dis_pic, _d_buf) = make_pic_8bit(4, 4, 255);
        let sad = unsafe { compute_sad(&ref_pic, &dis_pic, 4, 4, 255) };
        assert_eq!(sad, 4 * 4 * 255);
    }

    #[test]
    fn unit_diff_gives_sad_equal_to_n_pixels() {
        // ref all-100, dis all-101 → each pixel contributes 1.
        let (ref_pic, _r_buf) = make_pic_8bit(8, 8, 100);
        let (dis_pic, _d_buf) = make_pic_8bit(8, 8, 101);
        let sad = unsafe { compute_sad(&ref_pic, &dis_pic, 8, 8, 255) };
        assert_eq!(sad, 64);
    }

    #[test]
    fn normalised_tad_range_is_zero_to_one() {
        // Max diff: normalised TAD should equal 1.0.
        let (ref_pic, _r_buf) = make_pic_8bit(2, 2, 0);
        let (dis_pic, _d_buf) = make_pic_8bit(2, 2, 255);
        let sad = unsafe { compute_sad(&ref_pic, &dis_pic, 2, 2, 255) };
        let n_pixels = 4_f64;
        let tad = sad as f64 / (n_pixels * 255_f64);
        assert!((tad - 1.0).abs() < 1e-12, "expected tad ≈ 1.0, got {tad}");
    }

    #[test]
    fn mid_diff_is_proportional() {
        // ref all-0, dis all-128 → TAD ≈ 128/255 ≈ 0.502.
        let (ref_pic, _r_buf) = make_pic_8bit(4, 4, 0);
        let (dis_pic, _d_buf) = make_pic_8bit(4, 4, 128);
        let sad = unsafe { compute_sad(&ref_pic, &dis_pic, 4, 4, 255) };
        let tad = sad as f64 / (16.0 * 255.0);
        assert!((tad - 128.0 / 255.0).abs() < 1e-12);
    }
}
