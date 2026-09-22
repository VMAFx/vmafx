/* 
RES = corrDn(IM, FILT, EDGES, STEP, START, STOP);
  >>> See corrDn.m for documentation <<<
  This is a matlab interface to the internal_reduce function. 
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
} corrdn_args;

/* ARG 1: IMAGE, ARG 2: FILTER */
static void parse_image_and_filter(const mxArray *prhs[], corrdn_args *a)
{
    const mxArray *arg;

    arg = prhs[0];
    if notDblMtx (arg)
        mexErrMsgTxt("IMAGE arg must be a non-sparse double float matrix.");
    a->image = mxGetPr(arg);
    a->x_idim = (int)mxGetM(arg); /* X is inner index! */
    a->y_idim = (int)mxGetN(arg);

    arg = prhs[1];
    if notDblMtx (arg)
        mexErrMsgTxt("FILTER arg must be non-sparse double float matrix.");
    a->filt = mxGetPr(arg);
    a->x_fdim = (int)mxGetM(arg);
    a->y_fdim = (int)mxGetN(arg);

    if ((a->x_fdim > a->x_idim) || (a->y_fdim > a->y_idim)) {
        mexPrintf("Filter: [%d %d], Image: [%d %d]\n", a->x_fdim, a->y_fdim, a->x_idim, a->y_idim);
        mexErrMsgTxt("FILTER dimensions larger than IMAGE dimensions.");
    }
}

/* ARG 4 (optional): STEP */
static void parse_step(const mxArray *arg, corrdn_args *a)
{
    double *mxMat;

    if notDblMtx (arg)
        mexErrMsgTxt("STEP arg must be a double float matrix.");
    if (mxGetM(arg) * mxGetN(arg) != 2)
        mexErrMsgTxt("STEP arg must contain two elements.");
    mxMat = mxGetPr(arg);
    a->x_step = (int)mxMat[0];
    a->y_step = (int)mxMat[1];
    if ((a->x_step < 1) || (a->y_step < 1))
        mexErrMsgTxt("STEP values must be greater than zero.");
}

/* ARG 5 (optional): START */
static void parse_start(const mxArray *arg, corrdn_args *a)
{
    double *mxMat;

    if notDblMtx (arg)
        mexErrMsgTxt("START arg must be a double float matrix.");
    if (mxGetM(arg) * mxGetN(arg) != 2)
        mexErrMsgTxt("START arg must contain two elements.");
    mxMat = mxGetPr(arg);
    a->x_start = (int)mxMat[0];
    a->y_start = (int)mxMat[1];
    if ((a->x_start < 1) || (a->x_start > a->x_idim) || (a->y_start < 1) ||
        (a->y_start > a->y_idim))
        mexErrMsgTxt("START values must lie between 1 and the image dimensions.");
}

/* ARG 6 (optional): STOP */
static void parse_stop(const mxArray *arg, corrdn_args *a)
{
    double *mxMat;

    if notDblMtx (arg)
        mexErrMsgTxt("STOP arg must be double float matrix.");
    if (mxGetM(arg) * mxGetN(arg) != 2)
        mexErrMsgTxt("STOP arg must contain two elements.");
    mxMat = mxGetPr(arg);
    a->x_stop = (int)mxMat[0];
    a->y_stop = (int)mxMat[1];
    if ((a->x_stop < a->x_start) || (a->x_stop > a->x_idim) || (a->y_stop < a->y_start) ||
        (a->y_stop > a->y_idim))
        mexErrMsgTxt("STOP values must lie between START and the image dimensions.");
}

static void parse_args(int nrhs, const mxArray *prhs[], corrdn_args *a)
{
    a->x_start = 1;
    a->x_step = 1;
    a->y_start = 1;
    a->y_step = 1;
    /* Bounded copy of the default edge-handler name (HISS-08: strcpy is banned). */
    memset(a->edges, 0, sizeof(a->edges));
    strncpy(a->edges, "reflect1", sizeof(a->edges) - 1);

    if (nrhs < 2)
        mexErrMsgTxt("requres at least 2 args.");

    parse_image_and_filter(prhs, a);

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

    a->x_start--; /* convert from Matlab to standard C indexes */
    a->y_start--;

    if (nrhs > 5) {
        parse_stop(prhs[5], a);
    } else {
        a->x_stop = a->x_idim;
        a->y_stop = a->y_idim;
    }
}

void mexFunction(int nlhs,             /* Num return vals on lhs */
                 mxArray *plhs[],      /* Matrices on lhs      */
                 int nrhs,             /* Num args on rhs    */
                 const mxArray *prhs[] /* Matrices on rhs */
)
{
    corrdn_args a;
    double *temp, *result;
    int x_rdim, y_rdim;

    parse_args(nrhs, prhs, &a);

    x_rdim = (a.x_stop - a.x_start + a.x_step - 1) / a.x_step;
    y_rdim = (a.y_stop - a.y_start + a.y_step - 1) / a.y_step;

    /*  mxFreeMatrix(plhs[0]); */
    plhs[0] = (mxArray *)mxCreateDoubleMatrix(x_rdim, y_rdim, mxREAL);
    if (plhs[0] == NULL)
        mexErrMsgTxt("Cannot allocate result matrix");
    result = mxGetPr(plhs[0]);

    temp = mxCalloc(a.x_fdim * a.y_fdim, sizeof(double));
    if (temp == NULL)
        mexErrMsgTxt("Cannot allocate necessary temporary space");

    /* Edited by Rajiv on April 10,2010
  if (strcmp(edges,"circular") == 0)
  	internal_wrap_reduce(image, x_idim, y_idim, filt, x_fdim, y_fdim,
			     x_start, x_step, x_stop, y_start, y_step, y_stop,
			     result);
  else internal_reduce(image, x_idim, y_idim, filt, temp, x_fdim, y_fdim,
		       x_start, x_step, x_stop, y_start, y_step, y_stop,
		       result, edges);*/

    internal_reduce(a.image, a.x_idim, a.y_idim, a.filt, temp, a.x_fdim, a.y_fdim, a.x_start,
                    a.x_step, a.x_stop, a.y_start, a.y_step, a.y_stop, result, a.edges);
    /* End edit */

    mxFree((char *)temp);
    return;
}
