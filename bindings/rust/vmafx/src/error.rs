// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// Error type for the safe `vmafx` crate.
//
// libvmaf returns negative errno values from most of its C functions; this
// module maps the common ones to dedicated `Error` variants so callers can
// `match` on them ergonomically. Anything not in the curated set becomes
// `Error::Libvmaf { code }`, preserving the raw signed integer so the caller
// can inspect or log it. We deliberately do not promise a complete mapping:
// libvmaf does not version its errno surface, so over-promising is worse
// than catching the long tail under one variant.

use std::ffi::NulError;
use std::fmt;
use std::io;

/// Convenience alias for `Result<T, vmafx::Error>`.
pub type Result<T> = std::result::Result<T, Error>;

/// Errors returned by the safe `vmafx` API.
#[derive(Debug)]
#[non_exhaustive]
pub enum Error {
    /// Out of memory (`-ENOMEM`).
    OutOfMemory,
    /// Invalid argument (`-EINVAL`).
    InvalidArgument,
    /// Operation not supported (`-ENOSYS` / `-ENOTSUP`).
    NotSupported,
    /// Permission denied (`-EACCES`).
    PermissionDenied,
    /// File or resource not found (`-ENOENT`).
    NotFound,
    /// Catch-all for any negative libvmaf return value not mapped above.
    /// The raw errno-style code is preserved for logging.
    Libvmaf {
        /// The raw negative integer returned by the C function.
        code: i32,
    },
    /// A Rust `&str` contained an interior NUL byte and could not be turned
    /// into a `CString` for the FFI call.
    Nul(NulError),
    /// An operation was attempted on a resource that had already been
    /// consumed or moved (e.g. reading a picture after handing it to
    /// `Context::read_pictures`).
    InvalidState(&'static str),
    /// An I/O failure occurred (e.g. loading a model from disk).
    Io(io::Error),
}

impl Error {
    /// Map a raw libvmaf return code into an `Error`.
    ///
    /// Returns `Ok(())` for non-negative codes and `Err(Error::...)` for
    /// negatives. The variant chosen follows POSIX errno conventions where
    /// libvmaf is known to honour them.
    pub(crate) const fn from_libvmaf_rc(rc: i32) -> Result<()> {
        if rc >= 0 {
            return Ok(());
        }
        // libvmaf returns -errno on failure; normalise to positive for the match.
        let positive = -rc;
        // The libc errno constants are not in scope here without an extra
        // dependency; we use the well-known numeric values from <errno.h> on
        // Linux/macOS, which match POSIX. These are stable; libvmaf's CI
        // covers these same platforms.
        Err(match positive {
            12 => Self::OutOfMemory,       // ENOMEM
            22 => Self::InvalidArgument,   // EINVAL
            38 | 95 => Self::NotSupported, // ENOSYS / ENOTSUP (Linux: 95)
            13 => Self::PermissionDenied,  // EACCES
            2 => Self::NotFound,           // ENOENT
            _ => Self::Libvmaf { code: rc },
        })
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::OutOfMemory => f.write_str("libvmaf: out of memory"),
            Self::InvalidArgument => f.write_str("libvmaf: invalid argument"),
            Self::NotSupported => f.write_str("libvmaf: operation not supported"),
            Self::PermissionDenied => f.write_str("libvmaf: permission denied"),
            Self::NotFound => f.write_str("libvmaf: not found"),
            Self::Libvmaf { code } => write!(f, "libvmaf: error code {code}"),
            Self::Nul(e) => write!(f, "string contained interior NUL byte: {e}"),
            Self::InvalidState(msg) => write!(f, "invalid state: {msg}"),
            Self::Io(e) => write!(f, "I/O error: {e}"),
        }
    }
}

impl std::error::Error for Error {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Nul(e) => Some(e),
            Self::Io(e) => Some(e),
            _ => None,
        }
    }
}

impl From<NulError> for Error {
    fn from(e: NulError) -> Self {
        Self::Nul(e)
    }
}

impl From<io::Error> for Error {
    fn from(e: io::Error) -> Self {
        Self::Io(e)
    }
}

#[cfg(test)]
#[allow(clippy::expect_used, clippy::unwrap_used)]
mod tests {
    use super::*;

    #[test]
    fn ok_for_non_negative_codes() {
        assert!(Error::from_libvmaf_rc(0).is_ok());
        assert!(Error::from_libvmaf_rc(42).is_ok());
    }

    #[test]
    fn maps_known_errnos() {
        assert!(matches!(
            Error::from_libvmaf_rc(-12).unwrap_err(),
            Error::OutOfMemory
        ));
        assert!(matches!(
            Error::from_libvmaf_rc(-22).unwrap_err(),
            Error::InvalidArgument
        ));
        assert!(matches!(
            Error::from_libvmaf_rc(-2).unwrap_err(),
            Error::NotFound
        ));
    }

    #[test]
    fn unknown_errno_falls_through() {
        match Error::from_libvmaf_rc(-9999).unwrap_err() {
            Error::Libvmaf { code } => assert_eq!(code, -9999),
            other => panic!("expected Libvmaf, got {other:?}"),
        }
    }

    #[test]
    fn display_is_human_readable() {
        assert!(!format!("{}", Error::OutOfMemory).is_empty());
        assert!(!format!("{}", Error::Libvmaf { code: -42 }).is_empty());
    }
}
