// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2
//
// Error type of the Rust twins and its mapping onto the ABI status codes.

use core::ffi::CStr;

use crate::abi;

/// Why a twin call failed. Every message is a static C string the shim logs
/// next to the twin's name.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    /// An argument or input the C extractor rejects with `-EINVAL`.
    InvalidArgument(&'static CStr),
    /// A valid option value or input this twin does not implement.
    Unsupported(&'static CStr),
    /// A buffer could not be reserved.
    OutOfMemory,
    /// A value is out of range (`-ERANGE`).
    Range(&'static CStr),
    /// A score is not finite.
    NonFinite(&'static CStr),
    /// A host (feature collector) callback returned this negative errno.
    Host(i32),
}

impl Error {
    /// The ABI status code of this error.
    #[must_use]
    pub const fn status(self) -> i32 {
        match self {
            Self::InvalidArgument(_) => abi::VMAFX_RS_E_INVAL,
            Self::Unsupported(_) => abi::VMAFX_RS_E_NOTSUP,
            Self::OutOfMemory => abi::VMAFX_RS_E_NOMEM,
            Self::Range(_) => abi::VMAFX_RS_E_RANGE,
            Self::NonFinite(_) => abi::VMAFX_RS_E_NONFINITE,
            Self::Host(_) => abi::VMAFX_RS_E_HOST,
        }
    }

    /// The message the shim logs.
    #[must_use]
    pub const fn message(self) -> &'static CStr {
        match self {
            Self::InvalidArgument(m)
            | Self::Unsupported(m)
            | Self::Range(m)
            | Self::NonFinite(m) => m,
            Self::OutOfMemory => c"out of memory",
            Self::Host(_) => c"feature collector call failed",
        }
    }
}

/// Reserve `len` elements of `T`, all set to `fill`, failing with
/// `Error::OutOfMemory` instead of aborting (HISS-03: call it in `init`).
///
/// # Errors
///
/// `Error::OutOfMemory` when the allocation fails.
pub fn try_filled_vec<T: Clone>(len: usize, fill: T) -> Result<Vec<T>, Error> {
    let mut v = Vec::new();
    v.try_reserve_exact(len).map_err(|_| Error::OutOfMemory)?;
    v.resize(len, fill);
    Ok(v)
}
