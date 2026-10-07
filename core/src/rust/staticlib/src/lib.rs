// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2
//
//! The one Rust archive libvmaf links (ADR-1713).
//!
//! It exports the registry of Rust twins the C shim
//! (`core/src/rust/shim/rust_twins.cpp`) reads, and re-exports every crate
//! whose `#[no_mangle]` symbols must reach the archive (`vmafx-fex`'s ABI
//! queries, the TAD pilot, the predictor). Lanes add twins in their own crates' `TWINS`; this file lists
//! the crates once and changes only when a crate is added.

#[cfg(test)]
use vmafx_fex::abi::VMAFX_RS_ABI_VERSION;
use vmafx_fex::abi::VmafxRsTwin;

// Crates whose exported C symbols must be linked into the archive.
pub use vmafx_fex;
pub use vmafx_predict;
pub use vmafx_tad;

/// Every twin group, in registry order.
const GROUPS: [&[VmafxRsTwin]; 5] = [
    vmafx_fex_psnr::TWINS,
    vmafx_fex_speed::TWINS,
    vmafx_fex_motion::TWINS,
    vmafx_fex_adm::TWINS,
    vmafx_fex_cambi::TWINS,
];

/// Twin `i` of the registry, or `None` past the end.
#[must_use]
pub fn twin_at(i: usize) -> Option<&'static VmafxRsTwin> {
    let mut k = i;
    for group in GROUPS {
        if let Some(t) = group.get(k) {
            return Some(t);
        }
        k -= group.len();
    }
    None
}

/// Twin `i` of the registry; NULL past the end. Declared in
/// `vmafx_rs.h` through the `trailer` of `core/src/rust/cbindgen.toml`.
#[unsafe(no_mangle)]
pub extern "C" fn vmafx_rs_twin_at(i: usize) -> *const VmafxRsTwin {
    twin_at(i).map_or(core::ptr::null(), core::ptr::from_ref)
}

#[cfg(test)]
mod tests {
    use super::*;
    use core::ffi::CStr;

    #[test]
    fn registry_lists_the_reference_twin() {
        let first = twin_at(0);
        assert!(first.is_some());
        if let Some(t) = first {
            assert_eq!(t.abi_version, VMAFX_RS_ABI_VERSION);
            // SAFETY: twin names are 'static C string literals.
            let name = unsafe { CStr::from_ptr(t.rust_name) };
            assert_eq!(name, c"psnr_rust");
        }
    }

    #[test]
    fn registry_ends_with_null() {
        let mut n = 0;
        while twin_at(n).is_some() {
            n += 1;
        }
        assert!(vmafx_rs_twin_at(n).is_null());
    }

    #[test]
    fn rust_names_are_c_name_plus_rust() {
        let mut i = 0;
        while let Some(t) = twin_at(i) {
            // SAFETY: twin names are 'static C string literals.
            let (c, r) = unsafe { (CStr::from_ptr(t.c_name), CStr::from_ptr(t.rust_name)) };
            let want = format!("{}_rust", c.to_string_lossy());
            assert_eq!(r.to_string_lossy(), want);
            i += 1;
        }
    }
}
