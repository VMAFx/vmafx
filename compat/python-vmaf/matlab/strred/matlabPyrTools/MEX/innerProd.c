/* 
RES = innerProd(MAT);
  Computes mat'*mat  
  Odelia Schwartz, 8/97.
*/

#define V4_COMPAT
#include <matrix.h>

#include <stdio.h>
#include <ctype.h>
#include <math.h>
#include <strings.h>
#include <stdlib.h>

void mexFunction(int nlhs,             /* Num return vals on lhs */
                 mxArray *plhs[],      /* Matrices on lhs      */
                 int nrhs,             /* Num args on rhs    */
                 const mxArray *prhs[] /* Matrices on rhs */
)
{
    double *res;
    double *mat;
    double tmp;
    size_t len;
    size_t wid;
    size_t i;
    size_t k;
    size_t j;
    size_t jlen;
    size_t ilen;
    size_t imat;
    size_t jmat;
    const mxArray *arg;

    (void)nlhs;
    (void)nrhs;

    /* get matrix input argument */
    /* should be matrix in which num rows >= num columns */
    arg = prhs[0];
    mat = mxGetPr(arg);
    len = mxGetM(arg);
    wid = mxGetN(arg);
    if (wid > len) {
        (void)printf("innerProd: Warning: width %zu is greater than length %zu.\n", wid, len);
    }
    plhs[0] = mxCreateDoubleMatrix(wid, wid, mxREAL);
    if (plhs[0] == NULL) {
        char message[80];
        const int written =
            snprintf(message, sizeof(message), "Error allocating %zux%zu result matrix", wid, wid);
        if (written < 0 || (size_t)written >= sizeof(message)) {
            mexErrMsgTxt("Error allocating result matrix");
        }
        mexErrMsgTxt(message);
    }
    res = mxGetPr(plhs[0]);

    for (i = 0, ilen = 0; i < wid; i++, ilen += len) {
        for (j = i, jlen = ilen; j < wid; j++, jlen += len) {
            tmp = 0.0;
            for (k = 0, imat = ilen, jmat = jlen; k < len; k++, imat++, jmat++) {
                tmp += mat[imat] * mat[jmat];
            }
            res[i * wid + j] = tmp;
            res[j * wid + i] = tmp;
        }
    }
}
