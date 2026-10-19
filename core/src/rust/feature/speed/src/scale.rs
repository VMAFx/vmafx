// Copyright 2016-2020 Netflix, Inc.
// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Prescale resamplers of SpEED. Mirrors `vif_scale_frame_s()` and its four
// methods in core/src/feature/vif_tools.c; ported statement by statement from
// Netflix code. Each sample position is `(y + 0.5) * ratio - 0.5` in double,
// narrowed to float; the weights are float, the lanczos kernel double.

use core::ffi::CStr;

use crate::fexapi::libm;

/// `enum vif_scaling_method`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Method {
    Nearest,
    Bicubic,
    Lanczos4,
    Bilinear,
}

impl Method {
    /// `vif_get_scaling_method()`; `None` for a name the C rejects.
    #[must_use]
    pub fn from_name(name: &CStr) -> Option<Self> {
        match name.to_bytes() {
            b"nearest" => Some(Self::Nearest),
            b"bilinear" => Some(Self::Bilinear),
            b"bicubic" => Some(Self::Bicubic),
            b"lanczos4" => Some(Self::Lanczos4),
            _ => None,
        }
    }
}

/// Source and destination planes share one stride (the C passes `stride_px`
/// for both).
#[derive(Clone, Copy)]
pub struct Plane {
    pub stride: usize,
    pub src_w: usize,
    pub src_h: usize,
    pub dst_w: usize,
    pub dst_h: usize,
}

const LANCZOS_TAPS: usize = 9;

/// `mirror()`.
fn mirror(i: f32, left: f32, right: f32) -> f32 {
    if i < left {
        -i
    } else if i > right {
        2.0 * right - i
    } else {
        i
    }
}

/// `mirror()` of an int index against `[0, n)`, with the C's int to float
/// conversions and `(int)` cast.
fn mirror_idx(i: i32, n: usize) -> usize {
    mirror(i as f32, 0.0, (n as i32 - 1) as f32) as i32 as usize
}

/// `(y + 0.5) * ratio - 0.5`: double arithmetic, narrowed to float.
fn position(y: usize, ratio: f32) -> f32 {
    ((f64::from(y as i32) + 0.5) * f64::from(ratio) - 0.5) as f32
}

fn bicubic_kernel(t: f32) -> f32 {
    let a = -0.75_f32;
    let t = if t < 0.0 { -t } else { t };
    if t < 1.0 {
        return ((a + 2.0) * t - (a + 3.0)) * t * t + 1.0;
    }
    if t < 2.0 {
        return (((t - 5.0) * t + 8.0) * t - 4.0) * a;
    }
    0.0
}

fn bicubic_at(src: &[f32], p: Plane, x: f32, y: f32) -> f32 {
    let x0 = x.floor() as i32;
    let y0 = y.floor() as i32;
    let dx = x - x0 as f32;
    let dy = y - y0 as f32;
    let mut wx = [0.0_f32; 4];
    let mut wy = [0.0_f32; 4];
    for i in -1_i32..=2 {
        wx[(i + 1) as usize] = bicubic_kernel(i as f32 - dx);
        wy[(i + 1) as usize] = bicubic_kernel(i as f32 - dy);
    }
    let mut interp = 0.0_f32;
    for j in -1_i32..=2 {
        for i in -1_i32..=2 {
            let xi = mirror_idx(x0 + i, p.src_w);
            let yi = mirror_idx(y0 + j, p.src_h);
            let weight = wx[(i + 1) as usize] * wy[(j + 1) as usize];
            interp += src[yi * p.stride + xi] * weight;
        }
    }
    interp
}

/// `lanczos4_kernel()`: the double expression with libm `sin`.
fn lanczos_kernel(x: f32, a: f32) -> f32 {
    if x == 0.0 {
        return 1.0;
    }
    if x > -a && x < a {
        let pi = core::f64::consts::PI;
        let (xd, ad) = (f64::from(x), f64::from(a));
        return (ad * libm::sin(pi * xd) * libm::sin(pi * xd / ad) / (pi * pi * xd * xd)) as f32;
    }
    0.0
}

/// `lanczos4_weights()`.
fn lanczos_weights(d: f32, weights: &mut [f32; LANCZOS_TAPS]) {
    let a = ((LANCZOS_TAPS - 1) / 2) as i32;
    for i in -a..=a {
        weights[(i + a) as usize] = lanczos_kernel(i as f32 - d, a as f32);
    }
}

fn lanczos_at(src: &[f32], p: Plane, x: f32, y: f32) -> f32 {
    let a = 4_i32;
    let x0 = x.floor() as i32;
    let y0 = y.floor() as i32;
    let mut wx = [0.0_f32; LANCZOS_TAPS];
    let mut wy = [0.0_f32; LANCZOS_TAPS];
    lanczos_weights(x - x0 as f32, &mut wx);
    lanczos_weights(y - y0 as f32, &mut wy);
    let mut value = 0.0_f32;
    let mut weight_sum = 0.0_f32;
    for iy in -a..=a {
        for ix in -a..=a {
            let weight = wx[(ix + a) as usize] * wy[(iy + a) as usize];
            weight_sum += weight;
            let xi = mirror_idx(x0 + ix, p.src_w);
            let yi = mirror_idx(y0 + iy, p.src_h);
            value += src[yi * p.stride + xi] * weight;
        }
    }
    value / weight_sum
}

fn bilinear_at(src: &[f32], p: Plane, x: f32, y: f32) -> f32 {
    let right = (p.src_w as i32 - 1) as f32;
    let bottom = (p.src_h as i32 - 1) as f32;
    let x1 = mirror(x.floor(), 0.0, right) as i32;
    let x2 = mirror(x.ceil(), 0.0, right) as i32 as usize;
    let y1 = mirror(y.floor(), 0.0, bottom) as i32;
    let y2 = mirror(y.ceil(), 0.0, bottom) as i32 as usize;
    let dx = x - x1 as f32;
    let dy = y - y1 as f32;
    let (x1, y1) = (x1 as usize, y1 as usize);
    (1.0 - dy) * (1.0 - dx) * src[y1 * p.stride + x1]
        + (1.0 - dy) * dx * src[y1 * p.stride + x2]
        + dy * (1.0 - dx) * src[y2 * p.stride + x1]
        + dy * dx * src[y2 * p.stride + x2]
}

/// `vif_scale_frame_nearest_s()`'s per-sample read.
fn nearest_at(src: &[f32], p: Plane, x: usize, y: usize, ratio: (f32, f32)) -> f32 {
    let rounded_y = (y as i32 as f32 * ratio.1) as i32 as usize;
    let rounded_x = (x as i32 as f32 * ratio.0) as i32 as usize;
    src[rounded_y * p.stride + rounded_x]
}

/// `vif_scale_frame_s()`: scale `src` (`src_w` x `src_h`) into `dst`
/// (`dst_w` x `dst_h`). Equal sizes copy `dst_h` rows of `stride` floats.
pub fn scale_frame(method: Method, src: &[f32], dst: &mut [f32], p: Plane) {
    if p.src_w == p.dst_w && p.src_h == p.dst_h {
        let n = p.stride * p.dst_h;
        dst[..n].copy_from_slice(&src[..n]);
        return;
    }
    let ratio = (
        p.src_w as i32 as f32 / p.dst_w as i32 as f32,
        p.src_h as i32 as f32 / p.dst_h as i32 as f32,
    );
    for y in 0..p.dst_h {
        let yy = position(y, ratio.1);
        for x in 0..p.dst_w {
            let xx = position(x, ratio.0);
            dst[y * p.stride + x] = match method {
                Method::Nearest => nearest_at(src, p, x, y, ratio),
                Method::Bilinear => bilinear_at(src, p, xx, yy),
                Method::Bicubic => bicubic_at(src, p, xx, yy),
                Method::Lanczos4 => lanczos_at(src, p, xx, yy),
            };
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn method_names_follow_the_c() {
        assert_eq!(Method::from_name(c"bilinear"), Some(Method::Bilinear));
        assert_eq!(Method::from_name(c"lanczos4"), Some(Method::Lanczos4));
        assert_eq!(Method::from_name(c"cubic"), None);
    }

    #[test]
    fn same_size_copies() {
        let src = [1.0_f32, 2.0, 3.0, 4.0];
        let mut dst = [0.0_f32; 4];
        let p = Plane {
            stride: 2,
            src_w: 2,
            src_h: 2,
            dst_w: 2,
            dst_h: 2,
        };
        scale_frame(Method::Bicubic, &src, &mut dst, p);
        assert_eq!(src, dst);
    }

    #[test]
    fn bilinear_halves_a_ramp() {
        let src: Vec<f32> = (0..16).map(|v| (v % 4) as f32).collect();
        let mut dst = vec![0.0_f32; 16];
        let p = Plane {
            stride: 4,
            src_w: 4,
            src_h: 4,
            dst_w: 2,
            dst_h: 2,
        };
        scale_frame(Method::Bilinear, &src, &mut dst, p);
        assert!((dst[0] - 0.5).abs() < 1e-6 && (dst[1] - 2.5).abs() < 1e-6);
    }
}
