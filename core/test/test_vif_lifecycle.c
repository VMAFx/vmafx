/**
 *
 *  Copyright 2026 Lusoris
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

/* NOLINTBEGIN(modernize-use-nullptr) -- ADR-1138: preserve Netflix NULL; MSVC C nullptr support is unverified. */
#include <limits.h>
#include <stddef.h>

#include "test.h"
#include "feature/vif.h"

static char *test_differencing_stride(void)
{
    const float current[] = {8, 4, 7, 99, 2, 9, 3, 99};
    const float previous[] = {3, 6, 5, 88, 8, 1, 3, 88};
    float difference[] = {0, 0, 0, 77, 0, 0, 0, 77};
    const float expected[] = {5, -2, 2, 77, -6, 8, 0, 77};
    apply_frame_differencing(current, previous, difference, 3, 2, 4);
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i)
        mu_assert("odd-width differencing preserves row padding", difference[i] == expected[i]);
    return NULL;
}

typedef struct {
    unsigned calls;
    int status;
    unsigned frames;
} ReaderState;

static int read_terminal_frame(float *ref, float *dis, float *tmp, int stride, void *opaque)
{
    ReaderState *state = opaque;
    state->calls++;
    if (state->calls > state->frames)
        return state->status;
    for (size_t row = 0; row < 49; ++row) {
        for (size_t col = 0; col < 65; ++col) {
            const size_t index = row * (stride / sizeof(float)) + col;
            tmp[index] = (float)((row + col + state->calls) % 256);
            ref[index] = tmp[index];
            dis[index] = tmp[index];
        }
    }
    return 0;
}

static char *test_temporal_terminal_paths(void)
{
    ReaderState reader = {.status = 2, .frames = 2};
    int ret = vifdiff(read_terminal_frame, &reader, 65, 49, "yuv420p");
    mu_assert("EOF succeeds", ret == 0);
    mu_assert("two frames followed by EOF", reader.calls == 3);
    reader = (ReaderState){.status = 1};
    ret = vifdiff(read_terminal_frame, &reader, 65, 49, "yuv420p");
    mu_assert("read failure propagates", ret == 1);
    mu_assert("failing reader called once", reader.calls == 1);
    reader.calls = 0;
    ret = vifdiff(read_terminal_frame, &reader, 0, 49, "yuv420p");
    mu_assert("invalid geometry rejected", ret == 1);
    mu_assert("invalid geometry does not call reader", reader.calls == 0);
    return NULL;
}

static char *test_invalid_geometry(void)
{
    static const int dimensions[][2] = {{0, 49},  {65, 0},      {-1, 49},
                                        {65, -1}, {INT_MAX, 1}, {INT_MAX, INT_MAX}};
    for (size_t i = 0; i < sizeof(dimensions) / sizeof(dimensions[0]); ++i) {
        double score = 7.0;
        double numerator = 8.0;
        double denominator = 9.0;
        double scales[8] = {0};
        const int ret =
            compute_vif(NULL, NULL, dimensions[i][0], dimensions[i][1], 0, 0, &score, &numerator,
                        &denominator, scales, 100.0, 1.0, 0, 2.0, NULL, NULL);
        mu_assert("invalid VIF geometry rejected before input access", ret == 1);
        mu_assert("invalid VIF geometry preserves caller scores",
                  score == 7.0 && numerator == 8.0 && denominator == 9.0);
        ReaderState reader = {.status = 2};
        const int temporal =
            vifdiff(read_terminal_frame, &reader, dimensions[i][0], dimensions[i][1], "yuv420p");
        mu_assert("invalid temporal geometry rejected", temporal == 1);
        mu_assert("invalid temporal geometry never reads", reader.calls == 0);
    }
    /* This aligned plane would overflow the ten-plane slab on either data model.
     * Rejection must occur before dereferencing the null inputs or allocating. */
    double score = 7.0;
    double numerator = 8.0;
    double denominator = 9.0;
    double scales[8] = {0};
    const int ret = compute_vif(NULL, NULL, (INT_MAX - 31) / 4, INT_MAX, 0, 0, &score, &numerator,
                                &denominator, scales, 100.0, 1.0, 0, 2.0, NULL, NULL);
    mu_assert("ten-plane allocation overflow rejected", ret == 1);
    mu_assert("overflow does not update scores", score == 7.0);

    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_differencing_stride);
    mu_run_test(test_temporal_terminal_paths);
    mu_run_test(test_invalid_geometry);
    return NULL;
}
/* NOLINTEND(modernize-use-nullptr) */
