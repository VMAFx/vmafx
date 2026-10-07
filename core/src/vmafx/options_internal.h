/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/* VmafxOptions inside the library (ADR-1852, RC4 WP2). */

#ifndef VMAFX_OPTIONS_INTERNAL_H
#define VMAFX_OPTIONS_INTERNAL_H

#include "internal.h"
#include "libvmaf/feature.h"
#include "vmafx/vmafx.h"

/* The libvmaf feature dictionary an option set is (the same object). */
VmafFeatureDictionary **vmafx_options_dictionary(VmafxOptions **options);

/* A private copy of `options` (NULL: none) for an engine call that consumes
 * its dictionary. On failure `*copy` is NULL and the failure is reported. */
VmafxStatus vmafx_options_copy(const VmafxReport *report, const VmafxOptions *options,
                               VmafFeatureDictionary **copy);

#endif /* VMAFX_OPTIONS_INTERNAL_H */
