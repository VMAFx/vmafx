// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2
//
//! Rust extractor framework of VMAFX (ADR-1713).
//!
//! A Rust twin of a C feature extractor implements [`Extractor`] and lists
//! itself with [`twin`] in its crate's `pub const TWINS`. The C shim
//! (`core/src/rust/shim/rust_twins.cpp`) registers it as a
//! `VmafFeatureExtractor` named `<c name>_rust` that inherits the C
//! extractor's option table, provided features and flags. Rust never sees a
//! libvmaf struct: pictures, options and the feature collector cross the
//! boundary as the types in [`abi`].
//!
//! Bit-exactness rules (integer promotions, float evaluation order, no FMA,
//! libm through [`libm`]) are in `docs/development/rust-extractor-framework.md`.

pub mod abi;
pub mod error;
pub mod host;
pub mod libm;
pub mod options;
pub mod picture;
pub mod twin;

pub use abi::VmafxRsTwin;
pub use error::{Error, try_filled_vec};
pub use host::{Host, LogLevel};
pub use options::{FromOptions, OptionValues};
pub use picture::{Geometry, Picture, PixFmt, Plane, PlaneView, Sample};
pub use twin::{Extractor, Flush, Frame, twin};
