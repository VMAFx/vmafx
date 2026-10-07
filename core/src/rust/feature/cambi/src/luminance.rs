// Copyright 2016-2026 Netflix, Inc.
// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Port of the luminance model `cambi` uses: `core/src/feature/luminance_tools.cpp`
// (10-bit limited range, BT.1886 and PQ EOTFs) and the visibility searches of
// `core/src/feature/cambi.c` (`tvi_condition`, `tvi_hard_threshold_condition`,
// `get_tvi_for_diff`, `get_vlt_luma`, `vmaf_cambi_init_tvi_and_vlt`), statement
// by statement. `pow` is the platform libm's, as in the C.

use vmafx_fex::Error;
use vmafx_fex::libm::pow;

/// `CAMBI_TVI_SEARCH_STEPS`.
const TVI_SEARCH_STEPS: i32 = 16;

/// The EOTF the visibility thresholds use (`vmaf_luminance_init_eotf`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Eotf {
    /// `"bt1886"`.
    Bt1886,
    /// `"pq"`.
    Pq,
}

impl Eotf {
    /// `vmaf_luminance_init_eotf`: `"bt1886"` or `"pq"`, else `-EINVAL`.
    ///
    /// # Errors
    ///
    /// `Error::InvalidArgument` for any other name.
    pub fn from_name(name: &[u8]) -> Result<Self, Error> {
        match name {
            b"bt1886" => Ok(Self::Bt1886),
            b"pq" => Ok(Self::Pq),
            _ => Err(Error::InvalidArgument(c"unknown EOTF received")),
        }
    }

    /// The EOTF of a normalised code value.
    fn apply(self, v: f64) -> f64 {
        match self {
            Self::Bt1886 => bt1886_eotf(v),
            Self::Pq => pq_eotf(v),
        }
    }
}

/// `std::max(x, 0.0)` (`x < 0.0 ? 0.0 : x`).
fn max_zero(x: f64) -> f64 {
    if x < 0.0 { 0.0 } else { x }
}

/// `vmaf_luminance_bt1886_eotf`, every `std::pow` a libm `pow` call.
fn bt1886_eotf(v: f64) -> f64 {
    let gamma = 2.4_f64;
    let lw = 300.0_f64;
    let lb = 0.01_f64;
    let a = pow(pow(lw, 1.0 / gamma) - pow(lb, 1.0 / gamma), gamma);
    let b = pow(lb, 1.0 / gamma) / (pow(lw, 1.0 / gamma) - pow(lb, 1.0 / gamma));
    a * pow(max_zero(v + b), gamma)
}

/// `vmaf_luminance_pq_eotf`.
fn pq_eotf(v: f64) -> f64 {
    let m_1 = 0.159_301_757_812_5_f64;
    let m_2 = 78.843_75_f64;
    let c_1 = 0.835_937_5_f64;
    let c_2 = 18.851_562_5_f64;
    let c_3 = 18.6875_f64;
    let num = pow(v, 1.0 / m_2) - c_1;
    let num_clipped = max_zero(num);
    let den = c_2 - c_3 * pow(v, 1.0 / m_2);
    10000.0 * pow(num_clipped / den, 1.0 / m_1)
}

/// `VmafLumaRange` of 10-bit limited range (`range_foot_head(10, LIMITED)`).
#[derive(Clone, Copy, Debug)]
pub struct LumaRange {
    foot: i32,
    head: i32,
}

impl LumaRange {
    /// `vmaf_luminance_init_luma_range(&r, 10, VMAF_PIXEL_RANGE_LIMITED)`.
    #[must_use]
    pub const fn limited_10bit() -> Self {
        let bitdepth = 10;
        Self {
            foot: 16 * (1 << (bitdepth - 8)),
            head: 235 * (1 << (bitdepth - 8)),
        }
    }

    /// `vmaf_luminance_get_luminance`: clamp, normalise, EOTF.
    fn luminance(self, sample: i32, eotf: Eotf) -> f64 {
        let clipped = sample.clamp(self.foot, self.head);
        let normalized = f64::from(clipped - self.foot) / f64::from(self.head - self.foot);
        eotf.apply(normalized)
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Bisect {
    TooSmall,
    Correct,
    TooBig,
}

/// `tvi_condition`.
fn tvi_condition(sample: i32, diff: i32, tvi: f64, range: LumaRange, eotf: Eotf) -> bool {
    let mean_luminance = range.luminance(sample, eotf);
    let diff_luminance = range.luminance(sample + diff, eotf);
    let delta_luminance = diff_luminance - mean_luminance;
    delta_luminance > tvi * mean_luminance
}

/// `tvi_hard_threshold_condition`.
fn tvi_hard_threshold(sample: i32, diff: i32, tvi: f64, range: LumaRange, eotf: Eotf) -> Bisect {
    if !tvi_condition(sample, diff, tvi, range, eotf) {
        return Bisect::TooBig;
    }
    if tvi_condition(sample + 1, diff, tvi, range, eotf) {
        return Bisect::TooSmall;
    }
    Bisect::Correct
}

/// `get_tvi_for_diff` at `bitdepth` 10.
fn get_tvi_for_diff(diff: i32, tvi: f64, range: LumaRange, eotf: Eotf) -> i32 {
    let bitdepth = 10;
    let max_val = (1 << bitdepth) - 1;
    let mut foot = range.foot;
    let mut head = range.head - diff - 1;

    match tvi_hard_threshold(foot, diff, tvi, range, eotf) {
        Bisect::TooBig => return 0,
        Bisect::Correct => return foot,
        Bisect::TooSmall => {}
    }
    match tvi_hard_threshold(head, diff, tvi, range, eotf) {
        Bisect::TooSmall => return max_val,
        Bisect::Correct => return head,
        Bisect::TooBig => {}
    }
    for _ in 0..TVI_SEARCH_STEPS {
        let mid = foot + (head - foot) / 2;
        match tvi_hard_threshold(mid, diff, tvi, range, eotf) {
            Bisect::TooBig => head = mid,
            Bisect::TooSmall => foot = mid,
            Bisect::Correct => return mid,
        }
    }
    foot
}

/// `get_vlt_luma`: the smallest luma at or above the visibility threshold.
fn get_vlt_luma(threshold: f64, range: LumaRange, eotf: Eotf) -> i32 {
    let first = range.foot.unsigned_abs();
    for sample in first..=u32::from(u16::MAX) {
        // C: vmaf_luminance_get_luminance((uint16_t)sample, ...); sample <= 65535.
        let as_int = i32::from(u16::try_from(sample).unwrap_or(u16::MAX));
        if range.luminance(as_int, eotf) >= threshold {
            return if sample == first { 0 } else { as_int };
        }
    }
    i32::from(u16::MAX)
}

/// The contrast tables and luminance cut-offs of one `cambi` instance
/// (`set_contrast_arrays` + `vmaf_cambi_init_tvi_and_vlt`).
#[derive(Clone, Copy, Debug)]
pub struct Contrast {
    /// `num_diffs` = `1 << max_log_contrast`.
    pub num_diffs: u16,
    /// `tvi_for_diff[d]`, already offset by `num_diffs`.
    pub tvi_for_diff: [u16; 32],
    /// `vlt_luma`.
    pub vlt_luma: u16,
}

/// The value band of the c-value histograms (`v_band_base`, `v_band_size`).
#[derive(Clone, Copy, Debug)]
pub struct Band {
    /// `v_band_base`.
    pub base: u16,
    /// `v_band_size`.
    pub size: u16,
}

impl Contrast {
    /// `vmaf_cambi_init_tvi_and_vlt` for `num_diffs` contrasts.
    ///
    /// # Errors
    ///
    /// `Error::InvalidArgument` when the value band is empty, as the C.
    pub fn new(num_diffs: u16, tvi: f64, vis_lum: f64, eotf: Eotf) -> Result<(Self, Band), Error> {
        let range = LumaRange::limited_10bit();
        let mut tvi_for_diff = [0u16; 32];
        for (d, slot) in tvi_for_diff
            .iter_mut()
            .enumerate()
            .take(usize::from(num_diffs))
        {
            // diffs_to_consider[d] = d + 1; d < 32.
            let diff = i32::try_from(d).map_or(0, |v| v + 1);
            // C: (uint16_t)get_tvi_for_diff(...) then += (uint16_t)num_diffs, both truncating.
            let base = truncate_u16(get_tvi_for_diff(diff, tvi, range, eotf));
            *slot = base.wrapping_add(num_diffs);
        }
        let vlt = truncate_u16(get_vlt_luma(vis_lum, range, eotf));
        let contrast = Self {
            num_diffs,
            tvi_for_diff,
            vlt_luma: vlt,
        };
        Ok((contrast, contrast.band()?))
    }

    /// `v_band_base` / `v_band_size`; `-EINVAL` when the band is empty.
    fn band(&self) -> Result<Band, Error> {
        let nd = i32::from(self.num_diffs);
        let v_lo_signed = i32::from(self.vlt_luma) - 3 * nd + 1;
        let base = if v_lo_signed > 0 {
            truncate_u16(v_lo_signed)
        } else {
            0
        };
        let last = self.tvi_for_diff[usize::from(self.num_diffs) - 1];
        let size_signed = i32::from(last) + 1 - i32::from(base);
        if size_signed <= 0 {
            return Err(Error::InvalidArgument(
                c"v_band_size underflow; cambi_vis_lum_threshold may be too low",
            ));
        }
        Ok(Band {
            base,
            size: truncate_u16(size_signed),
        })
    }
}

/// C `(uint16_t)` of an int (keeps the low 16 bits).
pub const fn truncate_u16(v: i32) -> u16 {
    v as u16
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn limited_range_10bit() {
        let r = LumaRange::limited_10bit();
        assert_eq!((r.foot, r.head), (64, 940));
    }

    #[test]
    fn eotf_names() {
        assert_eq!(Eotf::from_name(b"bt1886"), Ok(Eotf::Bt1886));
        assert_eq!(Eotf::from_name(b"pq"), Ok(Eotf::Pq));
        assert!(Eotf::from_name(b"srgb").is_err());
    }

    #[test]
    fn bt1886_constants_match_gcc() {
        // a and b as GCC folds them in luminance_tools.cpp (equal to glibc pow at run time).
        let a = f64::from_bits(0x4072_2476_f497_003b);
        let b = f64::from_bits(0x3f8c_4d6a_be7f_d91b);
        assert_eq!(bt1886_eotf(0.0).to_bits(), (a * pow(b, 2.4)).to_bits());
    }
}
