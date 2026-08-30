/**
 *
 *  Copyright 2016-2026 Netflix, Inc.
 *
 *     Licensed under the BSD+Patent License (the "License");
 *     you may not use this file except in compliance with the License.
 *     You may obtain a copy of the License at
 *
 *         https://opensource.org/licenses/BSDplusPatent
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 *
 */

#include "test.h"
#include "feature/luminance_tools.c"

#define EPS 0.00001

/* Test support function */
int almost_equal(double a, double b)
{
    double diff = a > b ? a - b : b - a;
    return diff < EPS;
}

static char *test_bt1886_eotf()
{
    double L = vmaf_luminance_bt1886_eotf(0.5);
    mu_assert("wrong bt1886_eotf result", almost_equal(L, 58.716634));
    L = vmaf_luminance_bt1886_eotf(0.1);
    mu_assert("wrong bt1886_eotf result", almost_equal(L, 1.576653));
    L = vmaf_luminance_bt1886_eotf(0.9);
    mu_assert("wrong bt1886_eotf result", almost_equal(L, 233.819503));

    return NULL;
}

static char *test_pq_eotf()
{
    double L;
    L = vmaf_luminance_pq_eotf(0.0);
    mu_assert("wrong pq_eotf result", almost_equal(L, 0.0));
    L = vmaf_luminance_pq_eotf(0.1);
    mu_assert("wrong pq_eotf result", almost_equal(L, 0.324565591464487));
    L = vmaf_luminance_pq_eotf(0.3);
    mu_assert("wrong pq_eotf result", almost_equal(L, 10.038226310511750));
    L = vmaf_luminance_pq_eotf(0.8);
    mu_assert("wrong pq_eotf result", almost_equal(L, 1555.178364289284673));

    return NULL;
}

static char *test_range_foot_head()
{
    int foot;
    int head;

    range_foot_head(8, VMAF_PIXEL_RANGE_LIMITED, &foot, &head);
    mu_assert("wrong 'limited' 8b range computation", (foot == 16 && head == 235));
    range_foot_head(8, VMAF_PIXEL_RANGE_FULL, &foot, &head);
    mu_assert("wrong 'full' 8b range computation", (foot == 0 && head == 255));
    range_foot_head(10, VMAF_PIXEL_RANGE_LIMITED, &foot, &head);
    mu_assert("wrong 'limited' 10b range computation", (foot == 64 && head == 940));

    return NULL;
}

static char *test_get_luminance()
{
    VmafLumaRange range_8b_limited;
    vmaf_luminance_init_luma_range(&range_8b_limited, 8, VMAF_PIXEL_RANGE_LIMITED);
    VmafLumaRange range_10b_limited;
    vmaf_luminance_init_luma_range(&range_10b_limited, 10, VMAF_PIXEL_RANGE_LIMITED);
    VmafLumaRange range_10b_full;
    vmaf_luminance_init_luma_range(&range_10b_full, 10, VMAF_PIXEL_RANGE_FULL);

    double L;
    L = vmaf_luminance_get_luminance(100, range_8b_limited, vmaf_luminance_bt1886_eotf);
    mu_assert("wrong 'limited' 8b luminance bt1886", almost_equal(L, 31.68933962217197));
    L = vmaf_luminance_get_luminance(400, range_10b_limited, vmaf_luminance_bt1886_eotf);
    mu_assert("wrong 'limited' 10b luminance bt1886", almost_equal(L, 31.68933962217197));
    L = vmaf_luminance_get_luminance(400, range_10b_full, vmaf_luminance_bt1886_eotf);
    mu_assert("wrong 'full' 10b luminance bt1886", almost_equal(L, 33.133003757557773));

    L = vmaf_luminance_get_luminance(100, range_8b_limited, vmaf_luminance_pq_eotf);
    mu_assert("wrong 'limited' 8b luminance pq", almost_equal(L, 27.048765018959795));
    L = vmaf_luminance_get_luminance(400, range_10b_limited, vmaf_luminance_pq_eotf);
    mu_assert("wrong 'limited' 10b luminance pq", almost_equal(L, 27.048765018959795));
    L = vmaf_luminance_get_luminance(400, range_10b_full, vmaf_luminance_pq_eotf);
    mu_assert("wrong 'full' 10b luminance pq", almost_equal(L, 29.385657130952264));

    return NULL;
}

static char *test_normalize_range()
{
    VmafLumaRange range_8b_limited;
    vmaf_luminance_init_luma_range(&range_8b_limited, 8, VMAF_PIXEL_RANGE_LIMITED);
    VmafLumaRange range_8b_full;
    vmaf_luminance_init_luma_range(&range_8b_full, 10, VMAF_PIXEL_RANGE_FULL);
    VmafLumaRange range_10b_limited;
    vmaf_luminance_init_luma_range(&range_10b_limited, 10, VMAF_PIXEL_RANGE_LIMITED);

    double n = normalize_range(0, range_8b_full);
    mu_assert("wrong 'full' 8b normalize range", almost_equal(n, 0.0));
    n = normalize_range(128, range_8b_limited);
    mu_assert("wrong 'limited' 8b normalize range", almost_equal(n, 0.5114155251141552));
    n = normalize_range(255, range_8b_limited);
    mu_assert("wrong 'limited' 8b normalize range", almost_equal(n, 1.0));

    n = normalize_range(65, range_10b_limited);
    mu_assert("wrong 'limited' 10b normalize range", almost_equal(n, 0.001141552511415525));
    n = normalize_range(512, range_10b_limited);
    mu_assert("wrong 'limited' 10b normalize range", almost_equal(n, 0.5114155251141552));
    n = normalize_range(939, range_10b_limited);
    mu_assert("wrong 'limited' 10b normalize range", almost_equal(n, 0.9988584474885844));

    return NULL;
}

/* Coverage push: vmaf_luminance_init_eotf accepts "bt1886" and "pq"
 * literally and otherwise returns -EINVAL.  The function was reachable
 * only via the float_ssim public path before; this drives it directly. */
static char *test_init_eotf_dispatch()
{
    VmafEOTF eotf = NULL;

    int err = vmaf_luminance_init_eotf(&eotf, "bt1886");
    mu_assert("init_eotf(bt1886) must succeed", err == 0);
    mu_assert("init_eotf(bt1886) must bind the BT.1886 function pointer",
              eotf == vmaf_luminance_bt1886_eotf);

    eotf = NULL;
    err = vmaf_luminance_init_eotf(&eotf, "pq");
    mu_assert("init_eotf(pq) must succeed", err == 0);
    mu_assert("init_eotf(pq) must bind the PQ function pointer", eotf == vmaf_luminance_pq_eotf);

    eotf = NULL;
    err = vmaf_luminance_init_eotf(&eotf, "hlg-nonexistent");
    mu_assert("init_eotf(unknown) must return -EINVAL", err == -EINVAL);
    mu_assert("init_eotf(unknown) must not write the out param", eotf == NULL);

    return NULL;
}

/* Coverage push: range_foot_head must reject unknown VmafPixelRange
 * enum values with -EINVAL.  Exercises the default branch that was
 * silently dead before. */
static char *test_range_foot_head_invalid()
{
    int foot = 0xDEAD;
    int head = 0xBEEF;

    /* Cast through an integer value the enum cannot legitimately hold. */
    int err = range_foot_head(8, (enum VmafPixelRange)0x7F, &foot, &head);
    mu_assert("range_foot_head(unknown) must return -EINVAL", err == -EINVAL);

    return NULL;
}

char *run_tests()
{
    mu_run_test(test_bt1886_eotf);
    mu_run_test(test_pq_eotf);
    mu_run_test(test_range_foot_head);
    mu_run_test(test_get_luminance);
    mu_run_test(test_normalize_range);
    mu_run_test(test_init_eotf_dispatch);
    mu_run_test(test_range_foot_head_invalid);

    return NULL;
}
