// Copyright 2016-2026 Netflix, Inc.
// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Port of the scalar c-values pass of `core/src/feature/cambi.c` and
// `core/src/feature/cambi.h`: the sliding histograms (`update_histogram_*`,
// `uh_slide*`, `increment_range` / `decrement_range` on `uint16_t`, so they
// wrap), `c_value_pixel`, `calculate_c_values_row` and the four passes of
// `calculate_c_values`, statement by statement and in the C's order.

use core::ops::Range;

use crate::luminance::{Band, Contrast};
use crate::lut::{CONTRAST_WEIGHTS, RECIPROCAL_LUT};
use crate::mask::Plane10;

/// The image and the mask of one scale (same stride).
#[derive(Clone, Copy)]
pub struct Scale<'a> {
    /// Mode-filtered 10-bit image.
    pub image: Plane10<'a>,
    /// Spatial mask (0/1), same geometry.
    pub mask: &'a [u16],
}

/// `increment_range`.
fn increment(cells: &mut [u16]) {
    for c in cells {
        *c = c.wrapping_add(1);
    }
}

/// `decrement_range`.
fn decrement(cells: &mut [u16]) {
    for c in cells {
        *c = c.wrapping_sub(1);
    }
}

/// The histogram rows of one scale: `band.size` rows of `width` columns.
struct Hist<'a> {
    cells: &'a mut [u16],
    width: usize,
    pad: i32,
}

impl Hist<'_> {
    /// The cells of value row `row` the window centred on column `j` covers,
    /// clamped to the frame. In the middle columns the clamp is a no-op, which
    /// makes it the C's unclamped `update_histogram_add` range there.
    fn window(&mut self, row: usize, j: i32) -> &mut [u16] {
        let w = self.width as i32;
        let left = (j - self.pad).max(0) as usize;
        let right = (j + self.pad + 1).min(w) as usize;
        &mut self.cells[row * self.width + left..row * self.width + right]
    }
}

/// The value row of sample (`row`, `col`), or `None` when it is masked out or
/// outside the band (`(uint16_t)(v - v_band_base) >= v_band_size`).
fn band_row(s: &Scale<'_>, band: Band, row: usize, col: usize) -> Option<usize> {
    let at = row * s.image.stride + col;
    if s.mask[at] == 0 {
        return None;
    }
    let off = s.image.data[at].wrapping_sub(band.base);
    (off < band.size).then_some(usize::from(off))
}

/// `update_histogram_add*` / `update_histogram_subtract*` for every column of
/// `cols`, the sample taken from image row `src`.
fn update_row(
    h: &mut Hist<'_>,
    s: &Scale<'_>,
    band: Band,
    src: usize,
    cols: Range<i32>,
    add: bool,
) {
    for j in cols {
        if let Some(r) = band_row(s, band, src, j as usize) {
            if add {
                increment(h.window(r, j));
            } else {
                decrement(h.window(r, j));
            }
        }
    }
}

/// `uh_slide` / `uh_slide_edge`: remove row `i - pad - 1`, add row `i + pad`;
/// nothing when both are the same in-band value.
fn slide_row(h: &mut Hist<'_>, s: &Scale<'_>, band: Band, i: usize, cols: Range<i32>) {
    let pad = h.pad as usize;
    for j in cols {
        let sub = band_row(s, band, i - pad - 1, j as usize);
        let add = band_row(s, band, i + pad, j as usize);
        if sub.is_some() && sub == add {
            continue;
        }
        if let Some(r) = sub {
            decrement(h.window(r, j));
        }
        if let Some(r) = add {
            increment(h.window(r, j));
        }
    }
}

/// The left edge, middle and right edge column ranges of the C loops.
fn column_ranges(width: i32, pad: i32) -> [Range<i32>; 3] {
    [
        0..pad.min(width),
        pad..width - pad - 1,
        (width - pad - 1).max(pad)..width,
    ]
}

/// The contrast tables a c-value needs.
#[derive(Clone, Copy)]
pub struct Tables<'a> {
    /// `num_diffs`, `tvi_for_diff`, `vlt_luma`.
    pub contrast: &'a Contrast,
    /// `v_band_base` / `v_band_size`.
    pub band: Band,
}

/// `c_value_pixel` for histogram column `col`.
fn c_value_pixel(hist: &[u16], width: usize, col: usize, value: u16, t: &Tables<'_>) -> f32 {
    let nd = t.contrast.num_diffs;
    let offset = t.band.base.wrapping_add(nd);
    let compact = i32::from(value) - i32::from(offset);
    if compact as u32 >= u32::from(t.band.size) {
        return 0.0;
    }
    let cell = |idx: i32| hist[idx as usize * width + col];
    let p_0 = cell(compact);
    let mut c_value = 0.0_f32;
    let per_diff = t.contrast.tvi_for_diff.iter().zip(CONTRAST_WEIGHTS);
    for (d, (&tvi, weight)) in per_diff.take(usize::from(nd)).enumerate() {
        let step = d as i32 + 1;
        if i32::from(value) <= i32::from(tvi)
            && i32::from(value) + step > i32::from(t.contrast.vlt_luma)
        {
            let p_1 = cell(compact + step);
            let idx2 = compact - step;
            let p_2 = if idx2 >= 0 { cell(idx2) } else { 0 };
            let p_m = if p_1 > p_2 { p_1 } else { p_2 };
            let product = weight * i32::from(p_0) * i32::from(p_m);
            let val = product as f32 * RECIPROCAL_LUT[usize::from(p_m) + usize::from(p_0)];
            if val > c_value {
                c_value = val;
            }
        }
    }
    c_value
}

/// `calculate_c_values_row`: the c-value of every masked sample of `row`.
fn c_values_row(c_values: &mut [f32], hist: &[u16], s: &Scale<'_>, row: usize, t: &Tables<'_>) {
    let w = s.image.width;
    let nd = t.contrast.num_diffs;
    let base = row * s.image.stride;
    for col in 0..w {
        if s.mask[base + col] != 0 {
            // `image + num_diffs` (int) passed as uint16_t.
            let value = s.image.data[base + col].wrapping_add(nd);
            c_values[row * w + col] = c_value_pixel(hist, w, col, value, t);
        }
    }
}

/// `calculate_c_values`: c-values of one scale into `c_values[..w * h]`,
/// `hist` holding at least `band.size * w` cells.
pub fn calculate(
    c_values: &mut [f32],
    hist: &mut [u16],
    s: &Scale<'_>,
    window: u16,
    t: &Tables<'_>,
) {
    let (w, h) = (s.image.width, s.image.height);
    c_values[..w * h].fill(0.0);
    let cells = &mut hist[..w * usize::from(t.band.size)];
    cells.fill(0);
    let mut hist = Hist {
        cells,
        width: w,
        pad: i32::from(window >> 1),
    };
    first_pass(&mut hist, s, t.band);
    top_edge(&mut hist, c_values, s, t);
    middle(&mut hist, c_values, s, t);
    bottom_edge(&mut hist, c_values, s, t);
}

/// `c_values_first_pass`: rows `0 .. min(pad, h)` into the histograms.
fn first_pass(h: &mut Hist<'_>, s: &Scale<'_>, band: Band) {
    let (w, ht) = (s.image.width as i32, s.image.height as i32);
    for i in 0..h.pad.min(ht) {
        for cols in column_ranges(w, h.pad) {
            update_row(h, s, band, i as usize, cols, true);
        }
    }
}

/// `c_values_top_edge`: rows `0 .. min(pad + 1, h)`, adding row `i + pad`.
fn top_edge(h: &mut Hist<'_>, c_values: &mut [f32], s: &Scale<'_>, t: &Tables<'_>) {
    let (w, ht) = (s.image.width as i32, s.image.height as i32);
    for i in 0..(h.pad + 1).min(ht) {
        if i + h.pad < ht {
            for cols in column_ranges(w, h.pad) {
                update_row(h, s, t.band, (i + h.pad) as usize, cols, true);
            }
        }
        c_values_row(c_values, h.cells, s, i as usize, t);
    }
}

/// `c_values_middle_slide`: rows `pad + 1 .. h - pad`.
fn middle(h: &mut Hist<'_>, c_values: &mut [f32], s: &Scale<'_>, t: &Tables<'_>) {
    let (w, ht) = (s.image.width as i32, s.image.height as i32);
    for i in h.pad + 1..ht - h.pad {
        for cols in column_ranges(w, h.pad) {
            slide_row(h, s, t.band, i as usize, cols);
        }
        c_values_row(c_values, h.cells, s, i as usize, t);
    }
}

/// `c_values_bottom_edge`: rows `max(h - pad, 0) .. h`, removing row
/// `i - pad - 1` when it exists.
fn bottom_edge(h: &mut Hist<'_>, c_values: &mut [f32], s: &Scale<'_>, t: &Tables<'_>) {
    let (w, ht) = (s.image.width as i32, s.image.height as i32);
    for i in (ht - h.pad).max(0)..ht {
        // C: if (i - pad_size - 1 >= 0)
        if i > h.pad {
            for cols in column_ranges(w, h.pad) {
                update_row(h, s, t.band, (i - h.pad - 1) as usize, cols, false);
            }
        }
        c_values_row(c_values, h.cells, s, i as usize, t);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn column_ranges_cover_each_column_once() {
        for (w, pad) in [(1, 4), (3, 4), (8, 4), (9, 4), (10, 4), (40, 16)] {
            let mut seen = vec![0; w as usize];
            for r in column_ranges(w, pad) {
                for j in r {
                    seen[j as usize] += 1;
                }
            }
            assert!(seen.iter().all(|&n| n == 1), "w {w} pad {pad}");
        }
    }

    #[test]
    fn c_value_of_two_level_window() {
        // num_diffs 1: contrast 1, weight 1. Value row r and r + 1 with counts 3 and 5:
        // 1 * 3 * 5 * lut[8] = 15 * 0.125 = 1.875.
        let contrast = Contrast {
            num_diffs: 1,
            tvi_for_diff: [100; 32],
            vlt_luma: 0,
        };
        let band = Band { base: 0, size: 64 };
        let t = Tables {
            contrast: &contrast,
            band,
        };
        let mut hist = vec![0u16; 64];
        hist[10] = 3;
        hist[11] = 5;
        // value = image + num_diffs; compact = value - (base + num_diffs) = 10.
        let v = c_value_pixel(&hist, 1, 0, 11, &t);
        assert_eq!(v.to_bits(), 1.875_f32.to_bits());
    }
}
