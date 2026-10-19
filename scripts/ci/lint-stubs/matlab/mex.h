/* SPDX-License-Identifier: EUPL-1.2 */
/*
 * Copyright 2026 Lusoris
 *
 * Lint-only stand-in for MATLAB's <mex.h>; see matrix.h in this directory for why it exists and
 * where it is used. Self-authored; it carries no MathWorks text.
 */

#ifndef VMAF_LINT_STUB_MATLAB_MEX_H_
#define VMAF_LINT_STUB_MATLAB_MEX_H_

#include "matrix.h"

void mexFunction(int nlhs, mxArray *plhs[], int nrhs, const mxArray *prhs[]);
_Noreturn void mexErrMsgTxt(const char *message);
int mexPrintf(const char *format, ...);

#endif /* VMAF_LINT_STUB_MATLAB_MEX_H_ */
