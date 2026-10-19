// Copyright 2016-2026 Netflix, Inc.
// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent
//
//! Rust twin of the C `psnr` extractor (`core/src/feature/integer_psnr.c`,
//! `core/src/feature/psnr_score.h`), ported statement by statement: the
//! reference twin of the RC4 framework (ADR-1713) and the template the RC4
//! lanes copy. Scores equal the C extractor's bit for bit
//! (`scripts/ci/rust_twin_diff.py --feature psnr`).
//!
//! The C option table, its defaults and its ranges stay in C: this crate only
//! reads the parsed values.

#![forbid(unsafe_code)]

use core::ffi::CStr;

use vmafx_fex::libm::log10;
use vmafx_fex::{
    Error, Extractor, Flush, Frame, FromOptions, Geometry, Host, OptionValues, PixFmt, Plane,
    PlaneView, VmafxRsTwin, twin,
};

/// The twins this crate registers.
pub const TWINS: &[VmafxRsTwin] = &[twin::<Psnr>(c"psnr", c"psnr_rust")];

const PSNR_NAME: [&CStr; 3] = [c"psnr_y", c"psnr_cb", c"psnr_cr"];
const MSE_NAME: [&CStr; 3] = [c"mse_y", c"mse_cb", c"mse_cr"];
const APSNR_NAME: [&CStr; 3] = [c"apsnr_y", c"apsnr_cb", c"apsnr_cr"];

/// The options of `integer_psnr.c` (`PsnrState` fields set by the parser).
#[derive(Clone, Copy, Debug)]
pub struct PsnrOptions {
    enable_chroma: bool,
    enable_mse: bool,
    enable_apsnr: bool,
    reduced_hbd_peak: bool,
    min_sse: f64,
    uncapped: bool,
}

impl FromOptions for PsnrOptions {
    fn from_options(o: &OptionValues<'_>) -> Result<Self, Error> {
        Ok(Self {
            enable_chroma: o.bool(c"enable_chroma")?,
            enable_mse: o.bool(c"enable_mse")?,
            enable_apsnr: o.bool(c"enable_apsnr")?,
            reduced_hbd_peak: o.bool(c"reduced_hbd_peak")?,
            min_sse: o.f64(c"min_sse")?,
            uncapped: o.bool(c"uncapped")?,
        })
    }
}

/// Per-context state of the twin.
pub struct Psnr {
    opts: PsnrOptions,
    peak: u32,
    psnr_max: [f64; 3],
    apsnr_sse: [u64; 3],
    apsnr_n_pixels: [u64; 3],
}

/// `vmaf_psnr_peak()`.
fn psnr_peak(bpc: u32, reduced_hbd_peak: bool) -> Result<u32, Error> {
    if reduced_hbd_peak {
        bpc.checked_sub(8)
            .and_then(|s| 255_u32.checked_shl(s))
            .ok_or(Error::InvalidArgument(
                c"psnr: reduced_hbd_peak needs bpc >= 8",
            ))
    } else {
        1_u32
            .checked_shl(bpc)
            .map(|p| p - 1)
            .ok_or(Error::InvalidArgument(c"psnr: bpc out of range"))
    }
}

/// `vmaf_psnr_max()`.
fn psnr_max(bpc: u32, peak: u32, min_sse: f64, width: u32, height: u32) -> f64 {
    if min_sse != 0.0 {
        let mse = min_sse / (f64::from(width) * f64::from(height));
        return (10.0 * log10(f64::from(peak) * f64::from(peak) / mse)).ceil();
    }
    f64::from(6_u32.wrapping_mul(bpc).wrapping_add(12))
}

/// `vmaf_psnr_from_mse()`.
fn psnr_from_mse(mse: f64, peak_sq: f64, psnr_max: f64, uncapped: bool) -> f64 {
    let floored = if mse > 1e-16 { mse } else { 1e-16 };
    if !uncapped {
        let psnr = 10.0 * log10(peak_sq / floored);
        return if psnr < psnr_max { psnr } else { psnr_max };
    }
    if mse <= 0.0 {
        return psnr_max;
    }
    10.0 * log10(peak_sq / floored)
}

/// `vmaf_psnr_aggregate()`.
fn psnr_aggregate(peak: u32, sse: u64, n_pixels: u64, psnr_max: f64) -> f64 {
    if sse == 0 {
        return psnr_max;
    }
    let peak_sq = f64::from(peak) * f64::from(peak);
    #[allow(clippy::cast_precision_loss)] // C converts uint64_t to double here
    let (n, s) = (n_pixels as f64, sse as f64);
    let apsnr = 10.0 * (log10(peak_sq) + log10(n) - log10(s));
    let max_apsnr = (10.0 * log10(f64::from(peak) * f64::from(peak) * n)).ceil();
    if apsnr < max_apsnr { apsnr } else { max_apsnr }
}

/// `sse_line_8_c()`: the row sum is a `uint32_t` that wraps as in C.
fn sse_line_8(r: &[u8], d: &[u8]) -> u32 {
    let mut sse: u32 = 0;
    for (&a, &b) in r.iter().zip(d) {
        let e = i32::from(a) - i32::from(b);
        sse = sse.wrapping_add((e * e).unsigned_abs());
    }
    sse
}

/// `sse_line_16_c()`.
fn sse_line_16(r: &[u16], d: &[u16]) -> u64 {
    let mut sse: u64 = 0;
    for (&a, &b) in r.iter().zip(d) {
        let e = (i32::from(a) - i32::from(b)).unsigned_abs();
        sse = sse.wrapping_add(u64::from(e) * u64::from(e));
    }
    sse
}

fn plane_sse_8(r: &PlaneView<'_, u8>, d: &PlaneView<'_, u8>) -> u64 {
    let mut sse: u64 = 0;
    for y in 0..r.height() {
        sse = sse.wrapping_add(u64::from(sse_line_8(r.row(y), d.row(y))));
    }
    sse
}

fn plane_sse_16(r: &PlaneView<'_, u16>, d: &PlaneView<'_, u16>) -> u64 {
    let mut sse: u64 = 0;
    for y in 0..r.height() {
        sse = sse.wrapping_add(sse_line_16(r.row(y), d.row(y)));
    }
    sse
}

/// The SSE of plane `p`; the distorted plane must have the reference's
/// geometry (the C reads it with the reference's width and height).
fn plane_sse(r: Plane<'_>, d: Plane<'_>) -> Result<u64, Error> {
    if r.width() != d.width() || r.height() != d.height() {
        return Err(Error::InvalidArgument(c"psnr: planes differ in size"));
    }
    match (r, d) {
        (Plane::U8(r), Plane::U8(d)) => Ok(plane_sse_8(&r, &d)),
        (Plane::U16(r), Plane::U16(d)) => Ok(plane_sse_16(&r, &d)),
        _ => Err(Error::InvalidArgument(c"psnr: planes differ in bit depth")),
    }
}

impl Psnr {
    /// `psnr()` / `psnr_hbd()` for one plane: accumulate, emit psnr and mse.
    fn plane(
        &mut self,
        frame: &Frame<'_>,
        p: usize,
        peak_sq: f64,
        host: &mut Host<'_>,
    ) -> Result<(), Error> {
        let r = frame.reference.plane(p)?;
        let sse = plane_sse(r, frame.distorted.plane(p)?)?;
        // C: ref_pic->w[p] * ref_pic->h[p] in unsigned arithmetic, then to double.
        let (w, h) = (u32::try_from(r.width()), u32::try_from(r.height()));
        let (Ok(w), Ok(h)) = (w, h) else {
            return Err(Error::InvalidArgument(c"psnr: plane too large"));
        };
        if self.opts.enable_apsnr {
            self.apsnr_sse[p] = self.apsnr_sse[p].wrapping_add(sse);
            self.apsnr_n_pixels[p] =
                self.apsnr_n_pixels[p].wrapping_add(u64::from(h) * u64::from(w));
        }
        #[allow(clippy::cast_precision_loss)] // C converts uint64_t to double here
        let mse = sse as f64 / f64::from(w.wrapping_mul(h));
        let psnr = psnr_from_mse(mse, peak_sq, self.psnr_max[p], self.opts.uncapped);
        host.emit_raw(PSNR_NAME[p], frame.index, psnr)?;
        if self.opts.enable_mse {
            host.emit_raw(MSE_NAME[p], frame.index, mse)?;
        }
        Ok(())
    }
}

impl Extractor for Psnr {
    type Options = PsnrOptions;

    fn init(opts: &PsnrOptions, geom: &Geometry) -> Result<Self, Error> {
        let mut opts = *opts;
        let peak = psnr_peak(geom.bpc, opts.reduced_hbd_peak)?;
        if geom.pix_fmt == PixFmt::Yuv400p {
            opts.enable_chroma = false;
        }
        let ss_hor = geom.pix_fmt != PixFmt::Yuv444p;
        let ss_ver = geom.pix_fmt == PixFmt::Yuv420p;
        let mut max = [0.0_f64; 3];
        for (i, m) in max.iter_mut().enumerate() {
            let pw = if i > 0 && ss_hor {
                geom.w.wrapping_add(1) >> 1
            } else {
                geom.w
            };
            let ph = if i > 0 && ss_ver {
                geom.h.wrapping_add(1) >> 1
            } else {
                geom.h
            };
            *m = psnr_max(geom.bpc, peak, opts.min_sse, pw, ph);
        }
        Ok(Self {
            opts,
            peak,
            psnr_max: max,
            apsnr_sse: [0; 3],
            apsnr_n_pixels: [0; 3],
        })
    }

    fn extract(&mut self, frame: &Frame<'_>, host: &mut Host<'_>) -> Result<(), Error> {
        // psnr() uses `(double)peak * peak` with peak = 255; psnr_hbd() uses s->peak.
        let peak_sq = match frame.reference.bpc() {
            8 => 255.0_f64 * 255.0_f64,
            10 | 12 | 16 => f64::from(self.peak) * f64::from(self.peak),
            _ => return Err(Error::InvalidArgument(c"psnr: unsupported bit depth")),
        };
        let n = if self.opts.enable_chroma { 3 } else { 1 };
        for p in 0..n {
            self.plane(frame, p, peak_sq, host)?;
        }
        Ok(())
    }

    fn flush(&mut self, host: &mut Host<'_>) -> Result<Flush, Error> {
        if self.opts.enable_apsnr {
            let n_planes = if self.opts.enable_chroma { 3 } else { 1 };
            let planes = APSNR_NAME
                .iter()
                .zip(&self.apsnr_sse)
                .zip(&self.apsnr_n_pixels)
                .zip(&self.psnr_max);
            for (((name, &sse), &n_pixels), &max) in planes.take(n_planes) {
                host.set_aggregate(name, psnr_aggregate(self.peak, sse, n_pixels, max))?;
            }
        }
        Ok(Flush::Done)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn peak_matches_c() {
        assert_eq!(psnr_peak(8, false), Ok(255));
        assert_eq!(psnr_peak(10, false), Ok(1023));
        assert_eq!(psnr_peak(10, true), Ok(1020));
        assert!(psnr_peak(7, true).is_err());
    }

    #[test]
    fn psnr_max_without_min_sse_is_6bpc_plus_12() {
        assert_eq!(psnr_max(8, 255, 0.0, 16, 16).to_bits(), 60.0_f64.to_bits());
        assert_eq!(
            psnr_max(10, 1023, 0.0, 16, 16).to_bits(),
            72.0_f64.to_bits()
        );
    }

    #[test]
    fn zero_mse_reports_the_cap() {
        assert_eq!(
            psnr_from_mse(0.0, 65025.0, 60.0, false).to_bits(),
            60.0_f64.to_bits()
        );
        assert_eq!(
            psnr_from_mse(0.0, 65025.0, 60.0, true).to_bits(),
            60.0_f64.to_bits()
        );
    }

    #[test]
    fn sse_rows_match_hand_values() {
        assert_eq!(sse_line_8(&[10, 0, 255], &[7, 4, 0]), 9 + 16 + 65025);
        assert_eq!(sse_line_16(&[1023, 0], &[0, 1023]), 2 * 1023 * 1023);
    }

    #[test]
    fn aggregate_of_zero_sse_is_the_cap() {
        assert_eq!(
            psnr_aggregate(255, 0, 100, 60.0).to_bits(),
            60.0_f64.to_bits()
        );
    }
}
