// Copyright 2016-2026 Netflix, Inc.
// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Port of the scalar `decimate`, `min3`, `mode3` and `filter_mode` of
// `core/src/feature/cambi.c`, statement by statement. Both work in place on a
// plane of `stride` samples per row.

/// `decimate`: keep every second sample of every second row, in place
/// (`d[i][j] = d[2i][2j]`; a read never lands on a sample already written).
pub fn decimate(data: &mut [u16], stride: usize, width: usize, height: usize) {
    for i in 0..height {
        for j in 0..width {
            data[i * stride + j] = data[(i << 1) * stride + (j << 1)];
        }
    }
}

/// `min3`.
const fn min3(a: u16, b: u16, c: u16) -> u16 {
    if a <= b && a <= c {
        return a;
    }
    if b <= c {
        return b;
    }
    c
}

/// `mode3`: the value two of the three share, else the minimum.
const fn mode3(a: u16, b: u16, c: u16) -> u16 {
    if a == b || a == c {
        return a;
    }
    if b == c {
        return b;
    }
    min3(a, b, c)
}

/// One row of the horizontal mode filter into `line` (first and last sample
/// copied).
fn horizontal(row: &[u16], line: &mut [u16]) {
    let w = row.len();
    line[0] = row[0];
    for j in 1..w.saturating_sub(1) {
        line[j] = mode3(row[j - 1], row[j], row[j + 1]);
    }
    line[w - 1] = row[w - 1];
}

/// `filter_mode`: horizontal 3-tap mode of each row into a 3-line ring
/// (`buffer`, `3 * width`), then the vertical mode of the three lines written
/// to row `i - 1` once `i > 1`. Rows 0 and `height - 1` keep their samples.
pub fn filter_mode(
    data: &mut [u16],
    stride: usize,
    width: usize,
    height: usize,
    buffer: &mut [u16],
) {
    let w = width;
    let mut curr_line = 0usize;
    for i in 0..height {
        horizontal(
            &data[i * stride..i * stride + w],
            &mut buffer[curr_line * w..(curr_line + 1) * w],
        );
        if i > 1 {
            let dst = &mut data[(i - 1) * stride..(i - 1) * stride + w];
            for (j, d) in dst.iter_mut().enumerate() {
                *d = mode3(buffer[j], buffer[w + j], buffer[2 * w + j]);
            }
        }
        curr_line = if curr_line + 1 == 3 { 0 } else { curr_line + 1 };
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn mode3_cases() {
        assert_eq!(mode3(1, 1, 2), 1);
        assert_eq!(mode3(2, 1, 2), 2);
        assert_eq!(mode3(3, 1, 1), 1);
        assert_eq!(mode3(3, 1, 2), 1);
    }

    #[test]
    fn decimate_in_place() {
        let mut d: Vec<u16> = (0..16).collect();
        decimate(&mut d, 4, 2, 2);
        assert_eq!(&d[..2], &[0, 2]);
        assert_eq!(&d[4..6], &[8, 10]);
    }

    #[test]
    fn filter_mode_keeps_border_rows() {
        let mut d = vec![5u16, 1, 5, 7, 2, 7, 9, 3, 9];
        let mut buf = vec![0u16; 9];
        filter_mode(&mut d, 3, 3, 3, &mut buf);
        assert_eq!(&d[..3], &[5, 1, 5]);
        assert_eq!(&d[6..], &[9, 3, 9]);
        // Middle row: vertical mode of the horizontal modes [5,5,5], [7,7,7], [9,9,9]
        // (first / last columns copied: [5,5,5] [7,7,7] [9,9,9]) -> min 5 each.
        assert_eq!(&d[3..6], &[5, 5, 5]);
    }
}
