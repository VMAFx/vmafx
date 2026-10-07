// Copyright 2016-2026 Netflix, Inc.
// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Port of the `cambi` spatial mask of `core/src/feature/cambi.c`:
// `get_derivative_data_for_row`, `compute_dp_row`, `compute_mask_row`,
// `ceil_log2`, `get_mask_index` and `get_spatial_mask_for_index` with the
// 7x7 filter, statement by statement. The dp rows are `uint32_t` in the C;
// every add and subtract is a wrapping one here, as C unsigned arithmetic is.

/// `MASK_FILTER_SIZE`.
pub const MASK_FILTER_SIZE: u16 = 7;

/// Rows of the cyclic dp matrix (`2 * pad_size + 2`).
pub const DP_HEIGHT: usize = 2 * (MASK_FILTER_SIZE as usize >> 1) + 2;

/// Columns of the dp matrix for a plane `width` wide (`width + 2 * pad + 1`).
#[must_use]
pub const fn dp_width(width: usize) -> usize {
    width + 2 * (MASK_FILTER_SIZE as usize >> 1) + 1
}

/// `ceil_log2` (bounded 32-step loop, as the C).
fn ceil_log2(num: u32) -> u16 {
    if num == 0 {
        return 0;
    }
    let mut tmp = num - 1;
    let mut shift: u16 = 0;
    for _ in 0..32 {
        if tmp == 0 {
            break;
        }
        tmp >>= 1;
        shift += 1;
    }
    shift
}

/// `get_mask_index(width, height, filter_size)`: int arithmetic on the
/// promoted operands, the result truncated to `uint16_t`.
#[must_use]
pub fn mask_index(width: u32, height: u32, filter_size: u16) -> u16 {
    let shifted_wh = (width >> 6).wrapping_mul(height >> 6);
    let fs = i32::from(filter_size);
    let value = (fs * fs + 3 * (i32::from(ceil_log2(shifted_wh)) - 11) - 1) >> 1;
    value as u16
}

/// A plane of `width` x `height` samples at `stride` per row (read only).
#[derive(Clone, Copy)]
pub struct Plane10<'a> {
    /// Samples.
    pub data: &'a [u16],
    /// Samples per row.
    pub stride: usize,
    /// Width in use.
    pub width: usize,
    /// Height in use.
    pub height: usize,
}

/// `get_derivative_data_for_row`: 1 where a sample equals its right and lower
/// neighbours (the last column / row count as equal).
fn derivative_row(img: &Plane10<'_>, row: usize, out: &mut [u16]) {
    let (w, h, s) = (img.width, img.height, img.stride);
    let d = img.data;
    for (col, o) in out.iter_mut().enumerate().take(w) {
        let x = d[row * s + col];
        let horizontal = col == w - 1 || x == d[row * s + col + 1];
        let vertical = row == h - 1 || x == d[(row + 1) * s + col];
        *o = u16::from(horizontal && vertical);
    }
}

/// `compute_dp_row`: one row of the summed-area table as a row prefix sum
/// added to the previous dp row; past the derivative (or for an invalid one)
/// the prefix stays constant.
fn dp_row(curr: &mut [u32], prev: &[u32], deriv: &[u16], width: usize, deriv_valid: bool) {
    let pad = usize::from(MASK_FILTER_SIZE >> 1);
    let offset = pad + 1;
    let actual = if deriv_valid { width } else { 0 };
    let mut prefix: u32 = 0;
    for j in 0..actual {
        prefix = prefix.wrapping_add(u32::from(deriv[j]));
        curr[offset + j] = prev[offset + j].wrapping_add(prefix);
    }
    for j in actual..width + pad {
        curr[offset + j] = prev[offset + j].wrapping_add(prefix);
    }
}

/// `compute_mask_row`: the 7x7 box sum of zero derivatives against the
/// threshold.
fn mask_row(out: &mut [u16], bottom: &[u32], top: &[u32], width: usize, mask_index: u32) {
    let delta = 2 * usize::from(MASK_FILTER_SIZE >> 1) + 1;
    for (j, o) in out.iter_mut().enumerate().take(width) {
        let result = bottom[j + delta]
            .wrapping_add(top[j])
            .wrapping_sub(bottom[j])
            .wrapping_sub(top[j + delta]);
        *o = u16::from(result > mask_index);
    }
}

/// Row `r` of the dp matrix mutably and row `p` (`p != r`) shared.
fn dp_pair(dp: &mut [u32], width: usize, r: usize, p: usize) -> (&mut [u32], &[u32]) {
    if r < p {
        let (lo, hi) = dp.split_at_mut(p * width);
        (&mut lo[r * width..(r + 1) * width], &hi[..width])
    } else {
        let (lo, hi) = dp.split_at_mut(r * width);
        (&mut hi[..width], &lo[p * width..(p + 1) * width])
    }
}

/// `c + 1 == DP_HEIGHT ? 0 : c + 1`.
const fn next_row(c: usize) -> usize {
    if c + 1 == DP_HEIGHT { 0 } else { c + 1 }
}

/// The scratch of the spatial mask.
pub struct MaskScratch<'a> {
    /// `DP_HEIGHT * dp_width(width)` dp cells (cleared here).
    pub dp: &'a mut [u32],
    /// One derivative row (`width` samples).
    pub deriv: &'a mut [u16],
}

/// `get_spatial_mask` (`get_spatial_mask_for_index` with the 7x7 filter):
/// writes `img.height` rows of 0/1 into `mask` at `img.stride`.
pub fn spatial_mask(img: &Plane10<'_>, mask: &mut [u16], scratch: &mut MaskScratch<'_>) {
    let (w, h, s) = (img.width, img.height, img.stride);
    let index = u32::from(mask_index(w as u32, h as u32, MASK_FILTER_SIZE));
    let pad = usize::from(MASK_FILTER_SIZE >> 1);
    let dpw = dp_width(w);
    let dp = &mut scratch.dp[..dpw * DP_HEIGHT];
    dp.fill(0);
    for i in 0..pad {
        let valid = i < h;
        if valid {
            derivative_row(img, i, scratch.deriv);
        }
        let curr = i + pad + 1;
        let (c, p) = dp_pair(dp, dpw, curr, curr - 1);
        dp_row(c, p, scratch.deriv, w, valid);
    }
    let mut prev = DP_HEIGHT - 2;
    let mut curr = DP_HEIGHT - 1;
    let mut bottom = (pad + 1 + pad) % DP_HEIGHT;
    let mut top = (pad + 1 + DP_HEIGHT - pad - 1) % DP_HEIGHT;
    for i in pad..h + pad {
        let valid = i < h;
        if valid {
            derivative_row(img, i, scratch.deriv);
        }
        let (c, p) = dp_pair(dp, dpw, curr, prev);
        dp_row(c, p, scratch.deriv, w, valid);
        prev = curr;
        curr = next_row(curr);
        let row = &mut mask[(i - pad) * s..(i - pad) * s + w];
        let b = &dp[bottom * dpw..(bottom + 1) * dpw];
        let t = &dp[top * dpw..(top + 1) * dpw];
        mask_row(row, b, t, w, index);
        bottom = next_row(bottom);
        top = next_row(top);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ceil_log2_values() {
        assert_eq!(ceil_log2(0), 0);
        assert_eq!(ceil_log2(1), 0);
        assert_eq!(ceil_log2(2), 1);
        assert_eq!(ceil_log2(45), 6);
        assert_eq!(ceil_log2(u32::MAX), 32);
    }

    #[test]
    fn mask_index_values() {
        // 576x324: (9 * 5) -> ceil_log2 6 -> (49 - 15 - 1) >> 1 = 16.
        assert_eq!(mask_index(576, 324, 7), 16);
        // 3840x2160: 60 * 33 = 1980 -> 11 -> (49 + 0 - 1) >> 1 = 24.
        assert_eq!(mask_index(3840, 2160, 7), 24);
        // narrower than 64: shifted 0 -> ceil_log2 0 -> (49 - 33 - 1) >> 1 = 7.
        assert_eq!(mask_index(216, 50, 7), 7);
    }

    #[test]
    fn flat_plane_is_fully_masked() {
        let (w, h) = (16usize, 12usize);
        let data = vec![100u16; w * h];
        let img = Plane10 {
            data: &data,
            stride: w,
            width: w,
            height: h,
        };
        let mut mask = vec![0u16; w * h];
        let mut dp = vec![0u32; dp_width(w) * DP_HEIGHT];
        let mut deriv = vec![0u16; w];
        let mut scratch = MaskScratch {
            dp: &mut dp,
            deriv: &mut deriv,
        };
        spatial_mask(&img, &mut mask, &mut scratch);
        // The box sum is the number of in-frame pixels of the 7x7 window; the
        // index of a 16x12 plane is 7, so the 4x4 corner windows (16) pass too.
        assert!(mask.iter().all(|&m| m == 1));
    }
}
