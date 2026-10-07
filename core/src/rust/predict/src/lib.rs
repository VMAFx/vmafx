// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2
//
//! Rust model prediction (RC4 lane P, ADR-1713).
//!
//! Skeleton created by the framework PR. The lane adds `abi.rs` with the
//! entry points `vmafx_rs_model_new`, `vmafx_rs_model_predict` and
//! `vmafx_rs_model_free` (names fixed by the RC4 contract; the flat model
//! view struct is the lane's) and the only `unsafe` of this crate, inside
//! `#[allow(unsafe_code)] mod abi;`.

#![deny(unsafe_code)]
