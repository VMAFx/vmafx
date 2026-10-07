// Copyright 2016-2026 Netflix, Inc.
// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Ported statement by statement from `motion_flush_one`,
// `motion_window_stamp`, `motion_window_count_sads`, `motion_window_derive`,
// `vmaf_motion_window_advance` and `vmaf_motion_window_flush` of
// core/src/feature/integer_motion.c and `motion_blend` of
// core/src/feature/motion_blend_tools.h.

//! Derivation of motion2 and motion3 from the per-frame SAD scores, frame by
//! frame as the SAD scores come in (ADR-2090).

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

/// Where one extractor's derivation stands (C `VmafMotionWindowState`): the
/// SAD scores of frames `0 .. n_sad` are in the collector, motion2 and
/// motion3 of frames `0 .. next` are appended. `stamp` and `prev_processed`
/// are the two values upstream's flush carries from frame to frame.
#[derive(Clone, Copy, Debug, Default)]
pub struct State {
    n_sad: u32,
    next: u32,
    stamp: f64,
    prev_processed: f64,
}

/// motion3 of frame `i` (C `motion_flush_one`, second half).
fn motion3_of(win: &Window, state: &mut State, i: u32, motion2: f64) -> f64 {
    if i < win.min_idx() {
        state.prev_processed = state.stamp;
        return state.stamp;
    }
    let processed = win.processed(motion2);
    let motion3 = if win.moving_average {
        (processed + state.prev_processed) / 2.0
    } else {
        processed
    };
    state.prev_processed = processed;
    motion3
}

/// `motion_window_stamp()`: motion3 of the frames below `min_idx`, from the
/// SAD score of frame `min_idx` when the `n` SAD scores reach past it, else 0.
fn stamp(host: &impl Scores, win: &Window, n: u32) -> f64 {
    let min_idx = win.min_idx();
    if n > min_idx
        && let Some(sad) = host.get(SAD, min_idx)
    {
        return win.processed(sad);
    }
    0.0
}

/// `motion_window_count_sads()`: extend `n_sad` over the SAD scores the
/// collector holds without a gap, resumed where the last call stopped.
fn count_sads(host: &impl Scores, state: &mut State) {
    while state.n_sad < u32::MAX && host.get(SAD, state.n_sad).is_some() {
        state.n_sad += 1;
    }
}

/// `motion_window_derive()`: frames `next .. end`, in index order, with the
/// carried values kept in `state`.
fn derive(host: &mut impl Scores, win: &Window, state: &mut State, end: u32) -> Result<(), Error> {
    if state.next == 0 && end > 0 {
        state.stamp = stamp(host, win, state.n_sad);
    }
    while state.next < end {
        let i = state.next;
        let motion2 = motion2_of(host, win, i)?;
        host.emit(MOTION2, i, motion2)?;
        let motion3 = motion3_of(win, state, i, motion2);
        host.emit(MOTION3, i, motion3)?;
        state.next += 1;
    }
    Ok(())
}

/// `vmaf_motion_window_advance()`: appends motion2 and motion3 of every frame
/// whose window is complete, frame `i` once the SAD scores of frames
/// `0 ..= max(i + 1, min_idx)` are in.
///
/// # Errors
///
/// A missing SAD score the derivation reads, or a host error.
pub fn advance(host: &mut impl Scores, win: &Window, state: &mut State) -> Result<(), Error> {
    count_sads(host, state);
    if state.n_sad <= win.min_idx() {
        return Ok(());
    }
    derive(host, win, state, state.n_sad - 1)
}

/// `vmaf_motion_window_flush()`: appends motion2 and motion3 for every frame
/// that has a SAD score and is not derived yet. A collector without any gets
/// nothing.
///
/// # Errors
///
/// A missing SAD score the derivation reads, or a host error.
pub fn flush(host: &mut impl Scores, win: &Window, state: &mut State) -> Result<(), Error> {
    count_sads(host, state);
    derive(host, win, state, state.n_sad)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A collector holding SAD scores (`None`: not appended yet) that records
    /// what the derivation appends; an append out of order or a second append
    /// of a frame fails the test.
    struct Fake {
        sad: Vec<Option<f64>>,
        m2: Vec<f64>,
        m3: Vec<f64>,
    }

    impl Fake {
        fn with_sads(sad: &[f64]) -> Self {
            Self {
                sad: sad.iter().copied().map(Some).collect(),
                m2: vec![],
                m3: vec![],
            }
        }

        fn bits(&self) -> Vec<(u64, u64)> {
            self.m2
                .iter()
                .zip(&self.m3)
                .map(|(a, b)| (a.to_bits(), b.to_bits()))
                .collect()
        }
    }

    impl Scores for Fake {
        fn get(&self, feature: &CStr, index: u32) -> Option<f64> {
            if feature == SAD {
                self.sad.get(index as usize).copied().flatten()
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

    fn window_of(case: &Case) -> Window {
        let &(_, factor, offset, max_val, five, ma, _) = case;
        Window {
            blend_factor: factor,
            blend_offset: offset,
            max_val,
            five_frame_window: five,
            moving_average: ma,
        }
    }

    fn want_bits(want: &[(f64, f64)]) -> Vec<(u64, u64)> {
        want.iter()
            .map(|(a, b)| (a.to_bits(), b.to_bits()))
            .collect()
    }

    /// Frames final once the SAD scores of frames `0 .. have` are in.
    fn final_frames(win: &Window, have: u32) -> usize {
        if have <= win.min_idx() {
            0
        } else {
            (have - 1) as usize
        }
    }

    #[test]
    fn flush_matches_the_c_derivation() -> Result<(), Error> {
        for case in CASES {
            let mut fake = Fake::with_sads(case.0);
            flush(&mut fake, &window_of(case), &mut State::default())?;
            assert_eq!(fake.bits(), want_bits(case.6));
        }
        Ok(())
    }

    /// The SAD scores arrive one by one, in `order`; an advance after each.
    fn advance_in_order(case: &Case, order: &[usize]) -> Result<(), Error> {
        let win = window_of(case);
        let mut state = State::default();
        let mut fake = Fake {
            sad: vec![None; case.0.len()],
            m2: vec![],
            m3: vec![],
        };
        for &i in order {
            fake.sad[i] = Some(case.0[i]);
            advance(&mut fake, &win, &mut state)?;
            let have = fake.sad.iter().take_while(|s| s.is_some()).count();
            assert_eq!(fake.m2.len(), final_frames(&win, have as u32));
        }
        flush(&mut fake, &win, &mut state)?;
        assert_eq!(fake.bits(), want_bits(case.6));
        Ok(())
    }

    #[test]
    fn advance_then_flush_matches_the_c_derivation() -> Result<(), Error> {
        for case in CASES {
            let n = case.0.len();
            let in_order: Vec<usize> = (0..n).collect();
            let swapped: Vec<usize> = (0..n)
                .map(|i| if (i ^ 1) < n { i ^ 1 } else { i })
                .collect();
            let reversed: Vec<usize> = (0..n).rev().collect();
            advance_in_order(case, &in_order)?;
            advance_in_order(case, &swapped)?;
            advance_in_order(case, &reversed)?;
        }
        Ok(())
    }

    #[test]
    fn advance_then_flush_equals_one_flush_on_short_streams() -> Result<(), Error> {
        let sad = [0.0, 7.5, 2.25, 11.0];
        for five in [false, true] {
            let win = Window {
                blend_factor: 0.6,
                blend_offset: 3.0,
                max_val: 9.0,
                five_frame_window: five,
                moving_average: true,
            };
            for n in 0..=sad.len() {
                let mut once = Fake::with_sads(&sad[..n]);
                flush(&mut once, &win, &mut State::default())?;
                let mut stepped = Fake {
                    sad: vec![None; n],
                    m2: vec![],
                    m3: vec![],
                };
                let mut state = State::default();
                for (i, &v) in sad[..n].iter().enumerate() {
                    stepped.sad[i] = Some(v);
                    advance(&mut stepped, &win, &mut state)?;
                }
                flush(&mut stepped, &win, &mut state)?;
                assert_eq!(stepped.bits(), once.bits());
                assert_eq!(once.m2.len(), n);
            }
        }
        Ok(())
    }

    #[test]
    fn second_flush_and_late_advance_append_nothing() -> Result<(), Error> {
        for case in CASES {
            let win = window_of(case);
            let mut state = State::default();
            let mut fake = Fake::with_sads(case.0);
            advance(&mut fake, &win, &mut state)?;
            flush(&mut fake, &win, &mut state)?;
            flush(&mut fake, &win, &mut state)?;
            advance(&mut fake, &win, &mut state)?;
            assert_eq!(fake.bits(), want_bits(case.6));
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
        let mut fake = Fake::with_sads(&[]);
        let mut state = State::default();
        advance(&mut fake, &win, &mut state)?;
        flush(&mut fake, &win, &mut state)?;
        assert!(fake.m2.is_empty() && fake.m3.is_empty());
        Ok(())
    }

    #[test]
    fn c_min_keeps_the_second_operand_on_nan() {
        assert!(c_min(f64::NAN, 1.0).to_bits() == 1.0_f64.to_bits());
        assert!(c_min(1.0, f64::NAN).is_nan());
    }
}
