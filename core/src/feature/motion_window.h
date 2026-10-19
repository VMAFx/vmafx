/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The temporal window of the integer motion extractors (ADR-1478, ADR-2090).
 *
 * Every integer motion extractor stores one SAD score per frame and derives
 * motion2 and motion3 of each frame from those scores. The derivation is
 * integer_motion.c::flush() (Netflix a4a1492d), with the three-frame window
 * or, with motion_five_frame_window, the five-frame one. It is defined once,
 * in integer_motion.c, and called by the CPU extractors `motion` and
 * `motion_v2` and by the GPU twins that derive through it, so a twin's scores
 * are the CPU's whenever its SAD scores are.
 *
 * Frame i is derived as soon as its window is complete (ADR-2090): once the
 * SAD scores of frames 0 to max(i + 1, min_idx) are in the collector, where
 * min_idx is 1, or 2 with the five-frame window. vmaf_motion_window_advance()
 * derives those frames; vmaf_motion_window_flush() derives the rest at the end
 * of the stream, the last frame without a later one. Both run the statements
 * of upstream's flush() frame by frame in index order and carry the
 * moving-average state across calls in a VmafMotionWindowState, so every value
 * is the one a single flush over the whole stream computes.
 */

#ifndef FEATURE_MOTION_WINDOW_H_
#define FEATURE_MOTION_WINDOW_H_

#include <stdbool.h>

#include "dict.h"
#include "feature_collector.h"

#ifdef __cplusplus
extern "C" {
#endif

/* NOLINTBEGIN(modernize-use-using): C header included by C and C++ translation units. ADR-1138. */

/* Where one extractor's derivation stands: the SAD scores of frames
 * 0 .. n_sad - 1 are in the collector, motion2 and motion3 of frames
 * 0 .. next - 1 are appended. stamp_value and prev_processed are the two
 * values upstream's flush() carries from frame to frame. Zero-initialised in
 * the extractor's private data; one per extractor context, touched only by the
 * thread that feeds frames (ADR-2090). */
typedef struct VmafMotionWindowState {
    unsigned n_sad;
    unsigned next;
    double stamp_value;
    double prev_processed;
} VmafMotionWindowState;

/* What one extractor's derivation needs: the provided-feature keys it writes
 * (the collector names are looked up in its feature-name dictionary, so
 * option suffixes are applied), the options that shape the window, and its
 * state. A NULL state derives every frame in one flush, as before ADR-2090. */
typedef struct VmafMotionWindow {
    const char *sad_feature;     /* per-frame SAD score, read */
    const char *motion2_feature; /* written for every frame */
    const char *motion3_feature; /* written for every frame */
    double motion_blend_factor;
    double motion_blend_offset;
    double motion_max_val;
    bool motion_five_frame_window;
    bool motion_moving_average;
    VmafMotionWindowState *state;
} VmafMotionWindow;

/* NOLINTEND(modernize-use-using) */

/*
 * Append motion2 and motion3 for every frame whose window is complete: the
 * frames i not yet derived with SAD scores of frames 0 .. max(i + 1, min_idx)
 * in the collector. Requires `window->state`.
 *
 * Returns 0, -EINVAL when the dictionary lacks `sad_feature` or the state is
 * NULL, or the error of the first append that fails.
 */
int vmaf_motion_window_advance(VmafFeatureCollector *feature_collector,
                               VmafDictionary *feature_name_dict, const VmafMotionWindow *window);

/*
 * Append motion2 and motion3 for every frame that has a SAD score and is not
 * derived yet; the last frame has no later frame, and its motion2 is its own
 * SAD score.
 *
 * The SAD scores are those the extractor appended under `sad_feature`:
 * weighted by motion_fps_weight and capped at motion_max_val, 0 for the
 * frames without an earlier frame to difference against. A collector without
 * any SAD score gets nothing appended.
 *
 * Returns 0, -EINVAL when the dictionary lacks `sad_feature`, or the error of
 * the first append that fails.
 */
int vmaf_motion_window_flush(VmafFeatureCollector *feature_collector,
                             VmafDictionary *feature_name_dict, const VmafMotionWindow *window);

#ifdef __cplusplus
}
#endif

#endif /* FEATURE_MOTION_WINDOW_H_ */
