/* SPDX-License-Identifier: EUPL-1.2 */
/*
 * Copyright 2026 Lusoris
 *
 * Lint-only stand-in for MATLAB's <matrix.h>: the types and functions the ten MEX sources under
 * compat/python-vmaf/matlab/ use, and nothing else. The real header ships with MATLAB and exists
 * on no runner or image, so clang-tidy could not parse those sources (T-TIDY-MATLAB-MEX-
 * UNMEASURED-2026-09-22, decision Q-016). Self-authored from the documented MATLAB C API
 * signatures; it carries no MathWorks text.
 *
 * Used only on the include path scripts/ci/gen-mex-compile-commands.py writes into the lint
 * compilation database. No meson target, no build and no link ever sees it: a MEX file built
 * against it would not run.
 */

#ifndef VMAF_LINT_STUB_MATLAB_MATRIX_H_
#define VMAF_LINT_STUB_MATLAB_MATRIX_H_

#include <stddef.h>

typedef struct mxArray_tag mxArray;
typedef size_t mwSize;
typedef size_t mwIndex;

typedef enum {
    mxREAL,
    mxCOMPLEX,
} mxComplexity;

mxArray *mxCreateDoubleMatrix(mwSize m, mwSize n, mxComplexity complexity);
void mxDestroyArray(mxArray *array);
double *mxGetPr(const mxArray *array);
mwSize mxGetM(const mxArray *array);
mwSize mxGetN(const mxArray *array);
int mxGetString(const mxArray *array, char *buffer, mwSize length);
int mxIsChar(const mxArray *array);
int mxIsComplex(const mxArray *array);
int mxIsDouble(const mxArray *array);
int mxIsNumeric(const mxArray *array);
int mxIsSparse(const mxArray *array);
void *mxCalloc(size_t count, size_t size);
void mxFree(void *pointer);

#endif /* VMAF_LINT_STUB_MATLAB_MATRIX_H_ */
