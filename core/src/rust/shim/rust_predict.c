/* SPDX-License-Identifier: EUPL-1.2 */
/* Copyright 2026 Lusoris */

/**
 * @file rust_predict.c
 * @brief The Rust predictor's table for `predict.c` (RC4 lane P, ADR-1713).
 *
 * Builds `VmafxRsModelView` from `struct VmafModel` / `struct svm_model`, maps
 * the Rust status codes back to the errno values `predict.c` returns, and
 * installs the three entry points as the `VmafRustPredictOps` table of
 * predict.h. A direct source of the libvmaf library target (rust_shim_sources),
 * so only libvmaf links the Rust archive.
 */

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "model.h"
#include "predict.h"
#include "rust/include/vmafx_rs.h"
#include "rust/include/vmafx_rs_predict.h"
#include "rust/shim/rust_predict.h"
#include "svm.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit, built with cl.exe on
 * Windows, whose C23 feature set has no `nullptr`. ADR-1138. */

/* The scratch the Rust predictor fills is the C predictor's own `predict_nodes`. */
_Static_assert(sizeof(struct svm_node) == 16, "svm_node layout is part of the Rust ABI");
_Static_assert(offsetof(struct svm_node, value) == 8, "svm_node layout is part of the Rust ABI");

#define RUST_PREDICT_GUIDING "adm3"
#define RUST_PREDICT_GUIDED "speed_chroma"

/* Arrays the view borrows; freed after `vmafx_rs_model_new` copied them. */
typedef struct RustPredictArrays {
    double *feature_slope;
    double *feature_intercept;
    uint8_t *feature_guiding;
    uint8_t *feature_guided;
    double *knot_x;
    double *knot_y;
    uint32_t *sv_start;
    int32_t *sv_index;
    double *sv_value;
} RustPredictArrays;

static void arrays_free(RustPredictArrays *a)
{
    free(a->feature_slope);
    free(a->feature_intercept);
    free(a->feature_guiding);
    free(a->feature_guided);
    free(a->knot_x);
    free(a->knot_y);
    free(a->sv_start);
    free(a->sv_index);
    free(a->sv_value);
}

static int fill_features(const VmafModel *model, RustPredictArrays *a, VmafxRsModelView *v)
{
    const unsigned n = model->n_features;
    a->feature_slope = (double *)calloc(n, sizeof(double));
    a->feature_intercept = (double *)calloc(n, sizeof(double));
    a->feature_guiding = (uint8_t *)calloc(n, sizeof(uint8_t));
    a->feature_guided = (uint8_t *)calloc(n, sizeof(uint8_t));
    if (!a->feature_slope || !a->feature_intercept || !a->feature_guiding || !a->feature_guided)
        return -ENOMEM;
    for (unsigned i = 0; i < n; i++) {
        a->feature_slope[i] = model->feature[i].slope;
        a->feature_intercept[i] = model->feature[i].intercept;
        a->feature_guiding[i] = strstr(model->feature[i].name, RUST_PREDICT_GUIDING) != NULL;
        a->feature_guided[i] = strstr(model->feature[i].name, RUST_PREDICT_GUIDED) != NULL;
    }
    v->n_features = n;
    v->feature_slope = a->feature_slope;
    v->feature_intercept = a->feature_intercept;
    v->feature_guiding = a->feature_guiding;
    v->feature_guided = a->feature_guided;
    return 0;
}

static int fill_transform(const VmafModel *model, RustPredictArrays *a, VmafxRsModelView *v)
{
    v->transform_enabled = model->score_transform.enabled;
    v->p0_enabled = model->score_transform.p0.enabled;
    v->p0 = model->score_transform.p0.value;
    v->p1_enabled = model->score_transform.p1.enabled;
    v->p1 = model->score_transform.p1.value;
    v->p2_enabled = model->score_transform.p2.enabled;
    v->p2 = model->score_transform.p2.value;
    v->out_lte_in = model->score_transform.out_lte_in;
    v->out_gte_in = model->score_transform.out_gte_in;
    v->knots_enabled = model->score_transform.knots.enabled;
    if (!model->score_transform.knots.enabled)
        return 0;
    a->knot_x = (double *)calloc(model->score_transform.knots.n_knots, sizeof(double));
    a->knot_y = (double *)calloc(model->score_transform.knots.n_knots, sizeof(double));
    if (!a->knot_x || !a->knot_y)
        return -ENOMEM;
    for (unsigned i = 0; i < model->score_transform.knots.n_knots; i++) {
        a->knot_x[i] = model->score_transform.knots.list[i].x;
        a->knot_y[i] = model->score_transform.knots.list[i].y;
    }
    v->n_knots = model->score_transform.knots.n_knots;
    v->knot_x = a->knot_x;
    v->knot_y = a->knot_y;
    return 0;
}

/* Number of nodes before the `index == -1` terminator of one support vector. */
static unsigned sv_node_count(const struct svm_node *sv)
{
    unsigned n = 0;
    while (sv[n].index != -1)
        n++;
    return n;
}

static int fill_sv_arrays(const struct svm_model *svm, RustPredictArrays *a, unsigned total)
{
    a->sv_start = (uint32_t *)calloc((size_t)svm->l + 1, sizeof(uint32_t));
    a->sv_index = (int32_t *)calloc(total ? total : 1, sizeof(int32_t));
    a->sv_value = (double *)calloc(total ? total : 1, sizeof(double));
    if (!a->sv_start || !a->sv_index || !a->sv_value)
        return -ENOMEM;
    unsigned at = 0;
    for (int i = 0; i < svm->l; i++) {
        a->sv_start[i] = at;
        for (const struct svm_node *p = svm->SV[i]; p->index != -1; p++) {
            a->sv_index[at] = p->index;
            a->sv_value[at] = p->value;
            at++;
        }
    }
    a->sv_start[svm->l] = at;
    return 0;
}

static int fill_svm(const struct svm_model *svm, RustPredictArrays *a, VmafxRsModelView *v)
{
    if (svm->l < 0 || !svm->sv_coef || !svm->sv_coef[0] || !svm->rho)
        return -EINVAL;
    uint64_t total = 0;
    for (int i = 0; i < svm->l; i++)
        total += sv_node_count(svm->SV[i]);
    if (total > UINT32_MAX)
        return -EOVERFLOW;
    const int err = fill_sv_arrays(svm, a, (unsigned)total);
    if (err)
        return err;
    v->svm_type = svm->param.svm_type;
    v->kernel_type = svm->param.kernel_type;
    v->degree = svm->param.degree;
    v->gamma = svm->param.gamma;
    v->coef0 = svm->param.coef0;
    v->n_sv = (uint32_t)svm->l;
    v->sv_coef = svm->sv_coef[0];
    v->rho = svm->rho[0];
    v->sv_start = a->sv_start;
    v->n_nodes = (uint32_t)total;
    v->sv_index = a->sv_index;
    v->sv_value = a->sv_value;
    return 0;
}

static int fill_view(const VmafModel *model, RustPredictArrays *a, VmafxRsModelView *v)
{
    v->norm_type = (uint32_t)model->norm_type;
    v->slope = model->slope;
    v->intercept = model->intercept;
    v->chroma_enabled =
        model->chroma_from_luma.enabled && model->chroma_from_luma.chroma_correction_parameter != 0;
    v->chroma_parameter = model->chroma_from_luma.chroma_correction_parameter;
    v->clip_enabled = model->score_clip.enabled;
    v->clip_min = model->score_clip.min;
    v->clip_max = model->score_clip.max;
    int err = fill_features(model, a, v);
    if (!err)
        err = fill_transform(model, a, v);
    if (!err)
        err = fill_svm(model->svm, a, v);
    return err;
}

static int errno_of(int32_t status)
{
    switch (status) {
    case VMAFX_RS_E_NOTSUP:
        return -ENOTSUP;
    case VMAFX_RS_E_NOMEM:
        return -ENOMEM;
    default:
        return -EINVAL;
    }
}

static int rust_predict_create(const VmafModel *model, void **handle)
{
    if (!model || !handle || !model->svm || model->n_features == 0)
        return -EINVAL;
    assert(model->n_features > 0);
    VmafxRsModelView view;
    memset(&view, 0, sizeof(view));
    RustPredictArrays arrays;
    memset(&arrays, 0, sizeof(arrays));

    int err = fill_view(model, &arrays, &view);
    VmafxRsModel *rs = NULL;
    if (!err) {
        const int32_t status = vmafx_rs_model_new(&view, &rs);
        err = status == VMAFX_RS_OK ? 0 : errno_of(status);
    }
    arrays_free(&arrays);
    if (err)
        return err;
    *handle = rs;
    return 0;
}

static const char *stage_name(uint32_t stage)
{
    switch (stage) {
    case 1:
        return "score transform";
    case 2:
        return "piecewise score";
    default:
        return "model score";
    }
}

static int rust_predict_score(const VmafModel *model, void *handle, const double *scores,
                              enum VmafModelFlags flags, struct svm_node *nodes, unsigned index,
                              double *prediction)
{
    VmafxRsPredictFail fail = {0};
    const int32_t status = vmafx_rs_model_predict((const VmafxRsModel *)handle, scores,
                                                  model->n_features, (uint32_t)flags, nodes,
                                                  (size_t)model->n_features + 1, prediction, &fail);
    if (status == VMAFX_RS_OK)
        return 0;
    if (status == VMAFX_RS_E_NONFINITE)
        vmaf_log(VMAF_LOG_LEVEL_WARNING,
                 "predict: non-finite %s at frame %u (value=%g), failing frame\n",
                 stage_name(fail.stage), index, fail.value);
    return errno_of(status);
}

static void rust_predict_destroy(void *handle)
{
    vmafx_rs_model_free((VmafxRsModel *)handle);
}

static const struct VmafRustPredictOps rust_predict_ops = {
    .create = rust_predict_create,
    .predict = rust_predict_score,
    .destroy = rust_predict_destroy,
};

void vmaf_rust_predict_install(void)
{
    vmaf_predict_install_rust_ops(&rust_predict_ops);
}

/* NOLINTEND(modernize-use-nullptr) */
