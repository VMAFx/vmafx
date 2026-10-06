/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * What provenance.c reads for a record and provenance_render.c writes
 * (#2142, ADR-2073, RC4 WP5). Not exported.
 */

#ifndef VMAFX_PROVENANCE_RECORD_H
#define VMAFX_PROVENANCE_RECORD_H

#include <stdbool.h>
#include <stdint.h>

#include "feature/feature_collector.h"
#include "internal.h"
#include "model.h"
#include "provenance_json.h"
#include "vmafx/vmafx.h"

/* Feature vectors one record lists (HISS-02); a run has tens. */
#define VMAFX_PROVENANCE_FEATURES_MAX 4096u

/* The device a context scores on (vmafx_provenance_device()). */
typedef struct VmafxProvenanceDevice {
    uint32_t backend; /* VmafxBackend */
    int32_t index;
    const char *name;
    const char *runtime;
} VmafxProvenanceDevice;

/* The context's collector, locked, with its features in name order. */
typedef struct VmafxProvenanceSnapshot {
    VmafFeatureCollector *fc;
    bool locked;
    const FeatureVector **features;
    uint32_t n_features;
    uint32_t n_models;
    VmafxProvenanceDevice device;
} VmafxProvenanceSnapshot;

/* Lock the context's collector and list it; end it on every path. */
VmafxStatus vmafx_provenance_snapshot_begin(const VmafxReport *report, VmafxContext *context,
                                            VmafxProvenanceSnapshot *snap);
void vmafx_provenance_snapshot_end(VmafxProvenanceSnapshot *snap);

void vmafx_provenance_device(const VmafxContext *context, VmafxProvenanceDevice *out);

/* The collector's feature vectors in byte order of their report names (at
 * most `max`); the caller holds the collector's lock. */
uint32_t vmafx_provenance_features(VmafFeatureCollector *fc, const FeatureVector **out,
                                   uint32_t max);
void vmafx_provenance_feature(const FeatureVector *fv, const VmafxProvenanceDevice *device,
                              VmafxFeatureProvenance *out);

/* `sha256:` and the SHA-256 of one line `<report name> <index> <16 hex digits
 * of the IEEE-754 bits>` per written score, features in the given order and
 * frames in index order. */
void vmafx_provenance_scores_digest(const FeatureVector *const *features, uint32_t n,
                                    char out[VMAFX_DIGEST_TEXT_SIZE]);

uint32_t vmafx_provenance_model_count(const VmafFeatureCollector *fc);
const VmafModel *vmafx_provenance_model(const VmafFeatureCollector *fc, uint32_t index);
void vmafx_provenance_model_record(const VmafModel *model, VmafxModelProvenance *out);

void vmafx_provenance_annotation(const VmafxProvenanceState *state, uint32_t index,
                                 VmafxAnnotation *out);

/* Every scalar field of the record but `digest`'s text (filled by the
 * render), with the scores digest of the snapshot. */
VmafxStatus vmafx_provenance_scalars(const VmafxReport *report, VmafxContext *context,
                                     const VmafxProvenanceSnapshot *snap, VmafxProvenance *rec);

/* The record of `context` as RFC 8785 JSON (provenance_render.c), with the
 * digest computed into the state; `flags` VmafxProvenanceJsonFlags; `rec`
 * (may be NULL) receives the scalar fields. Call with the state's lock held;
 * the caller frees `*json`. */
VmafxStatus vmafx_provenance_render(const VmafxReport *report, VmafxContext *context,
                                    uint32_t flags, char **json, VmafxProvenance *rec);

/* The backend receipt the JSON report carries next to the record (ADR-1359,
 * ADR-0498): `,\n  "feature_backends": [{"extractor": ..., "backend": ...},
 * ...],\n  "backend_used": ...`, `backend_used` being the backend of the
 * first extractor on a device, else "cpu". `backends` are VmafxBackend. */
void vmafx_backend_receipt(VmafxJsonText *text, const char *const *names, const uint32_t *backends,
                           unsigned n);

#endif /* VMAFX_PROVENANCE_RECORD_H */
