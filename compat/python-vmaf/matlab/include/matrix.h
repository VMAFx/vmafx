/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  Minimal repository-owned MATLAB matrix.h stubs for static analysis and linting (ADR-1322).
 */

#ifndef VMAF_MATLAB_MATRIX_H_
#define VMAF_MATLAB_MATRIX_H_

#include <stddef.h>

typedef struct mxArray_tag mxArray;

typedef enum { mxREAL = 0, mxCOMPLEX = 1 } mxComplexity;

double *mxGetPr(const mxArray *pm);
size_t mxGetM(const mxArray *pm);
size_t mxGetN(const mxArray *pm);
mxArray *mxCreateDoubleMatrix(int m, int n, mxComplexity ComplexityFlag);
int mxIsNumeric(const mxArray *pm);
int mxIsDouble(const mxArray *pm);
int mxIsSparse(const mxArray *pm);
int mxIsComplex(const mxArray *pm);
int mxIsChar(const mxArray *pm);
int mxGetString(const mxArray *pm, char *str, int strlen);
void *mxCalloc(size_t n, size_t size);
void mxFree(void *ptr);
void mxDestroyArray(mxArray *pm);
void mxFreeMatrix(mxArray *pm);

#endif /* VMAF_MATLAB_MATRIX_H_ */
