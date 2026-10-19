// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2
//
// Test hooks of the `cambi` twin: C entry points through which
// `core/test/test_rust_cambi_kernels.c` compares the Rust init-time stages
// with the C functions of `core/src/feature/cambi.c` value by value. They
// take and return scalars only, so no `unsafe` block is needed; the
// `no_mangle` attribute is the crate's only unsafe item (contract section 5).

use crate::luminance::{Contrast, Eotf};
use crate::lut::{RECIPROCAL_LUT, RECIPROCAL_LUT_SIZE};
use crate::{mask, options, preprocess};

/// `reciprocal_lut[i]`; NaN past the end.
// SAFETY: the `vmafx_rs_testhook_cambi_` prefix is unique to this crate, so the
// unmangled name cannot collide with another symbol of the archive.
#[unsafe(no_mangle)]
pub extern "C" fn vmafx_rs_testhook_cambi_reciprocal(i: u32) -> f32 {
    RECIPROCAL_LUT.get(i as usize).copied().unwrap_or(f32::NAN)
}

/// `RECIPROCAL_LUT_SIZE`.
// SAFETY: unique crate-prefixed name (see above).
#[unsafe(no_mangle)]
pub extern "C" fn vmafx_rs_testhook_cambi_reciprocal_size() -> u32 {
    RECIPROCAL_LUT_SIZE as u32
}

/// Entry `slot` of the contrast tables `vmaf_cambi_init_tvi_and_vlt` builds for
/// `1 << max_log_contrast` contrasts: `tvi_for_diff[slot]` for `slot <
/// num_diffs`, `vlt_luma` for slot 32, `v_band_base` for 33, `v_band_size` for
/// 34; -1 when init refuses the values or the slot does not exist. `pq` selects
/// the PQ EOTF instead of BT.1886.
// SAFETY: unique crate-prefixed name (see above).
#[unsafe(no_mangle)]
pub extern "C" fn vmafx_rs_testhook_cambi_tables(
    max_log_contrast: u32,
    tvi_threshold: f64,
    vis_lum_threshold: f64,
    pq: u32,
    slot: u32,
) -> i32 {
    let eotf = if pq != 0 { Eotf::Pq } else { Eotf::Bt1886 };
    let Some(num_diffs) = 1u16.checked_shl(max_log_contrast).filter(|&n| n <= 32) else {
        return -1;
    };
    let Ok((c, band)) = Contrast::new(num_diffs, tvi_threshold, vis_lum_threshold, eotf) else {
        return -1;
    };
    match slot {
        s if s < u32::from(num_diffs) => i32::from(c.tvi_for_diff[s as usize]),
        32 => i32::from(c.vlt_luma),
        33 => i32::from(band.base),
        34 => i32::from(band.size),
        _ => -1,
    }
}

/// `adjust_window_size(window, w, h, speedup)`.
// SAFETY: unique crate-prefixed name (see above).
#[unsafe(no_mangle)]
pub extern "C" fn vmafx_rs_testhook_cambi_window(window: u32, w: u32, h: u32, speedup: u32) -> u32 {
    u32::from(options::adjust_window(window as u16, w, h, speedup != 0))
}

/// `get_mask_index(w, h, MASK_FILTER_SIZE)`.
// SAFETY: unique crate-prefixed name (see above).
#[unsafe(no_mangle)]
pub extern "C" fn vmafx_rs_testhook_cambi_mask_index(w: u32, h: u32) -> u32 {
    u32::from(mask::mask_index(w, h, mask::MASK_FILTER_SIZE))
}

/// The source index the nearest-sample resize reads for output index `i`
/// (`vmaf_cambi_resize_source_indices`).
// SAFETY: unique crate-prefixed name (see above).
#[unsafe(no_mangle)]
pub extern "C" fn vmafx_rs_testhook_cambi_resize_index(in_len: u32, out_len: u32, i: u32) -> u32 {
    preprocess::resize_index(in_len as usize, out_len as usize, i as usize) as u32
}
