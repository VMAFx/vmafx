/* 
RES = pointOp(IM, LUT, ORIGIN, INCREMENT, WARNINGS)
  >>> See pointOp.m for documentation <<<
  EPS, ported from OBVIUS, 7/96.
*/

#define V4_COMPAT
#include <matrix.h> /* Matlab matrices */
#include <mex.h>

#include <stddef.h> /* NULL */

#define notDblMtx(it) (!mxIsNumeric(it) || !mxIsDouble(it) || mxIsSparse(it) || mxIsComplex(it))

void internal_pointop();

/* Read a mandatory-or-optional real scalar argument.  @p name is used in the
   two diagnostics, which keep the exact wording the inline checks had. */
static double read_scalar_arg(const mxArray *arg, const char *type_msg, const char *size_msg)
{
    double *mxMat;

    if notDblMtx (arg)
        mexErrMsgTxt(type_msg);
    if (mxGetM(arg) * mxGetN(arg) != 1)
        mexErrMsgTxt(size_msg);
    mxMat = mxGetPr(arg);
    return *mxMat;
}

void mexFunction(int nlhs,             /* Num return vals on lhs */
                 mxArray *plhs[],      /* Matrices on lhs      */
                 int nrhs,             /* Num args on rhs    */
                 const mxArray *prhs[] /* Matrices on rhs */
)
{
    double *image, *lut, *res;
    double origin, increment;
    int x_dim, y_dim, lx_dim, ly_dim;
    int warnings = 1;
    const mxArray *arg;

    if (nrhs < 4)
        mexErrMsgTxt("requres  at least 4 args.");

    /* ARG 1: IMAGE  */
    arg = prhs[0];
    if notDblMtx (arg)
        mexErrMsgTxt("IMAGE arg must be a real non-sparse matrix.");
    image = mxGetPr(arg);
    x_dim = (int)mxGetM(arg); /* X is inner index! */
    y_dim = (int)mxGetN(arg);

    /* ARG 2: Lookup table */
    arg = prhs[1];
    if notDblMtx (arg)
        mexErrMsgTxt("LUT arg must be a real non-sparse matrix.");
    lut = mxGetPr(arg);
    lx_dim = (int)mxGetM(arg); /* X is inner index! */
    ly_dim = (int)mxGetN(arg);
    if ((lx_dim != 1) && (ly_dim != 1))
        mexErrMsgTxt("Lookup table must be a row or column vector.");

    /* ARG 3: ORIGIN */
    origin = read_scalar_arg(prhs[2], "ORIGIN arg must be a real scalar.",
                             "ORIGIN arg must be a real scalar.");

    /* ARG 4: INCREMENT */
    increment = read_scalar_arg(prhs[3], "INCREMENT arg must be a real scalar.",
                                "INCREMENT arg must be a real scalar.");

    /* ARG 5: WARNINGS */
    if (nrhs > 4)
        warnings = (int)read_scalar_arg(prhs[4], "WARINGS arg must be a real scalar.",
                                        "WARNINGS arg must be a real scalar.");

    plhs[0] = (mxArray *)mxCreateDoubleMatrix(x_dim, y_dim, mxREAL);
    if (plhs[0] == NULL)
        mexErrMsgTxt("Cannot allocate result matrix");
    res = mxGetPr(plhs[0]);

    internal_pointop(image, res, x_dim * y_dim, lut, lx_dim * ly_dim, origin, increment, warnings);
    return;
}

/* Use linear interpolation on a lookup table.
   Taken from OBVIUS.  EPS, Spring, 1987.
 */
void internal_pointop(im, res, size, lut, lutsize, origin, increment, warnings) register double *im,
    *res, *lut;
register double origin, increment;
register int size, lutsize, warnings;
{
    register int i, index;
    register double pos;
    register int l_unwarned = warnings;
    register int r_unwarned = warnings;

    lutsize = lutsize - 2; /* Maximum index value */
    if (increment > 0)
        for (i = 0; i < size; i++) {
            pos = (im[i] - origin) / increment;
            index = (int)pos; /* Floor */
            if (index < 0) {
                index = 0;
                if (l_unwarned) {
                    mexPrintf("Warning: Extrapolating to left of lookup table...\n");
                    l_unwarned = 0;
                }
            } else if (index > lutsize) {
                index = lutsize;
                if (r_unwarned) {
                    mexPrintf("Warning: Extrapolating to right of lookup table...\n");
                    r_unwarned = 0;
                }
            }
            res[i] = lut[index] + (lut[index + 1] - lut[index]) * (pos - index);
        }
    else
        for (i = 0; i < size; i++)
            res[i] = *lut;
}
