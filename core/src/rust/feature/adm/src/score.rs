// Copyright 2016-2020 Netflix, Inc.
// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Score finalisation of the integer ADM twin, ported statement by statement
// from core/src/feature/integer_adm.c (adm_result_finalise(),
// emit_adm_scores()), adm_score.h (vmaf_adm_floor_pair(),
// vmaf_adm_scale_ratios(), vmaf_adm3_score()) and nonfinite_score.h
// (vmaf_feature_emit_finite_scores()).

use core::ffi::CStr;

use vmafx_fex::{Error, Host};

/// What one frame produced (`AdmResult` in integer_adm.c).
#[derive(Clone, Copy, Debug, Default)]
pub struct AdmResult {
    pub score: f64,
    pub score_num: f64,
    pub score_den: f64,
    pub score_aim: f64,
    /// Per scale: `[2 s]` numerator, `[2 s + 1]` denominator.
    pub scores: [f64; 8],
}

const UNDEFINED: Error = Error::InvalidArgument(c"integer_adm: undefined or non-finite ADM score");

/// `vmaf_adm_floor_pair()`.
fn floor_pair(num: f64, den: f64, limit: f64) -> Result<(f64, f64), Error> {
    if !num.is_finite() || !den.is_finite() || !limit.is_finite() || limit < 0.0 {
        return Err(Error::InvalidArgument(
            c"integer_adm: invalid ADM reduction before floor",
        ));
    }
    Ok((
        if num < limit { 0.0 } else { num },
        if den < limit { 0.0 } else { den },
    ))
}

/// `vmaf_adm_scale_ratios()` over `N` (num, den) pairs: validate every pair
/// first, then form `den == 0 ? 1 : num / den`.
#[allow(clippy::float_cmp)]
pub fn scale_ratios<const N: usize>(pairs: &[f64], ratios: &mut [f64; N]) -> Result<(), Error> {
    for s in 0..N {
        let (num, den) = (pairs[2 * s], pairs[2 * s + 1]);
        if !num.is_finite() || !den.is_finite() {
            return Err(UNDEFINED);
        }
        if den == 0.0 {
            if num != 0.0 {
                return Err(UNDEFINED);
            }
            continue;
        }
        if !(num / den).is_finite() {
            return Err(UNDEFINED);
        }
    }
    for (s, r) in ratios.iter_mut().enumerate() {
        let (num, den) = (pairs[2 * s], pairs[2 * s + 1]);
        *r = if den == 0.0 { 1.0 } else { num / den };
    }
    Ok(())
}

/// `adm_result_finalise()`: floor, then DLM and (unclipped, ADR-1417) AIM.
pub fn finalise(
    res: &mut AdmResult,
    num: f64,
    den: f64,
    aim_num: f64,
    limit: f64,
) -> Result<(), Error> {
    let (num, den) = floor_pair(num, den, limit)?;
    let mut ratios = [0.0_f64; 2];
    scale_ratios(&[num, den, aim_num, den], &mut ratios)?;
    res.score = ratios[0];
    res.score_aim = ratios[1];
    res.score_num = num;
    res.score_den = den;
    Ok(())
}

/// `vmaf_adm3_score()` without the harmonic mean (integer_adm passes 0).
pub fn adm3(score: f64, score_aim: f64, dlm_weight: f64, min_value: f64) -> Result<f64, Error> {
    let inputs = [score, score_aim, dlm_weight, min_value];
    let non_finite = Error::NonFinite(c"integer_adm: non-finite ADM3 score");
    if inputs.iter().any(|v| !v.is_finite()) {
        return Err(non_finite);
    }
    let raw = score * dlm_weight + (1.0 - score_aim) * (1.0 - dlm_weight);
    if !raw.is_finite() {
        return Err(non_finite);
    }
    Ok(if raw > min_value { raw } else { min_value })
}

/// Names of `emit_adm_scores()`, in its order: seven always, then the debug
/// outputs.
const NAMES: [&CStr; 18] = [
    c"VMAF_integer_feature_adm2_score",
    c"VMAF_integer_feature_aim_score",
    c"VMAF_integer_feature_adm3_score",
    c"integer_adm_scale0",
    c"integer_adm_scale1",
    c"integer_adm_scale2",
    c"integer_adm_scale3",
    c"integer_adm",
    c"integer_adm_num",
    c"integer_adm_den",
    c"integer_adm_num_scale0",
    c"integer_adm_den_scale0",
    c"integer_adm_num_scale1",
    c"integer_adm_den_scale1",
    c"integer_adm_num_scale2",
    c"integer_adm_den_scale2",
    c"integer_adm_num_scale3",
    c"integer_adm_den_scale3",
];

/// Keys of the second viewing distance's seven scores (`adm_extra_view_keys`
/// in `integer_adm.c`: the base name and `VMAF_ADM_EXTRA_VIEW_KEY_SUFFIX`).
/// The C descriptor's `extend_name_dict` maps them to that distance's feature
/// names (ADR-2795).
pub const EXTRA_VIEW_NAMES: [&CStr; 7] = [
    c"VMAF_integer_feature_adm2_score:nvde",
    c"VMAF_integer_feature_aim_score:nvde",
    c"VMAF_integer_feature_adm3_score:nvde",
    c"integer_adm_scale0:nvde",
    c"integer_adm_scale1:nvde",
    c"integer_adm_scale2:nvde",
    c"integer_adm_scale3:nvde",
];

/// The values of `emit_adm_scores()`, in `NAMES` order.
fn values(r: &AdmResult, adm3: f64, scale: &[f64; 4]) -> [f64; 18] {
    let s = &r.scores;
    [
        r.score,
        r.score_aim,
        adm3,
        scale[0],
        scale[1],
        scale[2],
        scale[3],
        r.score,
        r.score_num,
        r.score_den,
        s[0],
        s[1],
        s[2],
        s[3],
        s[4],
        s[5],
        s[6],
        s[7],
    ]
}

/// `emit_adm_scores()` through `vmaf_feature_emit_finite_scores()`: every
/// value is checked before the first one is published. View 0 files the
/// primary distance's scores (with `debug`, the numerators and denominators
/// too), view 1 the second distance's seven under [`EXTRA_VIEW_NAMES`].
pub fn emit(
    host: &mut Host<'_>,
    index: u32,
    r: &AdmResult,
    adm3: f64,
    scale: &[f64; 4],
    debug: bool,
    view: usize,
) -> Result<(), Error> {
    let count = if debug && view == 0 { 18 } else { 7 };
    let v = values(r, adm3, scale);
    if v[..count].iter().any(|x| !x.is_finite()) {
        return Err(Error::NonFinite(c"integer_adm: non-finite score"));
    }
    let names: &[&CStr] = if view == 0 {
        &NAMES[..count]
    } else {
        &EXTRA_VIEW_NAMES
    };
    for (name, value) in names.iter().zip(&v[..count]) {
        host.emit(name, index, *value)?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn zero_denominator_means_one() {
        let mut r = [0.0; 2];
        assert_eq!(scale_ratios(&[0.0, 0.0, 2.0, 4.0], &mut r), Ok(()));
        assert_eq!(r, [1.0, 0.5]);
        assert!(scale_ratios(&[1.0, 0.0, 0.0, 1.0], &mut r).is_err());
    }

    #[test]
    fn adm3_blends_and_floors() {
        assert_eq!(
            adm3(0.9, 0.2, 0.7, 0.5),
            Ok(0.9 * 0.7 + (1.0 - 0.2) * (1.0 - 0.7))
        );
        assert_eq!(adm3(0.1, 0.9, 0.7, 0.5), Ok(0.5));
        assert!(adm3(f64::NAN, 0.0, 0.7, 0.5).is_err());
    }

    #[test]
    fn extra_view_keys_are_the_first_seven_names_with_the_suffix() {
        for (key, name) in EXTRA_VIEW_NAMES.iter().zip(&NAMES[..7]) {
            let mut want = name.to_bytes().to_vec();
            want.extend_from_slice(b":nvde");
            assert_eq!(key.to_bytes(), want.as_slice());
        }
    }

    #[test]
    fn floor_clears_values_below_the_limit() {
        let mut r = AdmResult::default();
        assert_eq!(finalise(&mut r, 1e-12, 1e-12, 0.0, 1e-10), Ok(()));
        assert_eq!((r.score, r.score_aim), (1.0, 1.0));
    }
}
