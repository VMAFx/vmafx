// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2
//
// Option names of core/src/feature/integer_motion.c (names only: defaults,
// aliases and ranges stay in the C option parser).

//! Options of the motion extractor.

use vmafx_fex::{Error, FromOptions, OptionValues};

/// Every option of the C table, read back by name.
#[derive(Clone, Copy, Debug)]
pub struct MotionOptions {
    pub force_zero: bool,
    pub blend_factor: f64,
    pub blend_offset: f64,
    pub fps_weight: f64,
    pub max_val: f64,
    pub five_frame_window: bool,
    pub moving_average: bool,
    pub debug: bool,
}

impl FromOptions for MotionOptions {
    fn from_options(opts: &OptionValues<'_>) -> Result<Self, Error> {
        Ok(Self {
            force_zero: opts.bool(c"motion_force_zero")?,
            blend_factor: opts.f64(c"motion_blend_factor")?,
            blend_offset: opts.f64(c"motion_blend_offset")?,
            fps_weight: opts.f64(c"motion_fps_weight")?,
            max_val: opts.f64(c"motion_max_val")?,
            five_frame_window: opts.bool(c"motion_five_frame_window")?,
            moving_average: opts.bool(c"motion_moving_average")?,
            debug: opts.bool(c"debug")?,
        })
    }
}
