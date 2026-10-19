// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2
//
// Options of a Rust twin: the C extractor's option table, parsed by the C
// option parser and read back by name.

use core::ffi::CStr;

use crate::abi::{
    VMAFX_RS_OPT_BOOL, VMAFX_RS_OPT_DOUBLE, VMAFX_RS_OPT_INT, VMAFX_RS_OPT_STRING, VmafxRsOption,
};
use crate::error::Error;

/// The parsed options a twin's `init` receives. Values are the C parser's:
/// defaults, aliases, ranges and the model's `%g` normalisation are applied.
#[derive(Clone, Copy)]
pub struct OptionValues<'a> {
    opts: &'a [VmafxRsOption],
}

impl<'a> OptionValues<'a> {
    /// Wrap the options the shim passes.
    ///
    /// # Safety
    ///
    /// `opts` is NULL with `n == 0`, or points to `n` initialised
    /// `VmafxRsOption`s whose `name` (and non-NULL `s`) are NUL-terminated
    /// strings, all valid for `'a`.
    ///
    /// # Errors
    ///
    /// `Error::InvalidArgument` for a NULL array with `n > 0` or a NULL name.
    pub unsafe fn from_raw(opts: *const VmafxRsOption, n: usize) -> Result<Self, Error> {
        if n == 0 {
            return Ok(Self { opts: &[] });
        }
        if opts.is_null() {
            return Err(Error::InvalidArgument(c"option array is NULL"));
        }
        // SAFETY: the caller guarantees `opts` points to `n` initialised options valid for 'a.
        let opts = unsafe { core::slice::from_raw_parts(opts, n) };
        if opts.iter().any(|o| o.name.is_null()) {
            return Err(Error::InvalidArgument(c"option without a name"));
        }
        Ok(Self { opts })
    }

    /// An empty option list (twins of extractors without options, tests).
    #[must_use]
    pub const fn empty() -> Self {
        Self { opts: &[] }
    }

    /// Build from a slice (tests).
    #[must_use]
    pub const fn from_slice(opts: &'a [VmafxRsOption]) -> Self {
        Self { opts }
    }

    fn find(&self, name: &CStr, kind: u32) -> Result<&'a VmafxRsOption, Error> {
        for o in self.opts {
            // SAFETY: `from_raw` / `from_slice` callers guarantee every name is a valid C string.
            let o_name = unsafe { CStr::from_ptr(o.name) };
            if o_name == name {
                return if o.kind == kind {
                    Ok(o)
                } else {
                    Err(Error::InvalidArgument(c"option has another type"))
                };
            }
        }
        Err(Error::InvalidArgument(
            c"option not in the C extractor's table",
        ))
    }

    /// A bool option.
    ///
    /// # Errors
    ///
    /// `Error::InvalidArgument` when the option is missing or not a bool.
    pub fn bool(&self, name: &CStr) -> Result<bool, Error> {
        Ok(self.find(name, VMAFX_RS_OPT_BOOL)?.b != 0)
    }

    /// An int option.
    ///
    /// # Errors
    ///
    /// `Error::InvalidArgument` when the option is missing or not an int.
    pub fn int(&self, name: &CStr) -> Result<i32, Error> {
        Ok(self.find(name, VMAFX_RS_OPT_INT)?.i)
    }

    /// A double option.
    ///
    /// # Errors
    ///
    /// `Error::InvalidArgument` when the option is missing or not a double.
    pub fn f64(&self, name: &CStr) -> Result<f64, Error> {
        Ok(self.find(name, VMAFX_RS_OPT_DOUBLE)?.d)
    }

    /// A string option; `None` when the C value is NULL. Copy what must
    /// outlive `init`.
    ///
    /// # Errors
    ///
    /// `Error::InvalidArgument` when the option is missing or not a string.
    pub fn str(&self, name: &CStr) -> Result<Option<&'a CStr>, Error> {
        let o = self.find(name, VMAFX_RS_OPT_STRING)?;
        if o.s.is_null() {
            return Ok(None);
        }
        // SAFETY: a non-NULL string value is a valid C string for 'a (constructor contract).
        Ok(Some(unsafe { CStr::from_ptr(o.s) }))
    }
}

/// Parse a twin's options from the C extractor's parsed values.
pub trait FromOptions: Sized {
    /// Read every option the twin needs; refuse (`Error::Unsupported`) a value
    /// it does not implement.
    ///
    /// # Errors
    ///
    /// Any error the accessors return, or `Error::Unsupported`.
    fn from_options(opts: &OptionValues<'_>) -> Result<Self, Error>;
}

/// For extractors without options.
impl FromOptions for () {
    fn from_options(_: &OptionValues<'_>) -> Result<Self, Error> {
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use core::ptr;

    fn opt(name: &'static CStr, kind: u32) -> VmafxRsOption {
        VmafxRsOption {
            name: name.as_ptr(),
            kind,
            b: 1,
            i: 7,
            d: 0.5,
            s: ptr::null(),
        }
    }

    #[test]
    fn reads_values_by_name_and_type() {
        let table = [opt(c"a", VMAFX_RS_OPT_BOOL), opt(c"n", VMAFX_RS_OPT_INT)];
        let v = OptionValues::from_slice(&table);
        assert_eq!(v.bool(c"a"), Ok(true));
        assert_eq!(v.int(c"n"), Ok(7));
    }

    #[test]
    fn refuses_missing_and_mistyped_options() {
        let table = [opt(c"a", VMAFX_RS_OPT_BOOL)];
        let v = OptionValues::from_slice(&table);
        assert!(v.int(c"a").is_err());
        assert!(v.f64(c"missing").is_err());
    }

    #[test]
    fn null_string_is_none() {
        let table = [opt(c"p", VMAFX_RS_OPT_STRING)];
        let v = OptionValues::from_slice(&table);
        assert_eq!(v.str(c"p"), Ok(None));
    }
}
