// Copyright 2016-2026 Netflix, Inc.
// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Ported from `init`, `extract`, `advance` and `flush` of
// core/src/feature/integer_motion.c.

//! The `motion` extractor state and its `Extractor` implementation.

use vmafx_fex::{
    Error, Extractor, Flush, Frame, Geometry, Host, Picture, Plane, PlaneView, Sample,
    try_filled_vec,
};

use crate::options::MotionOptions;
use crate::sad::{self, MIN_DIM};
use crate::window::{self, State, Window, c_min};

const SAD_SCORE: &core::ffi::CStr = window::SAD;
const MOTION_SCORE: &core::ffi::CStr = c"VMAF_integer_feature_motion_score";

/// State of one context: options, geometry, the vertical-pass row and where
/// the motion2 / motion3 derivation stands (C `MotionState.window_state`).
pub struct Motion {
    opts: MotionOptions,
    w: u32,
    h: u32,
    bpc: u32,
    y_row: Vec<i32>,
    window_state: State,
}

impl Motion {
    fn window(&self) -> Window {
        Window {
            blend_factor: self.opts.blend_factor,
            blend_offset: self.opts.blend_offset,
            max_val: self.opts.max_val,
            five_frame_window: self.opts.five_frame_window,
            moving_average: self.opts.moving_average,
        }
    }

    /// SAD of the luma planes of `prev` and `cur` (`s->pipeline`).
    fn sad(&mut self, prev: &Picture<'_>, cur: &Picture<'_>) -> Result<u64, Error> {
        let (w, h) = (self.w as usize, self.h as usize);
        match (prev.plane(0)?, cur.plane(0)?) {
            (Plane::U8(p), Plane::U8(c)) => {
                check_dims(&p, &c, w, h)?;
                Ok(sad::sad_8(&p, &c, &mut self.y_row))
            }
            (Plane::U16(p), Plane::U16(c)) => {
                check_dims(&p, &c, w, h)?;
                Ok(sad::sad_16(&p, &c, self.bpc, &mut self.y_row))
            }
            _ => Err(Error::InvalidArgument(c"motion: plane types differ")),
        }
    }

    /// The SAD score of frame `index` (`extract()` up to the append).
    fn score(&mut self, frame: &Frame<'_>) -> Result<f64, Error> {
        let min_idx = self.window().min_idx();
        if self.opts.force_zero || frame.index < min_idx {
            return Ok(0.0);
        }
        let prev = if self.opts.five_frame_window {
            frame.prev_prev_ref.as_ref()
        } else {
            frame.prev_ref.as_ref()
        };
        let prev = prev.ok_or(Error::InvalidArgument(c"motion: no earlier reference"))?;
        let sad = self.sad(prev, &frame.reference)?;
        let pixels = f64::from(self.w.wrapping_mul(self.h));
        let score = (sad as f64) / 256.0 / pixels * self.opts.fps_weight;
        Ok(c_min(score, self.opts.max_val))
    }
}

/// Planes must match the init geometry, or the loops would read out of bounds.
fn check_dims<T: Sample>(
    p: &PlaneView<'_, T>,
    c: &PlaneView<'_, T>,
    w: usize,
    h: usize,
) -> Result<(), Error> {
    let ok = p.width() == w && p.height() == h && c.width() == w && c.height() == h;
    if ok {
        Ok(())
    } else {
        Err(Error::InvalidArgument(
            c"motion: plane size differs from init",
        ))
    }
}

impl Extractor for Motion {
    type Options = MotionOptions;

    fn init(opts: &MotionOptions, geom: &Geometry) -> Result<Self, Error> {
        if geom.h < MIN_DIM || geom.w < MIN_DIM {
            return Err(Error::InvalidArgument(
                c"motion: frame below the 5-tap filter minimum 3x3",
            ));
        }
        let y_row = try_filled_vec(geom.w as usize, 0_i32)?;
        Ok(Self {
            opts: *opts,
            w: geom.w,
            h: geom.h,
            bpc: geom.bpc,
            y_row,
            window_state: State::default(),
        })
    }

    fn extract(&mut self, frame: &Frame<'_>, host: &mut Host<'_>) -> Result<(), Error> {
        let score = self.score(frame)?;
        host.emit(SAD_SCORE, frame.index, score)?;
        if self.opts.debug {
            host.emit(MOTION_SCORE, frame.index, score)?;
        }
        Ok(())
    }

    /// ADR-2090: motion2 / motion3 of the frames whose window the SAD scores
    /// in the collector complete (C `advance()`).
    fn advance(&mut self, host: &mut Host<'_>) -> Result<(), Error> {
        let win = self.window();
        window::advance(host, &win, &mut self.window_state)
    }

    /// The frames no advance derived (C `flush()`).
    fn flush(&mut self, host: &mut Host<'_>) -> Result<Flush, Error> {
        let win = self.window();
        window::flush(host, &win, &mut self.window_state)?;
        Ok(Flush::Done)
    }
}
