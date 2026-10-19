// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2
//
// The feature-collector side of a twin call: the four shim callbacks.

use core::ffi::{CStr, c_void};

use crate::abi::{self, VmafxRsHost};
use crate::error::Error;

/// Feature-collector access for one `extract` or `flush` call. Names are the
/// BASE names of the C extractor's `provided_features`; mirror the C call for
/// call.
pub struct Host<'a> {
    raw: &'a VmafxRsHost,
}

type EmitFn = unsafe extern "C" fn(*mut c_void, *const core::ffi::c_char, u32, f64) -> i32;

impl<'a> Host<'a> {
    /// Wrap the shim's callbacks.
    ///
    /// # Safety
    ///
    /// Every non-NULL callback is safe to call with `raw.ctx` and a valid C
    /// string for the duration of `'a`.
    ///
    /// # Errors
    ///
    /// `Error::InvalidArgument` when a callback is missing.
    pub unsafe fn from_abi(raw: &'a VmafxRsHost) -> Result<Self, Error> {
        if raw.emit.is_none()
            || raw.emit_raw.is_none()
            || raw.get.is_none()
            || raw.set_aggregate.is_none()
            || raw.log.is_none()
        {
            return Err(Error::InvalidArgument(c"host callback missing"));
        }
        Ok(Self { raw })
    }

    fn call_emit(
        &self,
        f: Option<EmitFn>,
        feature: &CStr,
        index: u32,
        value: f64,
    ) -> Result<(), Error> {
        let Some(f) = f else {
            return Err(Error::InvalidArgument(c"host callback missing"));
        };
        // SAFETY: `from_abi`'s contract: the callback accepts ctx and a valid C string.
        let rc = unsafe { f(self.raw.ctx, feature.as_ptr(), index, value) };
        if rc == 0 {
            Ok(())
        } else {
            Err(Error::Host(rc))
        }
    }

    /// `vmaf_feature_collector_append_with_dict`: the option-decorated name.
    ///
    /// # Errors
    ///
    /// `Error::Host` when the collector refuses the score.
    pub fn emit(&mut self, feature: &CStr, index: u32, value: f64) -> Result<(), Error> {
        self.call_emit(self.raw.emit, feature, index, value)
    }

    /// `vmaf_feature_collector_append`: the name as given.
    ///
    /// # Errors
    ///
    /// `Error::Host` when the collector refuses the score.
    pub fn emit_raw(&mut self, feature: &CStr, index: u32, value: f64) -> Result<(), Error> {
        self.call_emit(self.raw.emit_raw, feature, index, value)
    }

    /// `vmaf_feature_collector_get_score` on the decorated name.
    #[must_use]
    pub fn get(&self, feature: &CStr, index: u32) -> Option<f64> {
        let f = self.raw.get?;
        let mut value = 0.0_f64;
        // SAFETY: `from_abi`'s contract; `value` is a valid out-pointer for the call.
        let rc = unsafe { f(self.raw.ctx, feature.as_ptr(), index, &raw mut value) };
        (rc == 0).then_some(value)
    }

    /// `vmaf_feature_collector_set_aggregate`: the name as given.
    ///
    /// # Errors
    ///
    /// `Error::Host` when the collector refuses the value.
    pub fn set_aggregate(&mut self, feature: &CStr, value: f64) -> Result<(), Error> {
        let Some(f) = self.raw.set_aggregate else {
            return Err(Error::InvalidArgument(c"host callback missing"));
        };
        // SAFETY: `from_abi`'s contract: the callback accepts ctx and a valid C string.
        let rc = unsafe { f(self.raw.ctx, feature.as_ptr(), value) };
        if rc == 0 {
            Ok(())
        } else {
            Err(Error::Host(rc))
        }
    }

    /// `vmaf_log(level, "%s\n", message)` (no allocation).
    pub fn log(&self, level: LogLevel, message: &CStr) {
        if let Some(f) = self.raw.log {
            // SAFETY: `from_abi`'s contract: the callback accepts ctx and a valid C string.
            unsafe { f(self.raw.ctx, level.raw(), message.as_ptr()) };
        }
    }

    /// Log a formatted message through a 256-byte stack buffer (truncated,
    /// never allocated), for the C's `vmaf_log` calls with arguments.
    pub fn log_fmt(&self, level: LogLevel, args: core::fmt::Arguments<'_>) {
        let mut buf = StackText::new();
        // A message longer than the buffer is truncated; that cannot fail.
        let _truncated = core::fmt::write(&mut buf, args);
        self.log(level, buf.as_c_str());
    }
}

/// `VmafLogLevel` of a host log line.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LogLevel {
    /// `VMAF_LOG_LEVEL_ERROR`.
    Error,
    /// `VMAF_LOG_LEVEL_WARNING`.
    Warning,
    /// `VMAF_LOG_LEVEL_INFO`.
    Info,
    /// `VMAF_LOG_LEVEL_DEBUG`.
    Debug,
}

impl LogLevel {
    const fn raw(self) -> i32 {
        match self {
            Self::Error => abi::VMAFX_RS_LOG_ERROR,
            Self::Warning => abi::VMAFX_RS_LOG_WARNING,
            Self::Info => abi::VMAFX_RS_LOG_INFO,
            Self::Debug => abi::VMAFX_RS_LOG_DEBUG,
        }
    }
}

const STACK_TEXT_LEN: usize = 256;

/// NUL-terminated text in a fixed buffer; writes past the end are dropped.
struct StackText {
    buf: [u8; STACK_TEXT_LEN],
    len: usize,
}

impl StackText {
    const fn new() -> Self {
        Self {
            buf: [0; STACK_TEXT_LEN],
            len: 0,
        }
    }

    fn as_c_str(&self) -> &CStr {
        // `len < STACK_TEXT_LEN` and `write_str` never stores a NUL, so the first NUL is at `len`.
        CStr::from_bytes_until_nul(&self.buf).unwrap_or(c"")
    }
}

impl core::fmt::Write for StackText {
    fn write_str(&mut self, s: &str) -> core::fmt::Result {
        for &b in s.as_bytes() {
            if self.len + 1 >= STACK_TEXT_LEN {
                break;
            }
            if let Some(slot) = self.buf.get_mut(self.len) {
                *slot = if b == 0 { b'?' } else { b };
                self.len += 1;
            }
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn stack_text_truncates_and_terminates() {
        let mut t = StackText::new();
        let long = "x".repeat(400);
        assert!(core::fmt::write(&mut t, format_args!("{long}")).is_ok());
        assert_eq!(t.as_c_str().to_bytes().len(), STACK_TEXT_LEN - 1);
        let mut u = StackText::new();
        assert!(core::fmt::write(&mut u, format_args!("n={}", 42)).is_ok());
        assert_eq!(u.as_c_str(), c"n=42");
    }
}
