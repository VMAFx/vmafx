// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2
//
// The platform libm, called exactly as the C extractors call it (ADR-1713):
// glibc on Linux, libSystem on macOS, the UCRT on Windows. Never the `libm`
// crate, never the std float methods for these functions: the reference is
// the C of the same build, and LLVM may evaluate a std intrinsic on constant
// arguments itself. Each wrapper is out of line and passes its arguments
// through `black_box`, so the call always reaches the library at run time.

use core::hint::black_box;

mod sys {
    // SAFETY: these are the C99 <math.h> functions with these exact signatures; they read only
    // their arguments, have no preconditions (domain errors return NaN / inf and may set errno,
    // which nothing here reads) and are thread-safe, so calling them is safe for any input.
    unsafe extern "C" {
        pub safe fn exp(x: f64) -> f64;
        pub safe fn log(x: f64) -> f64;
        pub safe fn log2(x: f64) -> f64;
        pub safe fn log10(x: f64) -> f64;
        pub safe fn pow(x: f64, y: f64) -> f64;
        pub safe fn cbrt(x: f64) -> f64;
        pub safe fn sin(x: f64) -> f64;
        pub safe fn cos(x: f64) -> f64;
        pub safe fn expf(x: f32) -> f32;
        pub safe fn logf(x: f32) -> f32;
        pub safe fn log2f(x: f32) -> f32;
        pub safe fn log10f(x: f32) -> f32;
        pub safe fn powf(x: f32, y: f32) -> f32;
        pub safe fn cbrtf(x: f32) -> f32;
    }
}

macro_rules! unary {
    ($($(#[$m:meta])* $name:ident: $t:ty;)*) => {$(
        $(#[$m])*
        #[inline(never)]
        #[must_use]
        pub fn $name(x: $t) -> $t {
            sys::$name(black_box(x))
        }
    )*};
}

unary! {
    /// C `exp`.
    exp: f64;
    /// C `log`.
    log: f64;
    /// C `log2`.
    log2: f64;
    /// C `log10`.
    log10: f64;
    /// C `cbrt`.
    cbrt: f64;
    /// C `sin`.
    sin: f64;
    /// C `cos`.
    cos: f64;
    /// C `expf`.
    expf: f32;
    /// C `logf`.
    logf: f32;
    /// C `log2f`.
    log2f: f32;
    /// C `log10f`.
    log10f: f32;
    /// C `cbrtf`.
    cbrtf: f32;
}

/// C `pow`.
#[inline(never)]
#[must_use]
pub fn pow(x: f64, y: f64) -> f64 {
    sys::pow(black_box(x), black_box(y))
}

/// C `powf`.
#[inline(never)]
#[must_use]
pub fn powf(x: f32, y: f32) -> f32 {
    sys::powf(black_box(x), black_box(y))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn exact_cases() {
        assert_eq!(log10(1000.0).to_bits(), 3.0_f64.to_bits());
        assert_eq!(log2(8.0).to_bits(), 3.0_f64.to_bits());
        assert_eq!(pow(2.0, 10.0).to_bits(), 1024.0_f64.to_bits());
        assert_eq!(exp(0.0).to_bits(), 1.0_f64.to_bits());
    }
}
