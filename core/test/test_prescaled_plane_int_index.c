/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 */

/*
 * float_vif and SpEED refuse a prescaled plane that the int index of
 * vif_tools.c cannot address (T-PRESCALED-PLANE-INT-INDEX-2026-10-05).
 *
 * vif_tools.c indexes a plane as y * stride + x in int. With vif_prescale or
 * speed_prescale p the plane is (W * p) x (H * p): at the 32768 x 32768
 * picture cap it passes INT_MAX samples from p = 1.4142, while 16K
 * (15360 x 8640) stays below at the option maximum p = 4. Every refusal below
 * returns from init() before the extractor allocates a buffer, so no test
 * allocates a plane of that size. speed_internal_init_dimensions() is the
 * geometry of every SpEED device twin; on master it accepted the cap at
 * p = 1.5.
 */

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "libvmaf/picture.h"

#include "opt.h"
#include "test.h"

#include "feature/feature_extractor.h"
#include "feature/speed_internal.h"
#include "feature/vif_tools.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. ADR-1138. */

#define PIC_CAP 32768u

static void close_and_free(VmafFeatureExtractor *fex, void *priv)
{
    if (fex->close) {
        (void)fex->close(fex);
    }
    free(priv);
    fex->priv = NULL;
}

/* init() of `name` on a w x h 4:4:4 8-bit picture with option `opt` = `val`
 * and every other option at its default. */
static int init_with_option(const char *name, unsigned w, unsigned h, const char *opt,
                            const char *val)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name(name);
    if (!fex || !fex->options) {
        return -ENOENT;
    }
    void *priv = calloc(1, fex->priv_size);
    if (!priv) {
        return -ENOMEM;
    }
    fex->priv = priv;
    int err = 0;
    for (unsigned i = 0; fex->options[i].name && !err; i++) {
        const int match = strcmp(fex->options[i].name, opt) == 0;
        err = vmaf_option_set(&fex->options[i], priv, match ? val : NULL);
    }
    if (err) {
        close_and_free(fex, priv);
        return err;
    }
    const int rc = fex->init(fex, VMAF_PIX_FMT_YUV444P, 8u, w, h);
    close_and_free(fex, priv);
    return rc;
}

static char *test_helper_boundary(void)
{
    mu_assert("INT_MAX samples fit", vif_plane_fits_int_index((size_t)INT_MAX, 1u));
    mu_assert("INT_MAX + 1 samples do not", !vif_plane_fits_int_index((size_t)INT_MAX + 1u, 1u));
    mu_assert("46340^2 fits", vif_plane_fits_int_index(46340u, 46340u));
    mu_assert("46341^2 does not", !vif_plane_fits_int_index(46341u, 46341u));
    mu_assert("an empty plane fits", vif_plane_fits_int_index(SIZE_MAX, 0u));
    /* 16K at the option maximum p = 4: 61440 x 34560 = 2,123,366,400. */
    mu_assert("16K at prescale 4 fits", vif_plane_fits_int_index(61440u, 34560u));
    return NULL;
}

static char *test_speed_geometry_refuses_the_cap_above_sqrt2(void)
{
    SpeedInternalDimensions dim;
    memset(&dim, 0, sizeof(dim));
    mu_assert("cap at prescale 1.5 refused",
              speed_internal_init_dimensions(&dim, (int)PIC_CAP, (int)PIC_CAP, 1.5) == -EINVAL);
    mu_assert("cap at prescale 4 refused",
              speed_internal_init_dimensions(&dim, (int)PIC_CAP, (int)PIC_CAP, 4.0) == -EINVAL);
    mu_assert("cap at prescale 1.4 accepted",
              speed_internal_init_dimensions(&dim, (int)PIC_CAP, (int)PIC_CAP, 1.4) == 0);
    mu_assert("16K at prescale 4 accepted",
              speed_internal_init_dimensions(&dim, 15360, 8640, 4.0) == 0);
    return NULL;
}

static char *test_extractors_refuse_the_cap_above_sqrt2(void)
{
    mu_assert("float_vif refuses the cap at vif_prescale 2",
              init_with_option("float_vif", PIC_CAP, PIC_CAP, "vif_prescale", "2.0") == -EINVAL);
    mu_assert("speed_chroma refuses the cap at speed_prescale 1.5",
              init_with_option("speed_chroma", PIC_CAP, PIC_CAP, "speed_prescale", "1.5") ==
                  -EINVAL);
    mu_assert("speed_temporal refuses the cap at speed_prescale 1.5",
              init_with_option("speed_temporal", PIC_CAP, PIC_CAP, "speed_prescale", "1.5") ==
                  -EINVAL);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_helper_boundary);
    mu_run_test(test_speed_geometry_refuses_the_cap_above_sqrt2);
    mu_run_test(test_extractors_refuse_the_cap_above_sqrt2);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
