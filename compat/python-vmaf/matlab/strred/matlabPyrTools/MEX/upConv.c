/* 
RES = upConv(IM, FILT, EDGES, STEP, START, STOP, RES);
  >>> See upConv.m for documentation <<<
  This is a matlab interface to the internal_expand function. 
  EPS, 7/96.
*/

#define V4_COMPAT
#include <matrix.h> /* Matlab matrices */
#include <mex.h>

#include <string.h>

#include "convolve.h"

#define notDblMtx(it) (!mxIsNumeric(it) || !mxIsDouble(it) || mxIsSparse(it) || mxIsComplex(it))

/* Every argument mexFunction() used to unpack into a dozen locals.  Grouping
   them lets each parsing step live in its own function and keeps mexFunction
   inside the 60-line complexity bound. */
typedef struct {
    double *image;
    double *filt;
    int x_idim, y_idim;
    int x_fdim, y_fdim;
    int x_start, x_step, x_stop;
    int y_start, y_step, y_stop;
    char edges[15];
} upconv_args;

/* ARG 4 (optional): STEP */
static void parse_step(const mxArray *arg, upconv_args *a)
{
    double *mxMat;

    if notDblMtx (arg)
        mexErrMsgTxt("STEP arg must be double float matrix.");
    if (mxGetM(arg) * mxGetN(arg) != 2)
        mexErrMsgTxt("STEP arg must contain two elements.");
    mxMat = mxGetPr(arg);
    a->x_step = (int)mxMat[0];
    a->y_step = (int)mxMat[1];
    if ((a->x_step < 1) || (a->y_step < 1))
        mexErrMsgTxt("STEP values must be greater than zero.");
}

/* ARG 5 (optional): START */
static void parse_start(const mxArray *arg, upconv_args *a)
{
    double *mxMat;

    if notDblMtx (arg)
        mexErrMsgTxt("START arg must be double float matrix.");
    if (mxGetM(arg) * mxGetN(arg) != 2)
        mexErrMsgTxt("START arg must contain two elements.");
    mxMat = mxGetPr(arg);
    a->x_start = (int)mxMat[0];
    a->y_start = (int)mxMat[1];
    if ((a->x_start < 1) || (a->y_start < 1))
        mexErrMsgTxt("START values must be greater than zero.");
}

/* ARG 6 (optional): STOP */
static void parse_stop(const mxArray *arg, upconv_args *a)
{
    double *mxMat;

    if notDblMtx (arg)
        mexErrMsgTxt("STOP arg must be double float matrix.");
    if (mxGetM(arg) * mxGetN(arg) != 2)
        mexErrMsgTxt("STOP arg must contain two elements.");
    mxMat = mxGetPr(arg);
    a->x_stop = (int)mxMat[0];
    a->y_stop = (int)mxMat[1];
    if ((a->x_stop < a->x_start) || (a->y_stop < a->y_start))
        mexErrMsgTxt("STOP values must be greater than START values.");
}

static void parse_args(int nrhs, const mxArray *prhs[], upconv_args *a)
{
    const mxArray *arg;

    a->x_start = 1;
    a->x_step = 1;
    a->y_start = 1;
    a->y_step = 1;
    /* Bounded copy of the default edge-handler name (HISS-08: strcpy is banned). */
    memset(a->edges, 0, sizeof(a->edges));
    strncpy(a->edges, "reflect1", sizeof(a->edges) - 1);

    if (nrhs < 2)
        mexErrMsgTxt("requres at least 2 args.");

    /* ARG 1: IMAGE  */
    arg = prhs[0];
    if notDblMtx (arg)
        mexErrMsgTxt("IMAGE arg must be a non-sparse double float matrix.");
    a->image = mxGetPr(arg);
    a->x_idim = (int)mxGetM(arg); /* X is inner index! */
    a->y_idim = (int)mxGetN(arg);

    /* ARG 2: FILTER */
    arg = prhs[1];
    if notDblMtx (arg)
        mexErrMsgTxt("FILTER arg must be non-sparse double float matrix.");
    a->filt = mxGetPr(arg);
    a->x_fdim = (int)mxGetM(arg);
    a->y_fdim = (int)mxGetN(arg);

    /* ARG 3 (optional): EDGES */
    if (nrhs > 2) {
        if (!mxIsChar(prhs[2]))
            mexErrMsgTxt("EDGES arg must be a string.");
        mxGetString(prhs[2], a->edges, 15);
    }

    if (nrhs > 3)
        parse_step(prhs[3], a);

    if (nrhs > 4)
        parse_start(prhs[4], a);

    a->x_start--; /* convert to standard C indexes */
    a->y_start--;

    if (nrhs > 5) {
        parse_stop(prhs[5], a);
    } else { /* default: make res dims a multiple of STEP size */
        a->x_stop = a->x_step * ((a->x_start / a->x_step) + a->x_idim);
        a->y_stop = a->y_step * ((a->y_start / a->y_step) + a->y_idim);
    }
}

/* ARG 7 (optional): RESULT image.  Returns the result buffer and its dims. */
static double *resolve_result(int nrhs, const mxArray *prhs[], mxArray *plhs[],
                              const upconv_args *a, int *x_rdim, int *y_rdim)
{
    const mxArray *arg;
    double *result;

    if (nrhs > 6) {
        arg = prhs[6];
        if notDblMtx (arg)
            mexErrMsgTxt("RES arg must be double float matrix.");

        /* 7/10/97: Returning one of the args causes problems with Matlab's memory 
	 manager, so we don't return anything if the result image is passed */
        /*  plhs[0] = arg;  */
        result = mxGetPr(arg);
        *x_rdim = (int)mxGetM(arg); /* X is inner index! */
        *y_rdim = (int)mxGetN(arg);
        if ((a->x_stop > *x_rdim) || (a->y_stop > *y_rdim))
            mexErrMsgTxt("STOP values must within image dimensions.");
        return result;
    }

    *x_rdim = a->x_stop;
    *y_rdim = a->y_stop;
    /*  x_rdim = x_step * ((x_stop+x_step-1)/x_step);
      y_rdim = y_step * ((y_stop+y_step-1)/y_step);  */

    plhs[0] = (mxArray *)mxCreateDoubleMatrix(*x_rdim, *y_rdim, mxREAL);
    if (plhs[0] == NULL)
        mexErrMsgTxt("Cannot allocate result matrix");
    return mxGetPr(plhs[0]);
}

/* upConv has a bug for even-length kernels when using the 
   reflect1, extend, or repeat edge-handlers.  Embed such a filter in the
   upper-left corner of an odd-sized one; returns the original x dimension
   when a padded copy was allocated, and 0 when the filter was left alone. */
static int pad_even_filter(upconv_args *a)
{
    double *orig_filt;
    int orig_x, orig_y, x, y;

    if (!((!strcmp(a->edges, "reflect1") || !strcmp(a->edges, "extend") ||
           !strcmp(a->edges, "repeat")) &&
          ((a->x_fdim % 2 == 0) || (a->y_fdim % 2 == 0))))
        return 0;

    orig_filt = a->filt;
    orig_x = a->x_fdim;
    orig_y = a->y_fdim;
    a->x_fdim = 2 * (orig_x / 2) + 1;
    a->y_fdim = 2 * (orig_y / 2) + 1;
    a->filt = mxCalloc(a->x_fdim * a->y_fdim, sizeof(double));
    if (a->filt == NULL)
        mexErrMsgTxt("Cannot allocate necessary temporary space");
    for (y = 0; y < orig_y; y++)
        for (x = 0; x < orig_x; x++)
            a->filt[y * a->x_fdim + x] = orig_filt[y * orig_x + x];
    return orig_x;
}

void mexFunction(int nlhs,             /* Num return vals on lhs */
                 mxArray *plhs[],      /* Matrices on lhs      */
                 int nrhs,             /* Num args on rhs    */
                 const mxArray *prhs[] /* Matrices on rhs */
)
{
    upconv_args a;
    double *temp, *result;
    int x_rdim, y_rdim, orig_x;

    parse_args(nrhs, prhs, &a);

    result = resolve_result(nrhs, prhs, plhs, &a, &x_rdim, &y_rdim);

    if ((((a.x_stop - a.x_start + a.x_step - 1) / a.x_step) != a.x_idim) ||
        (((a.y_stop - a.y_start + a.y_step - 1) / a.y_step) != a.y_idim)) {
        mexPrintf("Im dims: [%d %d]\n", a.x_idim, a.y_idim);
        mexPrintf("Start:   [%d %d]\n", a.x_start, a.y_start);
        mexPrintf("Step:    [%d %d]\n", a.x_step, a.y_step);
        mexPrintf("Stop:    [%d %d]\n", a.x_stop, a.y_stop);
        mexPrintf("Res dims: [%d %d]\n", x_rdim, y_rdim);
        mexErrMsgTxt("Image sizes and upsampling args are incompatible!");
    }

    orig_x = pad_even_filter(&a);

    if ((a.x_fdim > x_rdim) || (a.y_fdim > y_rdim)) {
        mexPrintf("Filter: [%d %d], ", a.x_fdim, a.y_fdim);
        mexPrintf("Result: [%d %d]\n", x_rdim, y_rdim);
        mexErrMsgTxt("FILTER dimensions larger than RESULT dimensions.");
    }

    temp = mxCalloc(a.x_fdim * a.y_fdim, sizeof(double));
    if (temp == NULL)
        mexErrMsgTxt("Cannot allocate necessary temporary space");

    if (strcmp(a.edges, "circular") == 0)
        internal_wrap_expand(a.image, a.filt, a.x_fdim, a.y_fdim, a.x_start, a.x_step, a.x_stop,
                             a.y_start, a.y_step, a.y_stop, result, x_rdim, y_rdim);
    else
        internal_expand(a.image, a.filt, temp, a.x_fdim, a.y_fdim, a.x_start, a.x_step, a.x_stop,
                        a.y_start, a.y_step, a.y_stop, result, x_rdim, y_rdim, a.edges);

    if (orig_x)
        mxFree((char *)a.filt);
    mxFree((char *)temp);

    return;
}
