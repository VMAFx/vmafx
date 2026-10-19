// Copyright 2016-2026 Netflix, Inc.
// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Port of the pooling of `core/src/feature/cambi.c`: the bounded Hoare
// quick-select (`quick_select_partition`, `quick_select`),
// `average_topk_elements`, `spatial_pooling`, `get_pixels_in_window` and
// `weight_scores_per_scale`, statement by statement. The selection order
// decides which elements the double sum adds in which order, so the C
// algorithm is reproduced, not replaced by a sort.

use crate::lut::SCALE_WEIGHTS;

/// `NUM_SCALES`.
pub const NUM_SCALES: usize = 5;

/// `quick_select_partition`: Hoare partition around `pivot`, larger values
/// first, every scan bounded by the span.
fn partition(arr: &mut [f32], i: &mut i32, j: &mut i32, pivot: f32) {
    let max_steps = *j - *i + 1;
    for _ in 0..max_steps {
        if *i > *j {
            break;
        }
        for _ in 0..max_steps {
            if !(*i <= *j && arr[*i as usize] > pivot) {
                break;
            }
            *i += 1;
        }
        for _ in 0..max_steps {
            if !(*i <= *j && arr[*j as usize] < pivot) {
                break;
            }
            *j -= 1;
        }
        if *i <= *j {
            arr.swap(*i as usize, *j as usize);
            *i += 1;
            *j -= 1;
        }
    }
}

/// `quick_select(arr, n, k)`: the `k` largest values to the front.
fn quick_select(arr: &mut [f32], n: i32, k: i32) {
    if n == k {
        return;
    }
    let mut left = 0;
    let mut right = n - 1;
    for _ in 0..n {
        if left >= right {
            break;
        }
        let pivot = arr[k as usize];
        let mut i = left;
        let mut j = right;
        partition(arr, &mut i, &mut j, pivot);
        if j < k {
            left = i;
        }
        if k < i {
            right = j;
        }
    }
}

/// `average_topk_elements`: double sum in index order, divided by the count.
fn average_topk(arr: &[f32], topk_elements: i32) -> f64 {
    let mut sum = 0.0_f64;
    for &v in &arr[..topk_elements as usize] {
        sum += f64::from(v);
    }
    sum / f64::from(topk_elements)
}

/// `spatial_pooling`: the mean of the `topk` share of the largest c-values;
/// reorders `c_values[..width * height]`.
pub fn spatial_pooling(c_values: &mut [f32], topk: f64, width: usize, height: usize) -> f64 {
    // C: int num_elements = height * width (unsigned product).
    let num_elements = (height as u32).wrapping_mul(width as u32) as i32;
    // C: clip(topk * num_elements, 1, num_elements): the double converts to the
    // int parameter by truncation.
    let raw = (topk * f64::from(num_elements)) as i32;
    let topk_num_elements = if raw < 1 {
        1
    } else if raw > num_elements {
        num_elements
    } else {
        raw
    };
    let arr = &mut c_values[..num_elements as usize];
    quick_select(arr, num_elements, topk_num_elements);
    average_topk(arr, topk_num_elements)
}

/// `get_pixels_in_window`: `(2 * (w >> 1) + 1)^2` in `uint16_t`.
#[must_use]
pub const fn pixels_in_window(window_length: u16) -> u16 {
    let odd_length = (2 * (window_length as i32 >> 1) + 1) as u16;
    (odd_length as i32 * odd_length as i32) as u16
}

/// `weight_scores_per_scale`: `sum(score[s] * {16, 8, 4, 2, 1}[s]) / norm`.
#[must_use]
pub fn weight_scores(scores: &[f64; NUM_SCALES], normalization: u16) -> f64 {
    let mut score = 0.0_f64;
    for (s, w) in scores.iter().zip(SCALE_WEIGHTS) {
        score += s * f64::from(w);
    }
    score / f64::from(normalization)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn selects_the_largest() {
        let mut v = [3.0_f32, 9.0, 1.0, 7.0, 5.0, 2.0];
        let m = spatial_pooling(&mut v, 0.5, 6, 1);
        assert_eq!(m.to_bits(), ((9.0_f64 + 7.0 + 5.0) / 3.0).to_bits());
    }

    #[test]
    fn topk_at_least_one() {
        let mut v = [1.0_f32, 4.0];
        assert_eq!(
            spatial_pooling(&mut v, 0.0001, 2, 1).to_bits(),
            4.0_f64.to_bits()
        );
    }

    #[test]
    fn window_pixels() {
        assert_eq!(pixels_in_window(9), 81);
        assert_eq!(pixels_in_window(65), 4225);
        assert_eq!(pixels_in_window(64), 4225);
    }
}
