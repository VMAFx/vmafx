/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Implementation of the SpEED internal API declared in speed_internal.h.
 *  Provides the CPU-side helpers that the GPU twins
 *  (feature/{cuda,hip,sycl}/speed_{chroma,temporal}_*) call to set up
 *  dimensions, run the small-matrix linear algebra (eigendecomposition,
 *  QR factorisation, Q^T multiply), filter and downscale frames, and
 *  decide whether the covariance matrix is regular.
 *
 *  The math here is a self-contained port of the static helpers in
 *  speed.c (the CPU SpEED extractor).  It is kept in its own TU rather
 *  than exposed from speed.c because:
 *    - speed.c is a Netflix-mirrored file (Netflix copyright header)
 *      and removing its `static` qualifiers to share them with GPU
 *      twins would dirty an upstream-tracked TU on every rebase.
 *    - The GPU TUs share an identical algorithm with the CPU path; the
 *      cost of duplication is bounded (a handful of small, well-tested
 *      pure-math helpers — no global state, no I/O), and the resulting
 *      isolation means a GPU-side regression cannot drift the CPU
 *      golden-data gate.
 *
 *  See ADR-0964 for the wiring rationale and PR #465 / PR #466 for the
 *  cross-backend audits that surfaced the missing TU.
 */

#include "feature/speed_internal.h"

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "log.h"
#include "mem.h"
#include "vif_tools.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SI_EIGENVALUE_EPS (1e-6f)
#define SI_MAX(x, y) ((x) > (y) ? (x) : (y))
#define SI_ALMOST_EQUAL(x, c) (fabs((x) - (c)) < 1.0e-3)

/* ------------------------------------------------------------------ */
/* Dimensions + stride                                                 */
/* ------------------------------------------------------------------ */

int speed_internal_init_dimensions(SpeedInternalDimensions *dim, int w, int h, double prescale)
{
    assert(dim != NULL);
    assert(w > 0);
    assert(h > 0);

    dim->original_height = (size_t)h;
    dim->original_width = (size_t)w;
    dim->scaled_height = (size_t)lround((double)dim->original_height * prescale);
    dim->scaled_width = (size_t)lround((double)dim->original_width * prescale);
    dim->alloc_height = SI_MAX(dim->original_height, dim->scaled_height);
    dim->alloc_width = SI_MAX(dim->original_width, dim->scaled_width);
    dim->operating_height = dim->scaled_height >> SPEED_INTERNAL_NUM_SCALES;
    dim->operating_width = dim->scaled_width >> SPEED_INTERNAL_NUM_SCALES;
    dim->block_size = SPEED_INTERNAL_BLOCK_SIZE;
    dim->truncated_width = (dim->operating_width / dim->block_size) * dim->block_size;
    dim->truncated_height = (dim->operating_height / dim->block_size) * dim->block_size;
    dim->num_blocks_horizontal = dim->truncated_width / dim->block_size;
    dim->num_blocks_vertical = dim->truncated_height / dim->block_size;
    dim->num_blocks = dim->num_blocks_horizontal * dim->num_blocks_vertical;
    dim->elements_in_block = dim->block_size * dim->block_size;
    dim->submatrix_width =
        dim->truncated_width >= dim->block_size ? dim->truncated_width - dim->block_size + 1 : 0;
    dim->submatrix_height =
        dim->truncated_height >= dim->block_size ? dim->truncated_height - dim->block_size + 1 : 0;

    if (dim->truncated_height == 0 || dim->truncated_width == 0) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "SpEED: image too small, operating width or height is 0\n");
        return -EINVAL;
    }
    return 0;
}

size_t speed_internal_float_stride(size_t alloc_width)
{
    return ALIGN_CEIL(alloc_width * sizeof(float));
}

/* ------------------------------------------------------------------ */
/* Filter + downscale (mirror of static filter_and_downscale in speed.c) */
/* ------------------------------------------------------------------ */

static void si_subtract_image(float *im1, const float *im2, int w, int h, size_t stride)
{
    const size_t stride_px = stride / sizeof(float);
    for (int i = 0; i < h; i++) {
        for (int j = 0; j < w; j++) {
            im1[(size_t)i * stride_px + (size_t)j] -= im2[(size_t)i * stride_px + (size_t)j];
        }
    }
}

void speed_internal_filter_and_downscale(const SpeedInternalDimensions *dim,
                                         SpeedInternalOptions *opt, float *frame_buffer,
                                         float *tmp_buffer, size_t float_stride)
{
    assert(opt != NULL);
    assert(frame_buffer != NULL);
    assert(tmp_buffer != NULL);

    const size_t stride_px = float_stride / sizeof(float);
    const size_t frame_size = stride_px * dim->alloc_height;
    float *curr_scale = tmp_buffer;
    float *tmpbuf = tmp_buffer + frame_size;

    /* Scaling-method validity is checked at init by the caller; here we just decode the string. */
    enum vif_scaling_method scaling_method;
    (void)vif_get_scaling_method(opt->speed_prescale_method, &scaling_method);

    if (!SI_ALMOST_EQUAL(opt->speed_prescale, 1.0)) {
        (void)memcpy(tmpbuf, frame_buffer, stride_px * dim->alloc_height * sizeof(float));
        vif_scale_frame_s(scaling_method, tmpbuf, frame_buffer, (int)dim->original_width,
                          (int)dim->original_height, (int)stride_px, (int)dim->scaled_width,
                          (int)dim->scaled_height, (int)stride_px);
    }

    const float kernelscale = (float)opt->speed_kernelscale;
    const int filter_width_antialias = vif_get_filter_size(1, kernelscale);
    float filter_antialias[128];
    speed_get_antialias_filter(filter_antialias, SPEED_INTERNAL_NUM_SCALES, kernelscale);
    vif_filter1d_s(filter_antialias, frame_buffer, curr_scale, tmpbuf, (int)dim->scaled_width,
                   (int)dim->scaled_height, (int)float_stride, (int)float_stride,
                   filter_width_antialias);

    vif_dec16_s(curr_scale, frame_buffer, (int)dim->scaled_width, (int)dim->scaled_height,
                (int)float_stride, (int)float_stride);

    const size_t downscaled_w = dim->scaled_width >> SPEED_INTERNAL_NUM_SCALES;
    const size_t downscaled_h = dim->scaled_height >> SPEED_INTERNAL_NUM_SCALES;

    const int filter_width = vif_get_filter_size(SPEED_INTERNAL_NUM_SCALES, kernelscale);
    float filter[128];
    vif_get_filter(filter, SPEED_INTERNAL_NUM_SCALES, kernelscale);
    vif_filter1d_s(filter, frame_buffer, curr_scale, tmpbuf, (int)downscaled_w, (int)downscaled_h,
                   (int)float_stride, (int)float_stride, filter_width);
    si_subtract_image(frame_buffer, curr_scale, (int)downscaled_w, (int)downscaled_h, float_stride);
}

/* ------------------------------------------------------------------ */
/* Covariance matrix (CPU reference; production GPU paths skip this)   */
/* ------------------------------------------------------------------ */

static float si_compute_mean(const SpeedInternalDimensions *dim, const float *data,
                             size_t stride_px, size_t start_row, size_t start_col)
{
    float result = 0.0f;
    for (size_t i = 0; i < dim->submatrix_height; i++) {
        for (size_t j = 0; j < dim->submatrix_width; j++) {
            result += data[(start_row + i) * stride_px + (start_col + j)];
        }
    }
    const float denom = (float)(dim->submatrix_width * dim->submatrix_height);
    return denom > 0.0f ? result / denom : 0.0f;
}

static float si_compute_covariance(const SpeedInternalDimensions *dim, const float *data,
                                   const float *means, size_t stride_px, size_t start_row_x,
                                   size_t start_col_x, size_t start_row_y, size_t start_col_y)
{
    const double mean_x = means[start_row_x * dim->block_size + start_col_x];
    const double mean_y = means[start_row_y * dim->block_size + start_col_y];
    double result = 0.0;
    for (size_t i = 0; i < dim->submatrix_height; i++) {
        for (size_t j = 0; j < dim->submatrix_width; j++) {
            const double val_x = data[(start_row_x + i) * stride_px + (start_col_x + j)];
            const double val_y = data[(start_row_y + i) * stride_px + (start_col_y + j)];
            result += (val_x - mean_x) * (val_y - mean_y);
        }
    }
    const double denom = (double)(dim->submatrix_width * dim->submatrix_height);
    return denom > 0.0 ? (float)(result / denom) : 0.0f;
}

void speed_internal_compute_cov_matrix(const SpeedInternalDimensions *dim, const float *data,
                                       float *cov_mat, float *tmp_means, size_t stride_px)
{
    assert(data != NULL);
    assert(cov_mat != NULL);
    assert(tmp_means != NULL);
    assert(dim->block_size > 0);

    for (size_t start_row = 0; start_row < dim->block_size; start_row++) {
        for (size_t start_col = 0; start_col < dim->block_size; start_col++) {
            tmp_means[start_row * dim->block_size + start_col] =
                si_compute_mean(dim, data, stride_px, start_row, start_col);
        }
    }
    const size_t elements_in_block = dim->block_size * dim->block_size;

    for (size_t x_index = 0; x_index < dim->elements_in_block; x_index++) {
        for (size_t y_index = 0; y_index <= x_index; y_index++) {
            const size_t start_row_x = x_index / dim->block_size;
            const size_t start_col_x = x_index % dim->block_size;
            const size_t start_row_y = y_index / dim->block_size;
            const size_t start_col_y = y_index % dim->block_size;
            const float cov = si_compute_covariance(dim, data, tmp_means, stride_px, start_row_x,
                                                    start_col_x, start_row_y, start_col_y);
            cov_mat[x_index * elements_in_block + y_index] = cov;
            cov_mat[y_index * elements_in_block + x_index] = cov;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Eigenvalues — Householder tridiagonalisation + implicit-shift QR    */
/* ------------------------------------------------------------------ */

static int si_get_sign(float x)
{
    return (x >= 0.0f ? 1 : -1);
}

static float si_pythagoras(float x, float y)
{
    return sqrtf(x * x + y * y);
}

static float si_column_norm(const float *A, int col, int start_row, int size)
{
    float norm = 0.0f;
    for (int i = start_row; i < size; i++) {
        const float v = A[i * size + col];
        norm += v * v;
    }
    return sqrtf(norm);
}

static float si_householder_transform(float *A, int col, int start_row, int size)
{
    if (size - start_row == 1) {
        return 0.0f;
    }
    const float xnorm = si_column_norm(A, col, start_row + 1, size);
    if (xnorm == 0.0f) {
        return 0.0f;
    }
    const float alpha = A[start_row * size + col];
    const float beta = -si_get_sign(alpha) * si_pythagoras(alpha, xnorm);
    const float tau = (beta - alpha) / beta;
    const float s = alpha - beta;
    if (s != 0.0f) {
        for (int i = start_row; i < size; i++) {
            A[i * size + col] /= s;
        }
        A[start_row * size + col] = beta;
    }
    return tau;
}

static void si_tri_multiply(const float *A, const float *v, float *x, float tau_i, int start,
                            int size)
{
    for (int i = start; i < size; i++) {
        x[i] = 0.0f;
        for (int j = start; j < size; j++) {
            x[i] += tau_i * A[i * size + j] * v[j];
        }
    }
}

static float si_tri_dot(const float *x, const float *v, int start, int size)
{
    float res = 0.0f;
    for (int i = start; i < size; i++) {
        res += x[i] * v[i];
    }
    return res;
}

static void si_tri_axpy(float *x, const float *v, float alpha, int start, int size)
{
    for (int i = start; i < size; i++) {
        x[i] += alpha * v[i];
    }
}

static void si_tri_syr2(float *A, const float *x, const float *v, int start, int size)
{
    for (int i = start; i < size; i++) {
        for (int j = start; j < size; j++) {
            A[i * size + j] -= x[i] * v[j] + v[i] * x[j];
        }
    }
}

static void si_convert_to_tridiagonal(float *A, int size, float *d, float *sd, float *buffer)
{
    float *v = buffer;
    float *x = buffer + size;

    for (int i = 0; i < size - 2; i++) {
        const float tau_i = si_householder_transform(A, i, i + 1, size);
        for (int j = i + 1; j < size; j++) {
            v[j] = A[j * size + i];
        }
        if (tau_i != 0.0f) {
            A[(i + 1) * size + i] = v[i + 1];
            v[i + 1] = 1.0f;
            si_tri_multiply(A, v, x, tau_i, i + 1, size);
            const float xv = si_tri_dot(x, v, i + 1, size);
            const float alpha = -0.5f * tau_i * xv;
            si_tri_axpy(x, v, alpha, i + 1, size);
            si_tri_syr2(A, x, v, i + 1, size);
        }
    }

    for (int i = 0; i < size; i++) {
        d[i] = A[i * size + i];
    }
    for (int i = 0; i < size - 1; i++) {
        sd[i] = A[(i + 1) * size + i];
    }
}

static void si_chop_small(float *d, float *sd, int size)
{
    for (int i = 0; i < size - 1; i++) {
        if (fabsf(sd[i]) < SI_EIGENVALUE_EPS * (fabsf(d[i]) + fabsf(d[i + 1]))) {
            sd[i] = 0.0f;
        }
    }
}

static float si_trailing_eigenvalue(const float *d, const float *sd, int n)
{
    const float ta = d[n - 2];
    const float tb = d[n - 1];
    const float tab = sd[n - 2];
    const float dt = (ta - tb) * 0.5f;

    if (dt > 0.0f) {
        return tb - tab * (tab / (dt + si_pythagoras(dt, tab)));
    } else if (dt == 0.0f) {
        return tb - fabsf(tab);
    } else {
        return tb + tab * (tab / (-dt + si_pythagoras(dt, tab)));
    }
}

static void si_create_givens(float a, float b, float *c, float *s)
{
    if (b == 0.0f) {
        *c = 1.0f;
        *s = 0.0f;
    } else if (fabsf(b) > fabsf(a)) {
        const float t = -a / b;
        const float s1 = 1.0f / sqrtf(1.0f + t * t);
        *s = s1;
        *c = s1 * t;
    } else {
        const float t = -b / a;
        const float c1 = 1.0f / sqrtf(1.0f + t * t);
        *c = c1;
        *s = c1 * t;
    }
}

static void si_qr_step_size2(float *d, float *sd, float x, float z)
{
    const float ap = d[0];
    const float bp = sd[0];
    const float aq = d[1];

    float c;
    float s;
    si_create_givens(x, z, &c, &s);

    const float ak = c * (c * ap - s * bp) + s * (s * aq - c * bp);
    const float bk = c * (s * ap + c * bp) - s * (s * bp + c * aq);
    const float ap_new = s * (s * ap + c * bp) + c * (s * bp + c * aq);

    d[0] = ak;
    sd[0] = bk;
    d[1] = ap_new;
}

static void si_qr_step_general(float *d, float *sd, int n, float x, float z)
{
    float ak = 0.0f;
    float bk = 0.0f;
    float zk = 0.0f;
    float ap = d[0];
    float bp = sd[0];
    float aq = d[1];
    float bq = sd[1];

    for (int k = 0; k < n - 1; k++) {
        float c;
        float s;
        si_create_givens(x, z, &c, &s);

        const float bk1 = c * bk - s * zk;
        const float ap1 = c * (c * ap - s * bp) + s * (s * aq - c * bp);
        const float bp1 = c * (s * ap + c * bp) - s * (s * bp + c * aq);
        const float zp1 = -s * bq;
        const float aq1 = s * (s * ap + c * bp) + c * (s * bp + c * aq);
        const float bq1 = c * bq;

        ak = ap1;
        bk = bp1;
        zk = zp1;
        ap = aq1;
        bp = bq1;

        if (k < n - 2) {
            aq = d[k + 2];
        }
        if (k < n - 3) {
            bq = sd[k + 2];
        }

        d[k] = ak;
        if (k > 0) {
            sd[k - 1] = bk1;
        }
        if (k < n - 2) {
            sd[k + 1] = bp;
        }

        x = bk;
        z = zk;
    }

    d[n - 1] = ap;
    sd[n - 2] = bk;
}

static void si_qr_step(float *d, float *sd, int n)
{
    float mu = si_trailing_eigenvalue(d, sd, n);
    if (SI_EIGENVALUE_EPS * fabsf(mu) > fabsf(d[0]) + fabsf(sd[0])) {
        mu = 0.0f;
    }

    const float x = d[0] - mu;
    const float z = sd[0];

    if (n == 2) {
        si_qr_step_size2(d, sd, x, z);
        return;
    }
    si_qr_step_general(d, sd, n, x, z);
}

static void si_compute_eigenvalues_tridiagonal(float *d, float *sd, float *eigenvalues, int size)
{
    si_chop_small(d, sd, size);

    int b = size - 1;
    int iter = 0;
    while (b > 0 && iter < SPEED_INTERNAL_EIGENVALUE_MAX_ITERS) {
        if (sd[b - 1] == 0.0f) {
            b--;
            continue;
        }
        int a = b - 1;
        while (a > 0) {
            if (sd[a - 1] == 0.0f) {
                break;
            }
            a--;
        }

        const int n_block = b - a + 1;
        float *d_block = d + a;
        float *sd_block = sd + a;
        si_qr_step(d_block, sd_block, n_block);
        si_chop_small(d_block, sd_block, n_block);
        iter++;
    }

    if (iter == SPEED_INTERNAL_EIGENVALUE_MAX_ITERS) {
        vmaf_log(VMAF_LOG_LEVEL_WARNING,
                 "speed_internal: eigenvalue QR iteration reached cap, possible non-convergence\n");
    }

    for (int i = 0; i < size; i++) {
        eigenvalues[i] = d[i];
    }
}

void speed_internal_compute_eigenvalues(const float *A_immutable, float *eigenvalues, int size,
                                        float *buffer)
{
    assert(A_immutable != NULL);
    assert(eigenvalues != NULL);
    assert(buffer != NULL);
    assert(size > 0);

    float *A = buffer;
    float *d = buffer + (size_t)size * (size_t)size;
    float *sd = d + size;
    float *tmp = sd + size;

    (void)memcpy(A, A_immutable, (size_t)size * (size_t)size * sizeof(float));

    if (size == 1) {
        eigenvalues[0] = A[0];
        return;
    }
    si_convert_to_tridiagonal(A, size, d, sd, tmp);
    si_compute_eigenvalues_tridiagonal(d, sd, eigenvalues, size);
}

/* ------------------------------------------------------------------ */
/* QR factorisation (Householder reflections)                          */
/* ------------------------------------------------------------------ */

static void si_mat_zero(float *m, int rows, int cols)
{
    (void)memset(m, 0, (size_t)rows * (size_t)cols * sizeof(float));
}

static void si_mat_identity(float *m, int size)
{
    si_mat_zero(m, size, size);
    for (int i = 0; i < size; i++) {
        m[i * size + i] = 1.0f;
    }
}

static void si_mat_copy(float *dst, const float *src, int rows, int cols)
{
    (void)memcpy(dst, src, (size_t)rows * (size_t)cols * sizeof(float));
}

static void si_mat_mul(float *dst, const float *x, const float *y, int size)
{
    si_mat_zero(dst, size, size);
    for (int i = 0; i < size; i++) {
        for (int k = 0; k < size; k++) {
            const float xik = x[i * size + k];
            for (int j = 0; j < size; j++) {
                dst[i * size + j] += xik * y[k * size + j];
            }
        }
    }
}

static void si_mat_transpose(float *m, int size)
{
    for (int i = 0; i < size; i++) {
        for (int j = 0; j < i; j++) {
            const float tmp = m[i * size + j];
            m[i * size + j] = m[j * size + i];
            m[j * size + i] = tmp;
        }
    }
}

static void si_mat_minor(float *m, int size, int d)
{
    for (int i = 0; i < size; i++) {
        for (int j = 0; j < size; j++) {
            if (i < d || j < d) {
                m[i * size + j] = (i == j) ? 1.0f : 0.0f;
            }
        }
    }
}

static void si_take_kth_col(const float *m, float *v, int size, int k)
{
    for (int i = 0; i < size; i++) {
        v[i] = m[i * size + k];
    }
}

static float si_vec_norm(const float *x, int n)
{
    float sum = 0.0f;
    for (int i = 0; i < n; i++) {
        sum += x[i] * x[i];
    }
    return sqrtf(sum);
}

static void si_vec_div(const float *x, float d, float *y, int n)
{
    for (int i = 0; i < n; i++) {
        y[i] = x[i] / d;
    }
}

static void si_identity_minus_v_vt(float *dst, const float *v, int size)
{
    for (int i = 0; i < size; i++) {
        for (int j = 0; j < size; j++) {
            dst[i * size + j] = -2.0f * v[i] * v[j];
        }
    }
    for (int i = 0; i < size; i++) {
        dst[i * size + i] += 1.0f;
    }
}

int speed_internal_qr_factorize(float *A_data, int size, float *Q_out, float *R_out, float *tmp)
{
    assert(A_data != NULL);
    assert(Q_out != NULL);
    assert(R_out != NULL);
    assert(tmp != NULL);
    assert(size > 0);

    /* Workspace layout (3 × size² floats): tmp_q | tmp_z | tmp_mul.
     * R_out doubles as a working vector during the loop (see speed.c's
     * matrix_qr_decomposition where `vec = R->data`).  After the loop
     * the final R is assembled into R_out via R = Q × A. */
    float *tmp_q = tmp;
    float *tmp_z = tmp + (size_t)size * (size_t)size;
    float *tmp_mul = tmp_z + (size_t)size * (size_t)size;
    float *vec = R_out;

    si_mat_copy(tmp_z, A_data, size, size);
    si_mat_identity(Q_out, size);

    for (int k = 0; k < size - 1; k++) {
        si_mat_minor(tmp_z, size, k);
        si_take_kth_col(tmp_z, vec, size, k);

        const float norm = si_vec_norm(vec, size);
        const int sign = si_get_sign(A_data[k * size + k]);
        vec[k] += (float)sign * norm;

        const float vn = si_vec_norm(vec, size);
        if (vn == 0.0f) {
            continue;
        }
        si_vec_div(vec, vn, vec, size);
        si_identity_minus_v_vt(tmp_q, vec, size);

        si_mat_mul(tmp_mul, tmp_q, tmp_z, size);
        si_mat_copy(tmp_z, tmp_mul, size, size);
        si_mat_mul(tmp_mul, tmp_q, Q_out, size);
        si_mat_copy(Q_out, tmp_mul, size, size);
    }

    /* R = Q × A; then transpose Q so Q_out holds the true orthogonal factor
     * (matches matrix_qr_decomposition in speed.c). */
    si_mat_mul(R_out, Q_out, A_data, size);
    si_mat_transpose(Q_out, size);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Q^T × B multiply (B is overwritten in place)                        */
/* ------------------------------------------------------------------ */

void speed_internal_qt_multiply(const float *Q, float *B, int size, int num_cols, float *tmp)
{
    assert(Q != NULL);
    assert(B != NULL);
    assert(tmp != NULL);
    assert(size > 0);
    assert(num_cols > 0);

    /* tmp = Q^T × B; then copy back into B. */
    for (int i = 0; i < size; i++) {
        for (int j = 0; j < num_cols; j++) {
            float acc = 0.0f;
            for (int k = 0; k < size; k++) {
                /* Q^T[i,k] = Q[k,i]. */
                acc += Q[k * size + i] * B[k * num_cols + j];
            }
            tmp[i * num_cols + j] = acc;
        }
    }
    (void)memcpy(B, tmp, (size_t)size * (size_t)num_cols * sizeof(float));
}

/* ------------------------------------------------------------------ */
/* Backward substitution: R × X = B, in place into B                   */
/* ------------------------------------------------------------------ */

int speed_internal_backward_substitution(const float *R, float *B, int size, int num_cols)
{
    assert(R != NULL);
    assert(B != NULL);
    assert(size > 0);
    assert(num_cols > 0);

    for (int i = size - 1; i >= 0; i--) {
        const float denom = R[i * size + i];
        if (fabsf(denom) < SI_EIGENVALUE_EPS) {
            return -EINVAL;
        }
        for (int j = 0; j < num_cols; j++) {
            float term = B[i * num_cols + j];
            for (int k = i + 1; k < size; k++) {
                term -= B[k * num_cols + j] * R[i * size + k];
            }
            B[i * num_cols + j] = term / denom;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Regularity check                                                    */
/* ------------------------------------------------------------------ */

bool speed_internal_is_matrix_regular(const float *eigenvalues, size_t num_elements)
{
    assert(eigenvalues != NULL);
    for (size_t i = 0; i < num_elements; i++) {
        if (eigenvalues[i] < SI_EIGENVALUE_EPS) {
            return false;
        }
    }
    return true;
}
