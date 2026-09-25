/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  Minimal repository-owned MATLAB mex.h stubs for static analysis and linting (ADR-1322).
 */

#ifndef VMAF_MATLAB_MEX_H_
#define VMAF_MATLAB_MEX_H_

#include <stdio.h>
#include <stdlib.h>
#include "matrix.h"

#if defined(__GNUC__) || defined(__clang__)
#define MEX_NORETURN __attribute__((__noreturn__))
#elif defined(_MSC_VER)
#define MEX_NORETURN __declspec(noreturn)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define MEX_NORETURN _Noreturn
#else
#define MEX_NORETURN
#endif

MEX_NORETURN void mexErrMsgTxt(const char *error_msg);
int mexPrintf(const char *format, ...);
void mexFunction(int nlhs, mxArray *plhs[], int nrhs, const mxArray *prhs[]);

#endif /* VMAF_MATLAB_MEX_H_ */
