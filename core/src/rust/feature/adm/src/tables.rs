// Copyright 2016-2026 Netflix, Inc.
// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Literal tables of the integer ADM extractor, copied from
// core/src/feature/integer_adm.h, integer_adm_kernels.h and barten_csf_tools.h.
// Every C table is `float` initialised from double literals, so each entry is
// written as the double literal narrowed with `f`: decimal -> double -> float,
// the two roundings the C compiler performs, never one direct decimal -> float
// rounding (which can differ).

/// C `(float)x` of a double literal `x`, evaluated at compile time.
#[allow(clippy::cast_possible_truncation)]
const fn f(x: f64) -> f32 {
    x as f32
}

/// Daubechies-2 low-pass taps (`dwt2_db2_coeffs_lo`).
pub const DWT_LO: [i16; 4] = [15826, 27411, 7345, -4240];
/// Daubechies-2 high-pass taps (`dwt2_db2_coeffs_hi`).
pub const DWT_HI: [i16; 4] = [-4240, -7345, 27411, -15826];
/// `dwt2_db2_coeffs_lo_sum`.
pub const DWT_LO_SUM: i32 = 46342;
/// `dwt2_db2_coeffs_hi_sum`.
pub const DWT_HI_SUM: i32 = 0;

/// `ONE_BY_15`: 1/15 in Q17.
pub const ONE_BY_15: i32 = 8738;
/// `I4_ONE_BY_15`: 1/15 in Q32.
pub const I4_ONE_BY_15: i32 = 286_331_153;
/// `ADM_FIX_ONE_BY_30`: 1/30 in Q17.
pub const FIX_ONE_BY_30: i32 = 4369;
/// `I4_ADM_FIX_ONE_BY_30`: 1/30 in Q32 (`143165577u`).
pub const I4_FIX_ONE_BY_30: u32 = 143_165_577;

/// `div_Q_factor`: 2^30.
const DIV_Q_FACTOR: i32 = 1_073_741_824;

/// `div_lookup[]`: Q30 reciprocals of -32768..=32768, index +32768. Entry
/// 32768 (division by zero) stays 0 as in the zero-initialised C global.
pub static DIV_LOOKUP: [i32; 65537] = div_lookup();

const fn div_lookup() -> [i32; 65537] {
    let mut t = [0_i32; 65537];
    let mut i: usize = 1;
    while i <= 32768 {
        // `i` <= 32768 fits i32; the quotient is the C `(int32_t)(div_Q_factor / i)`.
        #[allow(clippy::cast_possible_truncation, clippy::cast_possible_wrap)]
        let recip = DIV_Q_FACTOR / (i as i32);
        t[32768 + i] = recip;
        t[32768 - i] = 0 - recip;
        i += 1;
    }
    t
}

/// One entry of `dwt_7_9_YCbCr_threshold` (Watson et al. 1997, table IV).
pub struct DwtModelParams {
    pub a: f32,
    pub k: f32,
    pub f0: f32,
    pub g: [f32; 4],
}

/// `dwt_7_9_YCbCr_threshold[0]` (Y); the extractor reads only the luma row.
pub const DWT_7_9_Y_THRESHOLD: DwtModelParams = DwtModelParams {
    a: f(0.495),
    k: f(0.466),
    f0: f(0.401),
    g: [f(1.501), f(1.0), f(0.534), f(1.0)],
};

/// `dwt_7_9_basis_function_amplitudes[lambda][theta]` (table V, transposed).
pub const DWT_7_9_BASIS_AMPLITUDES: [[f32; 4]; 6] = [
    [f(0.62171), f(0.67234), f(0.72709), f(0.67234)],
    [f(0.34537), f(0.41317), f(0.49428), f(0.41317)],
    [f(0.18004), f(0.22727), f(0.28688), f(0.22727)],
    [f(0.091401), f(0.11792), f(0.15214), f(0.11792)],
    [f(0.045943), f(0.059758), f(0.077727), f(0.059758)],
    [f(0.023013), f(0.030018), f(0.039156), f(0.030018)],
];

/// `barten_csf_param_anchors`: luminance levels (cd/m2) of the HDR-VDP2 fits.
pub const BARTEN_PARAM_ANCHORS: [f32; 6] = [f(0.002), f(0.02), f(0.2), 2.0, 20.0, 150.0];

/// `barten_csf_params`.
pub const BARTEN_PARAMS: [[f32; 5]; 6] = [
    [
        f(0.0160737),
        f(0.991265),
        f(3.74038),
        f(0.50722),
        f(4.46044),
    ],
    [
        f(0.383873),
        f(0.800889),
        f(3.54104),
        f(0.682505),
        f(4.94958),
    ],
    [
        f(0.929301),
        f(0.476505),
        f(4.37453),
        f(0.750315),
        f(5.28678),
    ],
    [f(1.29776), f(0.405782), f(4.40602), f(0.935314), f(5.61425)],
    [f(1.49222), f(0.334278), f(3.79542), f(1.07327), f(6.4635)],
    [f(1.46213), f(0.394533), f(2.7755), f(1.16577), f(7.45665)],
];

/// `barten_csf_sa`.
pub const BARTEN_SA: [f32; 4] = [f(30.162), f(4.0627), f(1.6596), f(0.2712)];
/// `barten_mtf_params_a`.
pub const BARTEN_MTF_A: [f32; 4] = [
    f(0.424_838_596_301_290),
    f(0.572_435_103_936_480),
    f(0.000_167_576_239_164_937),
    f(0.002_558_723_523_064_33),
];
/// `barten_mtf_params_b`.
pub const BARTEN_MTF_B: [f32; 4] = [f(0.028), f(0.37), 37.0, 360.0];

/// One blended-CSF table: `[theta][scale]`.
pub type BlendTable = [[f32; 4]; 2];

/// `BLENDED_CSF_1080_3H`.
pub const BLEND_1080_3H: BlendTable = [
    [f(0.01183), f(0.025026), f(0.04295), f(0.058621)],
    [f(0.004302), f(0.011778), f(0.023918), f(0.035901)],
];
/// `BLENDED_CSF_1080_5H`.
pub const BLEND_1080_5H: BlendTable = [
    [f(0.004212), f(0.014809), f(0.029642), f(0.047464)],
    [f(0.000984), f(0.005852), f(0.0146), f(0.027574)],
];
/// `BLENDED_CSF_2160_3H`.
pub const BLEND_2160_3H: BlendTable = [
    [f(0.00226), f(0.01183), f(0.025026), f(0.04295)],
    [f(0.000479), f(0.004302), f(0.011778), f(0.023918)],
];
/// `BLENDED_CSF_2160_5H`.
pub const BLEND_2160_5H: BlendTable = [
    [f(0.000092), f(0.004212), f(0.014809), f(0.029642)],
    [f(0.000050), f(0.000984), f(0.005852), f(0.0146)],
];
/// `BLENDED_CSF_720_3H`.
pub const BLEND_720_3H: BlendTable = [
    [f(0.018715), f(0.035637), f(0.052798), f(0.061509)],
    [f(0.007999), f(0.018396), f(0.031851), f(0.037718)],
];
/// `BLENDED_CSF_720_5H`.
pub const BLEND_720_5H: BlendTable = [
    [f(0.010144), f(0.022561), f(0.040309), f(0.05672)],
    [f(0.003463), f(0.010282), f(0.021839), f(0.034641)],
];
/// `BLENDED_CSF_480_3H`.
pub const BLEND_480_3H: BlendTable = [
    [f(0.027961), f(0.045875), f(0.060275), f(0.056234)],
    [f(0.013572), f(0.026277), f(0.036959), f(0.034511)],
];
/// `BLENDED_CSF_480_5H`.
pub const BLEND_480_5H: BlendTable = [
    [f(0.016781), f(0.032822), f(0.05032), f(0.061594)],
    [f(0.00691), f(0.016545), f(0.029917), f(0.037777)],
];

/// `BLENDED_CSF_1080_3H_MAE`.
pub const BLEND_1080_3H_MAE: BlendTable = [
    [f(0.011249), f(0.022606), f(0.035930), f(0.045673)],
    [f(0.004097), f(0.010921), f(0.021430), f(0.031313)],
];
/// `BLENDED_CSF_1080_5H_MAE`.
pub const BLEND_1080_5H_MAE: BlendTable = [
    [f(0.004052), f(0.013939), f(0.026298), f(0.038833)],
    [f(0.000927), f(0.005544), f(0.013415), f(0.024515)],
];
/// `BLENDED_CSF_2160_3H_MAE`.
pub const BLEND_2160_3H_MAE: BlendTable = [
    [f(0.002166), f(0.011249), f(0.022606), f(0.035930)],
    [f(0.000447), f(0.004097), f(0.010921), f(0.021430)],
];
/// `BLENDED_CSF_2160_5H_MAE`.
pub const BLEND_2160_5H_MAE: BlendTable = [
    [f(0.000077), f(0.004052), f(0.013939), f(0.026298)],
    [f(0.000045), f(0.000927), f(0.005544), f(0.013415)],
];
/// `BLENDED_CSF_720_3H_MAE`.
pub const BLEND_720_3H_MAE: BlendTable = [
    [f(0.017329), f(0.030870), f(0.042134), f(0.047410)],
    [f(0.007509), f(0.016707), f(0.028072), f(0.032722)],
];
/// `BLENDED_CSF_720_5H_MAE`.
pub const BLEND_720_5H_MAE: BlendTable = [
    [f(0.009689), f(0.020577), f(0.034162), f(0.044523)],
    [f(0.003302), f(0.009579), f(0.019661), f(0.030318)],
];
/// `BLENDED_CSF_480_3H_MAE`.
pub const BLEND_480_3H_MAE: BlendTable = [
    [f(0.024969), f(0.037825), f(0.046676), f(0.043974)],
    [f(0.012511), f(0.023424), f(0.032139), f(0.030166)],
];
/// `BLENDED_CSF_480_5H_MAE`.
pub const BLEND_480_5H_MAE: BlendTable = [
    [f(0.015665), f(0.028766), f(0.040612), f(0.047483)],
    [f(0.006514), f(0.015107), f(0.026476), f(0.032776)],
];

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn div_lookup_matches_the_c_table() {
        assert_eq!(DIV_LOOKUP[32768], 0);
        assert_eq!(DIV_LOOKUP[32769], 1_073_741_824);
        assert_eq!(DIV_LOOKUP[32767], -1_073_741_824);
        assert_eq!(DIV_LOOKUP[65536], 32768);
        assert_eq!(DIV_LOOKUP[0], -32768);
        assert_eq!(DIV_LOOKUP[32768 + 3], 357_913_941);
    }

    /// Every float table against the bit patterns the C initialisers give
    /// (dumped from integer_adm.h and barten_csf_tools.h, gcc, x86_64).
    #[test]
    fn float_tables_match_the_c_initialisers() {
        let y = &DWT_7_9_Y_THRESHOLD;
        let mut ours: Vec<(&str, Vec<f32>)> = vec![
            ("y_a", vec![y.a]),
            ("y_k", vec![y.k]),
            ("y_f0", vec![y.f0]),
            ("y_g", y.g.to_vec()),
            ("amp", DWT_7_9_BASIS_AMPLITUDES.concat()),
            ("anchors", BARTEN_PARAM_ANCHORS.to_vec()),
            ("params", BARTEN_PARAMS.concat()),
            ("sa", BARTEN_SA.to_vec()),
            ("mtfa", BARTEN_MTF_A.to_vec()),
            ("mtfb", BARTEN_MTF_B.to_vec()),
        ];
        let blends: [(&str, &BlendTable); 16] = [
            ("b1080_3", &BLEND_1080_3H),
            ("b1080_5", &BLEND_1080_5H),
            ("b2160_3", &BLEND_2160_3H),
            ("b2160_5", &BLEND_2160_5H),
            ("b720_3", &BLEND_720_3H),
            ("b720_5", &BLEND_720_5H),
            ("b480_3", &BLEND_480_3H),
            ("b480_5", &BLEND_480_5H),
            ("m1080_3", &BLEND_1080_3H_MAE),
            ("m1080_5", &BLEND_1080_5H_MAE),
            ("m2160_3", &BLEND_2160_3H_MAE),
            ("m2160_5", &BLEND_2160_5H_MAE),
            ("m720_3", &BLEND_720_3H_MAE),
            ("m720_5", &BLEND_720_5H_MAE),
            ("m480_3", &BLEND_480_3H_MAE),
            ("m480_5", &BLEND_480_5H_MAE),
        ];
        ours.extend(blends.iter().map(|(n, t)| (*n, t.concat())));
        let mut lines = 0;
        for line in crate::tables_c::C_FLOAT_TABLES.lines() {
            let f: Vec<&str> = line.split(' ').collect();
            let (name, idx, bits) = (f[0], f[1].parse::<usize>(), u32::from_str_radix(f[2], 16));
            let table = ours.iter().find(|(n, _)| *n == name).map(|(_, v)| v);
            let got = table
                .zip(idx.ok())
                .and_then(|(v, i)| v.get(i))
                .map(|x| x.to_bits());
            assert_eq!(got, bits.ok(), "{line}");
            lines += 1;
        }
        assert_eq!(lines, 207);
    }
}
