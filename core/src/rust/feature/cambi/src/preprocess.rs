// Copyright 2016-2026 Netflix, Inc.
// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Port of the `cambi` preprocessing of `core/src/feature/cambi.c`:
// `validate_image*`, `decimate_generic_*_and_convert_to_10b`,
// `decimate_same_size_16b` and the scalar `anti_dithering_filter`, statement by
// statement. The output is a 10-bit plane in a buffer of `stride` samples per
// row.

use vmafx_fex::{Error, Plane, PlaneView, Sample};

/// A working plane: `width` x `height` samples at `stride` per row.
pub struct Work<'a> {
    /// Samples, row-major.
    pub data: &'a mut [u16],
    /// Samples per row of `data`.
    pub stride: usize,
    /// Width in use.
    pub width: usize,
    /// Height in use.
    pub height: usize,
}

/// `validate_image`: every luma sample at most `(1 << bpc) - 1` (8- and 16-bit
/// input is not checked, as in the C).
///
/// # Errors
///
/// `Error::InvalidArgument` for a sample above the bit depth's maximum.
pub fn validate(plane: &Plane<'_>, bpc: u32) -> Result<(), Error> {
    let ok = match plane {
        Plane::U8(view) => bpc == 8 || within(view, (1u32 << bpc) - 1),
        Plane::U16(view) => bpc == 16 || within(view, (1u32 << bpc) - 1),
    };
    if ok {
        Ok(())
    } else {
        Err(Error::InvalidArgument(
            c"Invalid input. The input contains values greater than the maximum value for its bit depth.",
        ))
    }
}

fn within<T: Sample + Into<u32>>(view: &PlaneView<'_, T>, max_val: u32) -> bool {
    (0..view.height()).all(|i| view.row(i).iter().all(|&v| v.into() <= max_val))
}

/// `vmaf_cambi_preprocessing` without the validation: resample plane 0 to
/// `out.width` x `out.height`, convert to 10 bits, and run the anti-dithering
/// filter when `enc_bitdepth < 10`.
pub fn convert(plane: &Plane<'_>, bpc: u32, out: &mut Work<'_>, enc_bitdepth: u32) {
    match plane {
        Plane::U8(view) => {
            // shift_factor = 10 - bpc; `data << shift_factor` in int.
            let shift = 10 - bpc;
            resample(view, out, |v| (u32::from(v) << shift) as u16);
        }
        Plane::U16(view) if bpc < 10 => resample(view, out, |v| (u32::from(v) << 1) as u16),
        Plane::U16(view) => {
            let shift = bpc - 10;
            let rounding = if shift == 0 { 0 } else { 1u32 << (shift - 1) };
            resample(view, out, |v| ((u32::from(v) + rounding) >> shift) as u16);
        }
    }
    if enc_bitdepth < 10 {
        anti_dithering(out);
    }
}

/// The same-size copy or the nearest-sample resize of the three
/// `decimate_generic_*` variants; `map` is the per-sample conversion.
///
/// The same-size 10-bit case is a row-by-row copy with both strides, as
/// `decimate_same_size_16b` in the C does since #2111
/// (T-CAMBI-10BIT-FULLREF-WIDE-SOURCE-ROWS-2026-10-05; the working buffer is
/// wider than the input under `full_ref` with a larger source).
fn resample<T: Sample, F: Fn(T) -> u16>(view: &PlaneView<'_, T>, out: &mut Work<'_>, map: F) {
    let (out_w, out_h) = (out.width, out.height);
    if view.width() == out_w && view.height() == out_h {
        for i in 0..out_h {
            let dst = &mut out.data[i * out.stride..i * out.stride + out_w];
            for (d, &s) in dst.iter_mut().zip(view.row(i)) {
                *d = map(s);
            }
        }
        return;
    }
    let (start_x, ratio_x) = walk(view.width(), out_w);
    let (start_y, ratio_y) = walk(view.height(), out_h);
    let mut y = start_y;
    for i in 0..out_h {
        let src = view.row(round_index(y));
        let dst = &mut out.data[i * out.stride..i * out.stride + out_w];
        let mut x = start_x;
        for d in dst.iter_mut() {
            *d = map(src[round_index(x)]);
            x += ratio_x;
        }
        y += ratio_y;
    }
}

/// `ratio = (float)in / out; start = ratio / 2 - 0.5` (the `0.5` is a double:
/// the subtraction is in double, the result stored to float).
fn walk(input: usize, output: usize) -> (f32, f32) {
    let ratio = input as f32 / output as f32;
    let start = (f64::from(ratio / 2.0) - 0.5) as f32;
    (start, ratio)
}

/// The source index of output index `i` of the resize walk (`start` plus `i`
/// additions of `ratio` in f32, as `vmaf_cambi_resize_source_indices`).
pub(crate) fn resize_index(input: usize, output: usize, i: usize) -> usize {
    let (start, ratio) = walk(input, output);
    let mut pos = start;
    for _ in 0..i {
        pos += ratio;
    }
    round_index(pos)
}

/// `(unsigned)(int)lroundf(pos)`: round half away from zero (`f32::round` is
/// `roundf`, exact), then the C conversions.
fn round_index(pos: f32) -> usize {
    pos.round() as i64 as i32 as u32 as usize
}

/// The scalar `anti_dithering_filter`, in place: 2x2 mean of each sample with
/// its right and lower neighbours (not yet overwritten), 1x2 / 2x1 means on
/// the last column / row.
fn anti_dithering(img: &mut Work<'_>) {
    let (w, h, s) = (img.width, img.height, img.stride);
    let d = &mut *img.data;
    for i in 0..h - 1 {
        for j in 0..w - 1 {
            let sum = u32::from(d[i * s + j])
                + u32::from(d[i * s + j + 1])
                + u32::from(d[(i + 1) * s + j])
                + u32::from(d[(i + 1) * s + j + 1]);
            d[i * s + j] = (sum >> 2) as u16;
        }
        let j = w - 1;
        d[i * s + j] = ((u32::from(d[i * s + j]) + u32::from(d[(i + 1) * s + j])) >> 1) as u16;
    }
    let i = h - 1;
    for j in 0..w - 1 {
        d[i * s + j] = ((u32::from(d[i * s + j]) + u32::from(d[i * s + j + 1])) >> 1) as u16;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn walk_matches_c_rounding() {
        // 576 -> 320: ratio 1.8f, start 0.4f.
        let (start, ratio) = walk(576, 320);
        assert_eq!(ratio.to_bits(), (576.0_f32 / 320.0_f32).to_bits());
        assert_eq!(
            start.to_bits(),
            ((f64::from(ratio / 2.0) - 0.5) as f32).to_bits()
        );
        assert_eq!(round_index(0.5), 1);
        assert_eq!(round_index(-0.25), 0);
        assert_eq!(round_index(2.5), 3);
    }

    #[test]
    fn anti_dithering_in_place() {
        let mut buf = [4u16, 8, 12, 16, 20, 24];
        let mut w = Work {
            data: &mut buf,
            stride: 3,
            width: 3,
            height: 2,
        };
        anti_dithering(&mut w);
        // (4+8+16+20)>>2 = 12, (8+12+20+24)>>2 = 16, (12+24)>>1 = 18, (16+20)>>1 = 18,
        // (20+24)>>1 = 22, last sample untouched.
        assert_eq!(buf, [12, 16, 18, 18, 22, 24]);
    }
}
