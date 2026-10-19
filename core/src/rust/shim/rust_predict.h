/* SPDX-License-Identifier: EUPL-1.2 */
/* Copyright 2026 Lusoris */

/**
 * @file rust_predict.h
 * @brief Installation of the Rust model predictor (RC4 lane P, ADR-1713).
 *
 * `predict.c` keeps model loading, feature-name resolution, the score gather
 * from the feature collector and the append of the prediction. With
 * `-Denable_rust_features=true` and `VMAF_FEATURE_IMPL=rust` the prediction
 * itself (normalise, chroma correction, SVM, denormalise, transform, clip)
 * runs in `vmafx-predict`, bit-identical to the C path. `predict.c` reaches it
 * only through the `VmafRustPredictOps` table (predict.h) this file's
 * installer puts in place; rust_predict.c is a direct source of the libvmaf
 * library target, so binaries that link predict.c alone never need the Rust
 * archive.
 */

#ifndef VMAF_SRC_RUST_SHIM_RUST_PREDICT_H_
#define VMAF_SRC_RUST_SHIM_RUST_PREDICT_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Install the Rust predictor's table (vmaf_predict_install_rust_ops()).
 *
 * Idempotent and thread-safe; vmaf_init() calls it next to
 * vmaf_rust_twins_install() when HAVE_RUST_FEATURES is 1.
 */
void vmaf_rust_predict_install(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VMAF_SRC_RUST_SHIM_RUST_PREDICT_H_ */
