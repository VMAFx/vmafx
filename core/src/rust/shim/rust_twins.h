/**
 * Copyright 2026 Lusoris
 * SPDX-License-Identifier: EUPL-1.2
 *
 * rust_twins.h - registration of the Rust extractors (ADR-1713).
 *
 * Compiled only when meson is configured with -Denable_rust_features=true
 * (HAVE_RUST_FEATURES). The shim reads the registry of the Rust archive
 * (vmafx_rs_twin_at(), core/src/rust/include/vmafx_rs.h), builds one
 * VmafFeatureExtractor per Rust twin from the C extractor it replaces, and
 * installs them, together with the TAD pilot, into the extractor registry of
 * feature_extractor.cpp.
 */

#ifndef VMAF_RUST_TWINS_H
#define VMAF_RUST_TWINS_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Build the Rust extractors once and install them in the registry.
 *
 * Idempotent and thread-safe; vmaf_init() calls it before the registry audit.
 * A twin whose C extractor is not compiled in (or whose ABI version differs)
 * is skipped with a log line.
 */
void vmaf_rust_twins_install(void);

#ifdef __cplusplus
}
#endif

#endif /* VMAF_RUST_TWINS_H */
