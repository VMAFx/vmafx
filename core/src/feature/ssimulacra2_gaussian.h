/**
 *
 *  Copyright (c) the JPEG XL Project Authors.
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-3-Clause
 */

#ifndef SSIMULACRA2_GAUSSIAN_H_
#define SSIMULACRA2_GAUSSIAN_H_

#include <math.h>

#ifndef SSIMU2_PI
#define SSIMU2_PI 3.14159265358979323846
#endif

typedef struct Ssimu2GaussianCoeffs {
    int radius;
    float n2[3];
    float d1[3];
} Ssimu2GaussianCoeffs;

/* A * beta = gamma for the 3x3 system of CreateRecursiveGaussian, solved by
 * Cramer's rule in libjxl's expression order (each column of A replaced by
 * gamma in turn). */
static inline void ssimu2_solve_cramer_3x3(const double A[3][3], const double gamma[3],
                                           double beta[3])
{
    const double det_A = A[0][0] * (A[1][1] * A[2][2] - A[1][2] * A[2][1]) -
                         A[0][1] * (A[1][0] * A[2][2] - A[1][2] * A[2][0]) +
                         A[0][2] * (A[1][0] * A[2][1] - A[1][1] * A[2][0]);
    const double inv_det = 1.0 / det_A;

    for (int col = 0; col < 3; col++) {
        double M[3][3];
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++) {
                M[i][j] = A[i][j];
            }
        }
        for (int i = 0; i < 3; i++) {
            M[i][col] = gamma[i];
        }
        beta[col] = (M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) -
                     M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) +
                     M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0])) *
                    inv_det;
    }
}

/* Port of libjxl CreateRecursiveGaussian (lib/jxl/gauss_blur.cc).
 * Charalampidis [2016] "Recursive Implementation of the Gaussian Filter
 * Using Truncated Cosine Functions", with k={1,3,5}. Writes n2[], d1[],
 * and radius into coeffs. Pure host double math. */
static inline void ssimu2_compute_gaussian_coeffs(double sigma, Ssimu2GaussianCoeffs *coeffs)
{
    const double radius = round(3.2795 * sigma + 0.2546); /* (57), "N" */

    const double pi_div_2r = SSIMU2_PI / (2.0 * radius);
    const double omega[3] = {pi_div_2r, 3.0 * pi_div_2r, 5.0 * pi_div_2r};

    /* (37): poles p_k */
    const double p1 = +1.0 / tan(0.5 * omega[0]);
    const double p3 = -1.0 / tan(0.5 * omega[1]);
    const double p5 = +1.0 / tan(0.5 * omega[2]);

    /* (44): residues r_k */
    const double r1 = +p1 * p1 / sin(omega[0]);
    const double r3 = -p3 * p3 / sin(omega[1]);
    const double r5 = +p5 * p5 / sin(omega[2]);

    /* (50): rho_k */
    const double neg_half_sigma2 = -0.5 * sigma * sigma;
    const double recip_r = 1.0 / radius;
    double rho[3];
    for (int i = 0; i < 3; i++) {
        rho[i] = exp(neg_half_sigma2 * omega[i] * omega[i]) * recip_r;
    }

    /* (52): determinants and zetas */
    const double D13 = p1 * r3 - r1 * p3;
    const double D35 = p3 * r5 - r3 * p5;
    const double D51 = p5 * r1 - r5 * p1;
    const double recip_d13 = 1.0 / D13;
    const double zeta_15 = D35 * recip_d13;
    const double zeta_35 = D51 * recip_d13;

    /* A * beta = gamma; solve via Cramer's rule. */
    const double A[3][3] = {{p1, p3, p5}, {r1, r3, r5}, {zeta_15, zeta_35, 1.0}};
    const double gamma[3] = {1.0, radius * radius - sigma * sigma, /* (55) */
                             zeta_15 * rho[0] + zeta_35 * rho[1] + rho[2]};
    double beta[3];
    ssimu2_solve_cramer_3x3(A, gamma, beta);

    /* (33): final coefficients */
    coeffs->radius = (int)radius;
    for (int i = 0; i < 3; i++) {
        coeffs->n2[i] = (float)(-beta[i] * cos(omega[i] * (radius + 1.0)));
        coeffs->d1[i] = (float)(-2.0 * cos(omega[i]));
    }
}

#endif /* SSIMULACRA2_GAUSSIAN_H_ */
