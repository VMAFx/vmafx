// Copyright 2016-2026 Netflix, Inc.
// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Contrast-sensitivity weights of the integer ADM pipeline, ported statement by
// statement from core/src/feature/integer_adm_kernels.h (dwt_quant_step(),
// adm_csf_factors()), barten_csf_tools.h (barten_csf() and the blended tables)
// and adm_csf_fixed_point.h (adm_csf_fixed_scale(), fork code). The C
// evaluates these per stage and per frame; they depend on the options only, so
// the twin evaluates them once in init with the same libm calls.

use vmafx_fex::Error;
use vmafx_fex::libm;

use crate::tables::{
    BARTEN_MTF_A, BARTEN_MTF_B, BARTEN_PARAM_ANCHORS, BARTEN_PARAMS, BARTEN_SA, BLEND_480_3H,
    BLEND_480_3H_MAE, BLEND_480_5H, BLEND_480_5H_MAE, BLEND_720_3H, BLEND_720_3H_MAE, BLEND_720_5H,
    BLEND_720_5H_MAE, BLEND_1080_3H, BLEND_1080_3H_MAE, BLEND_1080_5H, BLEND_1080_5H_MAE,
    BLEND_2160_3H, BLEND_2160_3H_MAE, BLEND_2160_5H, BLEND_2160_5H_MAE, BlendTable,
    DWT_7_9_BASIS_AMPLITUDES, DWT_7_9_Y_THRESHOLD,
};

/// C `M_PI` (3.14159265358979323846264338327 rounds to this double).
const M_PI: f64 = core::f64::consts::PI;
/// `DEFAULT_ADM_CSF_LUM`.
const DEFAULT_ADM_CSF_LUM: f64 = 100.0;
/// `-EINVAL` returned as a float by the blended-CSF lookups.
const EINVAL_AS_FLOAT: f32 = -22.0;
/// `ADM_MIN_VIEWING_GEOMETRY`: 1080p at 3H.
const ADM_MIN_VIEWING_GEOMETRY: f64 = 3240.0;

/// `adm_csf_mode` values (`enum ADM_CSF_MODE`).
const MODE_WATSON97: i32 = 0;
const MODE_BARTEN: i32 = 1;
const MODE_BLEND: i32 = 2;
const MODE_BLEND_MAE: i32 = 3;

/// The options the weights depend on.
#[derive(Clone, Copy)]
pub struct CsfConfig {
    pub norm_view_dist: f64,
    pub ref_display_height: i32,
    pub csf_mode: i32,
    pub csf_scale: f64,
    pub csf_diag_scale: f64,
}

/// `AdmCsfFactors`: horizontal / vertical and diagonal weight of one scale.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct CsfFactors {
    pub factor1: f32,
    pub factor2: f32,
}

/// Fixed-point weights of one scale and their shared normalisation exponent.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct ScaleWeights {
    /// The float weights (`rfactor` of the denominator reductions).
    pub factors: CsfFactors,
    /// Scale 0: `uint16_t i_rfactor[3]`; scales 1..3: `uint32_t`.
    pub fixed: [u32; 3],
    /// `normalization_shift`.
    pub norm_shift: u32,
}

/// `adm_viewing_geometry_check()`.
pub fn viewing_geometry_check(cfg: &CsfConfig) -> Result<(), Error> {
    if cfg.norm_view_dist * f64::from(cfg.ref_display_height) < ADM_MIN_VIEWING_GEOMETRY {
        return Err(Error::InvalidArgument(
            c"integer_adm: viewing geometry below 1080p at 3H",
        ));
    }
    Ok(())
}

/// `dwt_quant_step()` for the luma model, as upstream evaluates it (ADR-1475).
fn dwt_quant_step(lambda: usize, theta: usize, nvd: f64, rdh: i32) -> f32 {
    let p = &DWT_7_9_Y_THRESHOLD;
    #[allow(clippy::cast_possible_truncation)]
    let r = (nvd * f64::from(rdh) * M_PI / 180.0) as f32;
    // `lambda` is a scale index (0..=3).
    #[allow(clippy::cast_precision_loss)]
    let two_pow = libm::pow(2.0, (lambda + 1) as f64);
    #[allow(clippy::cast_possible_truncation)]
    let temp = libm::log10(two_pow * f64::from(p.f0) * f64::from(p.g[theta]) / f64::from(r)) as f32;
    let amp = DWT_7_9_BASIS_AMPLITUDES[lambda][theta];
    #[allow(clippy::cast_possible_truncation)]
    let q = (2.0 * f64::from(p.a) * libm::pow(10.0, f64::from(p.k * temp * temp)) / f64::from(amp))
        as f32;
    q
}

/// `linear_interpolate()`: every operation in float (ADR-1489).
fn linear_interpolate(lp: f32, lv: f32, rp: f32, rv: f32, sp: f32) -> f32 {
    lv + ((rv - lv) / (rp - lp)) * (sp - lp)
}

/// `barten_rod_cone_sens()`.
#[allow(clippy::cast_possible_truncation)]
fn barten_rod_cone_sens(luminance_level: f32) -> f32 {
    let inner = libm::pow(
        f64::from(BARTEN_SA[1] / luminance_level),
        f64::from(BARTEN_SA[2]),
    );
    (f64::from(BARTEN_SA[0]) * libm::pow(inner + 1.0, f64::from(-BARTEN_SA[3]))) as f32
}

/// `barten_mtf()`.
#[allow(clippy::cast_possible_truncation)]
fn barten_mtf(spatial_frequency: f32) -> f32 {
    let mut mtf = 0.0_f32;
    for i in 0..4 {
        let e = libm::exp(f64::from(-BARTEN_MTF_B[i] * spatial_frequency));
        mtf = (f64::from(mtf) + f64::from(BARTEN_MTF_A[i]) * e) as f32;
    }
    mtf
}

/// C `CLAMP(x, low, high)` with float bounds promoted to double.
fn clamp_lum(x: f64) -> f64 {
    let low = f64::from(BARTEN_PARAM_ANCHORS[0]);
    let high = f64::from(BARTEN_PARAM_ANCHORS[5]);
    if x > high {
        high
    } else if x < low {
        low
    } else {
        x
    }
}

/// The interpolated HDR-VDP2 parameters p_0..p_3 at `lum` (cd/m2).
#[allow(clippy::cast_possible_truncation)]
fn barten_params_at(lum: f64) -> [f32; 4] {
    let clamped = clamp_lum(lum);
    let mut left = 0;
    let mut right = 0;
    for i in 0..5 {
        if clamped >= f64::from(BARTEN_PARAM_ANCHORS[i])
            && clamped <= f64::from(BARTEN_PARAM_ANCHORS[i + 1])
        {
            left = i;
            right = i + 1;
            break;
        }
    }
    let lp = libm::log10(f64::from(BARTEN_PARAM_ANCHORS[left])) as f32;
    let rp = libm::log10(f64::from(BARTEN_PARAM_ANCHORS[right])) as f32;
    let sp = libm::log10(clamped) as f32;
    let mut p = [0.0_f32; 4];
    for (k, out) in p.iter_mut().enumerate() {
        *out = linear_interpolate(
            lp,
            BARTEN_PARAMS[left][k + 1],
            rp,
            BARTEN_PARAMS[right][k + 1],
            sp,
        );
    }
    p
}

/// `barten_csf()` (simplified HDR-VDP2 Barten CSF), promotions as upstream
/// writes them: float products promoted for the math library, the three float
/// factors of the result multiplied in float, the double scale last.
#[allow(clippy::cast_possible_truncation)]
fn barten_csf(lambda: usize, nvd: f64, rdh: i32, lum: f64, csf_scale: f64) -> f32 {
    let r = (nvd * f64::from(rdh) * M_PI / 180.0) as f32;
    #[allow(clippy::cast_precision_loss)]
    let sf = (f64::from(r) / libm::pow(2.0, (lambda + 1) as f64)) as f32;
    let p = barten_params_at(lum);
    let a = (1.0 + libm::pow(f64::from(p[0] * sf), f64::from(p[1]))) as f32;
    let e = libm::exp(-libm::pow(f64::from(sf / 7.0), 2.0));
    let b = (1.0 / libm::pow(1.0 - e, f64::from(p[2]))) as f32;
    let csf = (f64::from(p[3]) / libm::pow(f64::from(a * b), 0.5)) as f32;
    let rod_cone = barten_rod_cone_sens(lum as f32);
    (f64::from(csf * barten_mtf(sf) * rod_cone) * csf_scale) as f32
}

/// The blended table of (`rdh`, `nvd`), or `None` (C returns `-EINVAL`).
fn blend_table(nvd: f64, rdh: i32, mae: bool) -> Option<&'static BlendTable> {
    let pick = |legacy: &'static BlendTable, fixed: &'static BlendTable| {
        Some(if mae { fixed } else { legacy })
    };
    #[allow(clippy::float_cmp)]
    let t = if (rdh == 1080 && nvd == 3.0) || (rdh == 2160 && nvd == 1.5) {
        pick(&BLEND_1080_3H, &BLEND_1080_3H_MAE)
    } else if rdh == 1080 && nvd == 5.0 {
        pick(&BLEND_1080_5H, &BLEND_1080_5H_MAE)
    } else if rdh == 2160 && nvd == 3.0 {
        pick(&BLEND_2160_3H, &BLEND_2160_3H_MAE)
    } else if rdh == 2160 && nvd == 5.0 {
        pick(&BLEND_2160_5H, &BLEND_2160_5H_MAE)
    } else {
        blend_table_small(nvd, rdh, mae)
    };
    t
}

/// The 720- and 480-line rows of `blend_table`.
fn blend_table_small(nvd: f64, rdh: i32, mae: bool) -> Option<&'static BlendTable> {
    let pick = |legacy: &'static BlendTable, fixed: &'static BlendTable| {
        Some(if mae { fixed } else { legacy })
    };
    #[allow(clippy::float_cmp)]
    let t = if rdh == 720 && nvd == 3.0 {
        pick(&BLEND_720_3H, &BLEND_720_3H_MAE)
    } else if rdh == 720 && nvd == 5.0 {
        pick(&BLEND_720_5H, &BLEND_720_5H_MAE)
    } else if rdh == 480 && nvd == 3.0 {
        pick(&BLEND_480_3H, &BLEND_480_3H_MAE)
    } else if rdh == 480 && nvd == 5.0 {
        pick(&BLEND_480_5H, &BLEND_480_5H_MAE)
    } else {
        None
    };
    t
}

/// `barten_watson_blend_csf()` / `barten_watson_blend_csf_mae()`.
fn blend_csf(scale: usize, theta: usize, nvd: f64, rdh: i32, mae: bool) -> f32 {
    blend_table(nvd, rdh, mae).map_or(EINVAL_AS_FLOAT, |t| t[theta][scale])
}

/// `adm_csf_factors()`.
pub fn csf_factors(scale: usize, cfg: &CsfConfig) -> CsfFactors {
    let (nvd, rdh) = (cfg.norm_view_dist, cfg.ref_display_height);
    match cfg.csf_mode {
        MODE_BARTEN => CsfFactors {
            factor1: barten_csf(scale, nvd, rdh, DEFAULT_ADM_CSF_LUM, cfg.csf_scale),
            factor2: barten_csf(scale, nvd, rdh, DEFAULT_ADM_CSF_LUM, cfg.csf_diag_scale),
        },
        MODE_BLEND | MODE_BLEND_MAE => {
            let mae = cfg.csf_mode == MODE_BLEND_MAE;
            CsfFactors {
                factor1: blend_csf(scale, 0, nvd, rdh, mae),
                factor2: blend_csf(scale, 1, nvd, rdh, mae),
            }
        }
        _ => CsfFactors {
            factor1: 1.0_f32 / dwt_quant_step(scale, 1, nvd, rdh),
            factor2: 1.0_f32 / dwt_quant_step(scale, 2, nvd, rdh),
        },
    }
}

/// `adm_csf_fixed_limit()`: exclusive bound of a fixed-point weight.
fn fixed_limit(scale: usize, band: usize) -> f64 {
    const EXCESS_MAX_SQ29: f64 = 1_073_741_823.0;
    const EXCESS_MAX_SQ30: f64 = 1_518_500_249.0;
    const I4_EXCESS_SLACK: f64 = 28.0;
    const BAND_MAX: [f64; 4] = [23040.0, 1_456_000_000.0, 755_000_000.0, 746_000_000.0];
    const I4_WEIGHT_SHIFT: f64 = 268_435_456.0;
    if scale == 0 {
        return if band == 2 {
            65536.0
        } else {
            EXCESS_MAX_SQ29 / BAND_MAX[0]
        };
    }
    (EXCESS_MAX_SQ30 - I4_EXCESS_SLACK) * I4_WEIGHT_SHIFT / BAND_MAX[scale]
}

/// `adm_csf_scale0_tabulated()`.
fn scale0_tabulated(cfg: &CsfConfig) -> bool {
    (cfg.norm_view_dist * f64::from(cfg.ref_display_height) - 3.0 * 1080.0).abs() < 1.0e-8
        && cfg.csf_mode == MODE_WATSON97
}

/// The unnarrowed fixed-point products of one scale (`adm_csf_scale0_fixed()`
/// and the scale 1..3 branch of `adm_csf_fixed_scale()`).
fn fixed_products(scale: usize, rf: [f32; 3], cfg: &CsfConfig) -> [f64; 3] {
    if scale == 0 {
        if scale0_tabulated(cfg) {
            return [36453.0, 36453.0, 49417.0];
        }
        return [
            f64::from(rf[0]) * libm::pow(2.0, 21.0),
            f64::from(rf[1]) * libm::pow(2.0, 21.0),
            f64::from(rf[2]) * libm::pow(2.0, 23.0),
        ];
    }
    [
        f64::from(rf[0]) * libm::pow(2.0, 32.0),
        f64::from(rf[1]) * libm::pow(2.0, 32.0),
        f64::from(rf[2]) * libm::pow(2.0, 32.0),
    ]
}

/// Most halvings a finite double can need before it is below the smallest
/// limit (2^1024 / 46603 < 2^1009); the loop below never reaches it.
const MAX_HALVINGS: u32 = 1100;

/// `adm_csf_fixed_scale()` plus the narrowing of `adm_csf_rfactor_scale0()` /
/// `adm_csf_rfactor_s123()`.
pub fn scale_weights(scale: usize, cfg: &CsfConfig) -> Result<ScaleWeights, Error> {
    viewing_geometry_check(cfg)?;
    let factors = csf_factors(scale, cfg);
    let rf = [factors.factor1, factors.factor1, factors.factor2];
    let mut fixed = fixed_products(scale, rf, cfg);
    if fixed.iter().any(|v| !v.is_finite() || *v < 0.0) {
        return Err(Error::InvalidArgument(
            c"integer_adm: adm_csf_mode yields an invalid CSF weight for this viewing geometry",
        ));
    }
    let limit = [
        fixed_limit(scale, 0),
        fixed_limit(scale, 1),
        fixed_limit(scale, 2),
    ];
    let mut norm_shift = 0_u32;
    while fixed[0] >= limit[0] || fixed[1] >= limit[1] || fixed[2] >= limit[2] {
        if norm_shift >= MAX_HALVINGS {
            return Err(Error::Range(
                c"integer_adm: CSF weight normalisation diverged",
            ));
        }
        fixed[0] *= 0.5;
        fixed[1] *= 0.5;
        fixed[2] *= 0.5;
        norm_shift += 1;
    }
    Ok(ScaleWeights {
        factors,
        fixed: narrow_fixed(scale, fixed),
        norm_shift,
    })
}

/// `(uint16_t)fixed` at scale 0, `(uint32_t)fixed` at scales 1..3: truncation;
/// every value is in [0, limit) here.
#[allow(clippy::cast_possible_truncation, clippy::cast_sign_loss)]
fn narrow_fixed(scale: usize, fixed: [f64; 3]) -> [u32; 3] {
    if scale == 0 {
        fixed.map(|v| u32::from(v as u16))
    } else {
        fixed.map(|v| v as u32)
    }
}

/// `adm_cos_1deg_sq()`: `cos(M_PI/180)^2` in double, narrowed to float. The C
/// compiler folds it; glibc's `cos` gives the same float (0x3F7FEC0A).
#[allow(clippy::cast_possible_truncation)]
pub fn cos_1deg_sq() -> f32 {
    (libm::cos(1.0 * M_PI / 180.0) * libm::cos(1.0 * M_PI / 180.0)) as f32
}

#[cfg(test)]
mod tests {
    use super::*;

    fn cfg(nvd: f64, rdh: i32, mode: i32) -> CsfConfig {
        CsfConfig {
            norm_view_dist: nvd,
            ref_display_height: rdh,
            csf_mode: mode,
            csf_scale: 1.0,
            csf_diag_scale: 1.0,
        }
    }

    #[test]
    fn watson_scale0_is_the_upstream_table() {
        let w = scale_weights(0, &cfg(3.0, 1080, 0));
        assert_eq!(
            w.map(|w| (w.fixed, w.norm_shift)),
            Ok(([36453, 36453, 49417], 0))
        );
    }

    #[test]
    fn cos_1deg_matches_the_folded_c_constant() {
        assert_eq!(cos_1deg_sq().to_bits(), 0x3F7F_EC0A);
    }

    #[test]
    fn barten_matches_the_c_reference() {
        // barten_csf(l, 3.0, 1080, 100.0, 1.0) from the C header (gcc 16, glibc).
        let expect = [0x3f9a_f18d_u32, 0x40a1_59e3, 0x416a_0875, 0x41d7_ce9b];
        for (l, e) in expect.iter().enumerate() {
            assert_eq!(
                barten_csf(l, 3.0, 1080, 100.0, 1.0).to_bits(),
                *e,
                "lambda {l}"
            );
        }
        assert_eq!(barten_rod_cone_sens(100.0).to_bits(), 0x41f0_f9c2);
    }

    #[test]
    fn unknown_blend_geometry_is_refused() {
        assert!(scale_weights(1, &cfg(4.0, 1080, 2)).is_err());
        assert!(scale_weights(1, &cfg(3.0, 1080, 2)).is_ok());
    }

    #[test]
    fn low_viewing_geometry_is_refused() {
        assert!(viewing_geometry_check(&cfg(2.0, 1080, 0)).is_err());
        assert!(viewing_geometry_check(&cfg(1.5, 2160, 0)).is_ok());
    }
}
