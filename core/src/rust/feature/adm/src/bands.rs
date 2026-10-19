// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2
//
// Working buffers of the integer ADM twin. The C extractor carves the same
// band sets out of one `data_buf` slab (integer_adm.c init_buffers()); here
// each band is its own vector, sized once in init. Every band of every scale
// uses the scale-0 element stride, as the C does.

use vmafx_fex::{Error, try_filled_vec};

/// The three detail bands of a set, indexed by `theta`: 0 horizontal
/// (`band_h`), 1 vertical (`band_v`), 2 diagonal (`band_d`).
pub type Hvd<T> = [Vec<T>; 3];

/// A full DWT output: low-pass `band_a` plus the detail bands.
pub struct Bands<T> {
    pub a: Vec<T>,
    pub hvd: Hvd<T>,
}

fn hvd<T: Clone + Default>(len: usize) -> Result<Hvd<T>, Error> {
    Ok([
        try_filled_vec(len, T::default())?,
        try_filled_vec(len, T::default())?,
        try_filled_vec(len, T::default())?,
    ])
}

fn bands<T: Clone + Default>(len: usize) -> Result<Bands<T>, Error> {
    Ok(Bands {
        a: try_filled_vec(len, T::default())?,
        hvd: hvd(len)?,
    })
}

/// The band sets of one pipeline width (16-bit at scale 0, 32-bit at 1..3).
pub struct Pipe<T> {
    pub ref_dwt: Bands<T>,
    pub dis_dwt: Bands<T>,
    pub decouple_r: Hvd<T>,
    pub decouple_a: Hvd<T>,
    pub csf_a: Hvd<T>,
    pub csf_f: Hvd<T>,
}

impl<T: Clone + Default> Pipe<T> {
    fn new(len: usize) -> Result<Self, Error> {
        Ok(Self {
            ref_dwt: bands(len)?,
            dis_dwt: bands(len)?,
            decouple_r: hvd(len)?,
            decouple_a: hvd(len)?,
            csf_a: hvd(len)?,
            csf_f: hvd(len)?,
        })
    }
}

/// The DWT source-index tables: `ind[k][i]` is the input index tap `k` reads
/// for output `i`.
pub type IndexTable = [Vec<usize>; 4];

fn index_table(len: usize) -> Result<IndexTable, Error> {
    Ok([
        try_filled_vec(len, 0)?,
        try_filled_vec(len, 0)?,
        try_filled_vec(len, 0)?,
        try_filled_vec(len, 0)?,
    ])
}

/// Every buffer one frame needs.
pub struct AdmBuffers {
    /// Element stride of every band (`ind_size_x >> 2` in C, without the
    /// alignment padding, which no stage reads).
    pub stride: usize,
    pub p16: Pipe<i16>,
    pub p32: Pipe<i32>,
    /// Scale-0 vertical-pass rows: low-pass then high-pass, `w` each.
    pub tmp16: Vec<i16>,
    /// Scale 1..3 vertical-pass rows: ref lo, ref hi, dis lo, dis hi.
    pub tmp32: Vec<i32>,
    pub ind_y: IndexTable,
    pub ind_x: IndexTable,
    /// Largest frame the buffers hold.
    pub max_w: usize,
    pub max_h: usize,
}

impl AdmBuffers {
    /// Allocate for a `w` x `h` frame.
    pub fn new(w: usize, h: usize) -> Result<Self, Error> {
        let w_half = w.div_ceil(2);
        let h_half = h.div_ceil(2);
        let len = w_half.checked_mul(h_half).ok_or(Error::OutOfMemory)?;
        Ok(Self {
            stride: w_half,
            p16: Pipe::new(len)?,
            p32: Pipe::new(len)?,
            tmp16: try_filled_vec(w.checked_mul(2).ok_or(Error::OutOfMemory)?, 0)?,
            tmp32: try_filled_vec(w.checked_mul(4).ok_or(Error::OutOfMemory)?, 0)?,
            ind_y: index_table(h_half)?,
            ind_x: index_table(w_half)?,
            max_w: w,
            max_h: h,
        })
    }
}
