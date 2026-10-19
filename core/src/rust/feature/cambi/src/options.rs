// Copyright 2016-2026 Netflix, Inc.
// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// The options of `cambi` and the init-time work `core/src/feature/cambi.c`
// does on them (`validate_and_setup_dimensions`, `adjust_window_size`, the
// `topk` / `cambi_topk` and `eotf` / `cambi_eotf` precedence, the
// `cambi_high_res_speedup` gating), statement by statement. Defaults, aliases
// and ranges come from the C option table through the shim and are never
// restated here.

use core::ffi::CStr;

use vmafx_fex::{Error, FromOptions, Geometry, OptionValues};

use crate::luminance::Eotf;

/// `DEFAULT_CAMBI_TOPK_POOLING`.
const DEFAULT_TOPK: f64 = 0.6;
/// `DEFAULT_CAMBI_EOTF`.
const DEFAULT_EOTF: &[u8] = b"bt1886";
/// `CAMBI_MIN_WIDTH_HEIGHT`.
const MIN_WIDTH_HEIGHT: u32 = 216;
/// `CAMBI_WINDOW_DIVISOR`.
const WINDOW_DIVISOR: u32 = 375;
/// `CAMBI_RECIPROCAL_LUT_SIZE` as the window guard compares it.
const LUT_SIZE: i32 = 4226;

/// The values of the C option table, as the C parser left them.
#[derive(Clone, Copy, Debug)]
pub struct CambiOptions {
    max_val: f64,
    enc: [u32; 3],
    src: [u32; 2],
    window_size: i32,
    topk: f64,
    tvi_threshold: f64,
    vis_lum_threshold: f64,
    max_log_contrast: i32,
    full_ref: bool,
    eotf: Eotf,
    high_res_speedup: i32,
}

/// An int option the C stores in an `unsigned` field.
fn uint(opts: &OptionValues<'_>, name: &CStr) -> Result<u32, Error> {
    Ok(opts.int(name)? as u32)
}

/// `cambi_eotf` when set and not the default, else `eotf`, else the default
/// (`vmaf_cambi_init_tvi_and_vlt`).
fn effective_eotf(opts: &OptionValues<'_>) -> Result<Eotf, Error> {
    let cambi_eotf = opts.str(c"cambi_eotf")?.map(CStr::to_bytes);
    let eotf = opts.str(c"eotf")?.map(CStr::to_bytes);
    let name = match cambi_eotf {
        Some(c) if c != DEFAULT_EOTF => c,
        _ => eotf.unwrap_or(DEFAULT_EOTF),
    };
    Eotf::from_name(name)
}

impl FromOptions for CambiOptions {
    fn from_options(opts: &OptionValues<'_>) -> Result<Self, Error> {
        if opts.str(c"heatmaps_path")?.is_some() {
            return Err(Error::Unsupported(
                c"heatmaps_path is not implemented; use the C extractor",
            ));
        }
        let topk = opts.f64(c"topk")?;
        Ok(Self {
            max_val: opts.f64(c"cambi_max_val")?,
            enc: [
                uint(opts, c"enc_width")?,
                uint(opts, c"enc_height")?,
                uint(opts, c"enc_bitdepth")?,
            ],
            src: [uint(opts, c"src_width")?, uint(opts, c"src_height")?],
            window_size: opts.int(c"window_size")?,
            // The original topk when it is not the default, else cambi_topk.
            topk: if topk != DEFAULT_TOPK {
                topk
            } else {
                opts.f64(c"cambi_topk")?
            },
            tvi_threshold: opts.f64(c"tvi_threshold")?,
            vis_lum_threshold: opts.f64(c"cambi_vis_lum_threshold")?,
            max_log_contrast: opts.int(c"max_log_contrast")?,
            full_ref: opts.bool(c"full_ref")?,
            eotf: effective_eotf(opts)?,
            high_res_speedup: opts.int(c"cambi_high_res_speedup")?,
        })
    }
}

/// The geometry of one pass (distorted at the encode size, or the source).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PassGeometry {
    /// Width the input is resampled to.
    pub width: usize,
    /// Height the input is resampled to.
    pub height: usize,
    /// The adjusted CAMBI window.
    pub window: u16,
}

/// Everything `init` derives from the options and the picture geometry.
#[derive(Clone, Copy, Debug)]
pub struct Settings {
    /// `cambi_max_val`.
    pub max_val: f64,
    /// `enc_bitdepth` after defaulting.
    pub enc_bitdepth: u32,
    /// The distorted pass.
    pub enc: PassGeometry,
    /// The source pass (`full_ref`).
    pub src: PassGeometry,
    /// Pooling ratio after the `topk` / `cambi_topk` precedence.
    pub topk: f64,
    /// `tvi_threshold`.
    pub tvi_threshold: f64,
    /// `cambi_vis_lum_threshold`.
    pub vis_lum_threshold: f64,
    /// `1 << max_log_contrast`.
    pub num_diffs: u16,
    /// `full_ref`.
    pub full_ref: bool,
    /// The effective EOTF.
    pub eotf: Eotf,
    /// `cambi_high_res_speedup` after gating (true = applied).
    pub high_res_speedup: bool,
    /// Width of the working buffers.
    pub alloc_w: usize,
    /// Height of the working buffers.
    pub alloc_h: usize,
}

/// `cambi_validate_dimensions`.
const fn valid_dimensions(w: u32, h: u32) -> bool {
    w >= MIN_WIDTH_HEIGHT || h >= MIN_WIDTH_HEIGHT
}

/// `adjust_window_size`: scale by `(w + h) / (3840 + 2160)` in 1/16 steps,
/// halve for the high-resolution speed-up, round up to odd.
pub(crate) fn adjust_window(window: u16, w: u32, h: u32, speedup: bool) -> u16 {
    let mut win =
        ((u32::from(window).wrapping_mul(w.wrapping_add(h)) / WINDOW_DIVISOR) >> 4) as u16;
    if speedup {
        win = ((u32::from(win) + 1) >> 1) as u16;
    }
    win | 1
}

/// The `cambi_high_res_speedup` switch of `validate_and_setup_dimensions`.
fn gate_speedup(tier: i32, enc_pix: i32) -> bool {
    let threshold = match tier {
        1080 => 1920 * 1080,
        1440 => 2560 * 1440,
        2160 => 3840 * 2160,
        _ => return false,
    };
    enc_pix >= threshold
}

/// Encode / source dimensions after defaulting and the downscale clamp.
fn dimensions(o: &CambiOptions, g: &Geometry) -> Result<([u32; 2], [u32; 2]), Error> {
    let (w, h) = (g.w, g.h);
    let mut enc = [o.enc[0], o.enc[1]];
    let mut src = o.src;
    if enc[0] == 0 || enc[1] == 0 {
        enc = [w, h];
    }
    if src[0] == 0 || src[1] == 0 {
        src = [w, h];
    }
    if enc[1] > h || enc[0] > w {
        enc = [w, h];
    }
    let invalid = !valid_dimensions(enc[0], enc[1])
        || !valid_dimensions(src[0], src[1])
        || (src[0] > enc[0] && src[1] < enc[1])
        || (src[0] < enc[0] && src[1] > enc[1]);
    if invalid {
        return Err(Error::InvalidArgument(
            c"invalid encode or source dimensions",
        ));
    }
    Ok((enc, src))
}

impl Settings {
    /// `init`'s option post-processing for pictures of geometry `g`.
    ///
    /// # Errors
    ///
    /// `Error::InvalidArgument` where the C `init` returns `-EINVAL`.
    pub fn new(o: &CambiOptions, g: &Geometry) -> Result<Self, Error> {
        let (enc, src) = dimensions(o, g)?;
        let enc_pix = enc[0].wrapping_mul(enc[1]) as i32;
        let speedup = gate_speedup(o.high_res_speedup, enc_pix);
        let window = o.window_size as u16;
        let enc_win = adjust_window(window, enc[0], enc[1], speedup);
        let src_win = adjust_window(window, src[0], src[1], speedup);
        let max_window = i32::from(enc_win.max(src_win));
        if max_window * max_window >= LUT_SIZE {
            return Err(Error::InvalidArgument(
                c"window_size too large for reciprocal LUT",
            ));
        }
        let (alloc_w, alloc_h) = if o.full_ref {
            (enc[0].max(src[0]), enc[1].max(src[1]))
        } else {
            (enc[0], enc[1])
        };
        Ok(Self {
            max_val: o.max_val,
            enc_bitdepth: if o.enc[2] == 0 { g.bpc } else { o.enc[2] },
            enc: pass(enc, enc_win),
            src: pass(src, src_win),
            topk: o.topk,
            tvi_threshold: o.tvi_threshold,
            vis_lum_threshold: o.vis_lum_threshold,
            num_diffs: 1u16 << o.max_log_contrast,
            full_ref: o.full_ref,
            eotf: o.eotf,
            high_res_speedup: speedup,
            alloc_w: alloc_w as usize,
            alloc_h: alloc_h as usize,
        })
    }
}

const fn pass(dims: [u32; 2], window: u16) -> PassGeometry {
    PassGeometry {
        width: dims[0] as usize,
        height: dims[1] as usize,
        window,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn window_adjustment() {
        // 576x324: (65 * 900 / 375) >> 4 = 9.
        assert_eq!(adjust_window(65, 576, 324, false), 9);
        // 3840x2160 with the speed-up: 65 -> (65 + 1) >> 1 = 33.
        assert_eq!(adjust_window(65, 3840, 2160, true), 33);
        // 1920x1080: (65 * 3000 / 375) >> 4 = 32 -> 16 -> odd 17.
        assert_eq!(adjust_window(65, 1920, 1080, true), 17);
    }

    #[test]
    fn speedup_gate() {
        assert!(gate_speedup(1080, 1920 * 1080));
        assert!(!gate_speedup(1080, 576 * 324));
        assert!(!gate_speedup(720, 3840 * 2160));
        assert!(!gate_speedup(2160, 2560 * 1440));
    }
}
