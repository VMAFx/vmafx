// Copyright 2016-2026 Netflix, Inc.
// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent
//
//! Rust twin of the integer `motion` extractor
//! (`core/src/feature/integer_motion.c`), ported statement by statement: SAD of
//! the blurred frame difference, motion2 and motion3 from the flush, the
//! five-frame window and the moving average. Scores equal the C extractor's
//! bit for bit (`scripts/ci/rust_twin_diff.py --feature motion`).
//!
//! RC4 lane M of the Rust extractor framework (ADR-1713).
//!
//! The C option table, its defaults and its ranges stay in C: this crate only
//! reads the parsed values.

#![forbid(unsafe_code)]

mod extractor;
mod options;
pub mod sad;
pub mod window;

pub use extractor::Motion;
pub use options::MotionOptions;
use vmafx_fex::{VmafxRsTwin, twin};

/// The twins this crate registers.
pub const TWINS: &[VmafxRsTwin] = &[twin::<Motion>(c"motion", c"motion_rust")];
