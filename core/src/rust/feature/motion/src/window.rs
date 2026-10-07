// Copyright 2016-2026 Netflix, Inc.
// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Ported statement by statement from `motion_flush_one` and
// `vmaf_motion_window_flush` of core/src/feature/integer_motion.c and
// `motion_blend` of core/src/feature/motion_blend_tools.h.

//! Derivation of motion2 and motion3 from the per-frame SAD scores.

use core::ffi::CStr;

use vmafx_fex::{Error, Host};

/// The collector as the flush uses it: scores by decorated name and frame.
pub trait Scores {
    /// `vmaf_feature_collector_get_score`.
    fn get(&self, feature: &CStr, index: u32) -> Option<f64>;
    /// `vmaf_feature_collector_append_with_dict`.
    fn emit(&mut self, feature: &CStr, index: u32, value: f64) -> Result<(), Error>;
}

impl Scores for Host<'_> {
    fn get(&self, feature: &CStr, index: u32) -> Option<f64> {
        Host::get(self, feature, index)
    }
    fn emit(&mut self, feature: &CStr, index: u32, value: f64) -> Result<(), Error> {
        Host::emit(self, feature, index, value)
    }
}

/// SAD score key, base name as in the C `provided_features`.
pub const SAD: &CStr = c"VMAF_integer_feature_motion_sad_score";
/// motion2 key.
pub const MOTION2: &CStr = c"VMAF_integer_feature_motion2_score";
/// motion3 key.
pub const MOTION3: &CStr = c"VMAF_integer_feature_motion3_score";

/// The options that shape the window (`VmafMotionWindow`).
#[derive(Clone, Copy, Debug)]
pub struct Window {
    pub blend_factor: f64,
    pub blend_offset: f64,
    pub max_val: f64,
    pub five_frame_window: bool,
    pub moving_average: bool,
}

/// C `MIN(x, y)`: `(x < y) ? x : y`, which differs from `f64::min` on NaN.
#[inline]
pub fn c_min(x: f64, y: f64) -> f64 {
    if x < y { x } else { y }
}

/// `motion_blend()`.
#[inline]
fn blend(score: f64, factor: f64, offset: f64) -> f64 {
    let kept = score * factor;
    let rest = (1.0 - factor) * c_min(offset, score);
    kept + rest
}

impl Window {
    /// `min_idx` and `stride` of the C: 2 with the five-frame window, else 1.
    pub fn min_idx(&self) -> u32 {
        if self.five_frame_window { 2 } else { 1 }
    }

    /// `MIN(motion_blend(score), motion_max_val)`.
    fn processed(&self, score: f64) -> f64 {
        c_min(
            blend(score, self.blend_factor, self.blend_offset),
            self.max_val,
        )
    }
}

/// The score of frame `index` of the collector, or `InvalidArgument` when the
/// C would have read an unset value.
fn sad_at(host: &impl Scores, index: u32) -> Result<f64, Error> {
    host.get(SAD, index)
        .ok_or(Error::InvalidArgument(c"motion: missing SAD score"))
}

/// motion2 of frame `i` (C `motion_flush_one`, first half).
fn motion2_of(host: &impl Scores, win: &Window, i: u32) -> Result<f64, Error> {
    let min_idx = win.min_idx();
    let sad_i = sad_at(host, i)?;
    if i < min_idx {
        return Ok(0.0);
    }
    let stride = min_idx;
    let lo_idx = i64::from(i) - i64::from(stride - 1);
    let Some(hi) = host.get(SAD, i + 1) else {
        return Ok(sad_i);
    };
    if lo_idx >= i64::from(min_idx) {
        let lo = sad_at(host, lo_idx as u32)?;
        return Ok(if lo < hi { lo } else { hi });
    }
    Ok(hi)
}

/// Loop-carried state of the flush: C's `stamp_value` and `prev_processed`.
struct Carry {
    stamp: f64,
    prev_processed: f64,
}

/// motion3 of frame `i` (C `motion_flush_one`, second half).
fn motion3_of(win: &Window, carry: &mut Carry, i: u32, motion2: f64) -> f64 {
    if i < win.min_idx() {
        carry.prev_processed = carry.stamp;
        return carry.stamp;
    }
    let processed = win.processed(motion2);
    let motion3 = if win.moving_average {
        (processed + carry.prev_processed) / 2.0
    } else {
        processed
    };
    carry.prev_processed = processed;
    motion3
}

/// Number of consecutive frames, from 0, that have a SAD score.
fn frame_count(host: &impl Scores) -> u32 {
    let mut n: u32 = 0;
    while n < u32::MAX && host.get(SAD, n).is_some() {
        n += 1;
    }
    n
}

/// `vmaf_motion_window_flush()`: appends motion2 and motion3 for every frame
/// that has a SAD score. A collector without any gets nothing.
pub fn flush(host: &mut impl Scores, win: &Window) -> Result<(), Error> {
    let n = frame_count(host);
    if n == 0 {
        return Ok(());
    }
    let min_idx = win.min_idx();
    let mut carry = Carry {
        stamp: 0.0,
        prev_processed: 0.0,
    };
    if n > min_idx
        && let Some(sad) = host.get(SAD, min_idx)
    {
        carry.stamp = win.processed(sad);
    }
    for i in 0..n {
        let motion2 = motion2_of(host, win, i)?;
        host.emit(MOTION2, i, motion2)?;
        let motion3 = motion3_of(win, &mut carry, i, motion2);
        host.emit(MOTION3, i, motion3)?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A collector holding SAD scores that records what the flush appends.
    struct Fake {
        sad: Vec<f64>,
        m2: Vec<f64>,
        m3: Vec<f64>,
    }

    impl Scores for Fake {
        fn get(&self, feature: &CStr, index: u32) -> Option<f64> {
            if feature == SAD {
                self.sad.get(index as usize).copied()
            } else {
                None
            }
        }
        fn emit(&mut self, feature: &CStr, index: u32, value: f64) -> Result<(), Error> {
            let out = if feature == MOTION2 {
                &mut self.m2
            } else {
                &mut self.m3
            };
            assert_eq!(out.len(), index as usize);
            out.push(value);
            Ok(())
        }
    }

    /// `(sad, factor, offset, max, five_frame, moving_average, [(motion2, motion3)])`,
    /// evaluated with a line-by-line port of `motion_flush_one` in IEEE doubles.
    type Case = (
        &'static [f64],
        f64,
        f64,
        f64,
        bool,
        bool,
        &'static [(f64, f64)],
    );

    const CASES: &[Case] = &[
        (
            &[0.0, 10.0, 20.0, 5.0, 30.0],
            1.0,
            40.0,
            10000.0,
            false,
            false,
            &[
                (0.0, 10.0),
                (10.0, 10.0),
                (5.0, 5.0),
                (5.0, 5.0),
                (30.0, 30.0),
            ],
        ),
        (
            &[0.0, 0.0, 3.25, 9.5, 1.75, 60.0, 44.0, 2.5],
            0.7,
            40.0,
            100.0,
            true,
            true,
            &[
                (0.0, 3.25),
                (0.0, 3.25),
                (9.5, 6.375),
                (1.75, 5.625),
                (9.5, 5.625),
                (1.75, 5.625),
                (2.5, 2.125),
                (2.5, 2.5),
            ],
        ),
        (
            &[0.0, 12.5, 48.0, 3.0],
            0.3,
            5.0,
            20.0,
            false,
            true,
            &[
                (0.0, 7.25),
                (12.5, 7.25),
                (3.0, 5.125),
                (3.0, 2.9999999999999996),
            ],
        ),
    ];

    #[test]
    fn flush_matches_the_c_derivation() -> Result<(), Error> {
        for &(sad, factor, offset, max_val, five, ma, want) in CASES {
            let win = Window {
                blend_factor: factor,
                blend_offset: offset,
                max_val,
                five_frame_window: five,
                moving_average: ma,
            };
            let mut fake = Fake {
                sad: sad.to_vec(),
                m2: vec![],
                m3: vec![],
            };
            flush(&mut fake, &win)?;
            let got: Vec<(u64, u64)> = fake
                .m2
                .iter()
                .zip(&fake.m3)
                .map(|(a, b)| (a.to_bits(), b.to_bits()))
                .collect();
            let exp: Vec<(u64, u64)> = want
                .iter()
                .map(|(a, b)| (a.to_bits(), b.to_bits()))
                .collect();
            assert_eq!(got, exp);
        }
        Ok(())
    }

    #[test]
    fn flush_without_scores_appends_nothing() -> Result<(), Error> {
        let win = Window {
            blend_factor: 1.0,
            blend_offset: 40.0,
            max_val: 1.0,
            five_frame_window: false,
            moving_average: false,
        };
        let mut fake = Fake {
            sad: vec![],
            m2: vec![],
            m3: vec![],
        };
        flush(&mut fake, &win)?;
        assert!(fake.m2.is_empty() && fake.m3.is_empty());
        Ok(())
    }

    #[test]
    fn c_min_keeps_the_second_operand_on_nan() {
        assert!(c_min(f64::NAN, 1.0).to_bits() == 1.0_f64.to_bits());
        assert!(c_min(1.0, f64::NAN).is_nan());
    }
}
