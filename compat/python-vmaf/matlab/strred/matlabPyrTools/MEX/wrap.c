/* 
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;  File: wrap.c
;;;  Author: Eero Simoncelli
;;;  Description: Circular convolution on 2D images.
;;;  Creation Date: Spring, 1987.
;;;  MODIFICATIONS:
;;;      6/96: Switched array types to double float.
;;;      2/97: made more robust and readable.  Added STOP arguments.
;;;  ----------------------------------------------------------------
;;;    Object-Based Vision and Image Understanding System (OBVIUS),
;;;      Copyright 1988, Vision Science Group,  Media Laboratory,  
;;;              Massachusetts Institute of Technology.
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
*/

#include <stdlib.h>

#include "convolve.h"

/*
 --------------------------------------------------------------------
 Performs correlation (i.e., convolution with filt(-x,-y)) of FILT
 with IMAGE followed by subsampling (a.k.a. REDUCE in Burt&Adelson81).
 The operations are combined to avoid unnecessary computation of the
 convolution samples that are to be discarded in the subsampling
 operation.  The convolution is done in 9 sections so that mod
 operations are not performed unnecessarily.  The subsampling lattice
 is specified by the START, STEP and STOP parameters.
 -------------------------------------------------------------------- */

/* Shared state of one internal_wrap_reduce() call.  The three row bands used
   to be inline sections reading a dozen `register` locals; passing them by
   struct keeps every band inside the 60-line complexity bound. */
typedef struct {
    image_type **imval;
    image_type *filt;
    image_type *result;
    int x_dim;
    int y_dim;
    int x_fdim;
    int filt_size;
    int x_step;
    int y_step;
    int x_start;
    int x_stop;
    int x_ctr_start;
    int x_ctr_stop;
} wrap_reduce_state;

/* abstract out the inner product computation (was the INPROD macro).  The
   two wrap flags pick the same index expressions the macro arguments did:
   wrapped rows/columns take a modulo, unwrapped ones do not. */
static void wrap_inprod(const wrap_reduce_state *s, int y_startv, int y_wrap, int x_startv,
                        int x_wrap, int res_pos)
{
    double sum = 0.0;
    int filt_pos, x_im, y_im, x_filt_stop;
    int row, col;

    for (y_im = y_startv, filt_pos = 0, x_filt_stop = s->x_fdim; x_filt_stop <= s->filt_size;
         y_im++, x_filt_stop += s->x_fdim)
        for (x_im = x_startv; filt_pos < x_filt_stop; filt_pos++, x_im++) {
            row = y_wrap ? (y_im % s->y_dim) : y_im;
            col = x_wrap ? (x_im % s->x_dim) : x_im;
            sum += s->imval[row][col] * s->filt[filt_pos];
        }
    s->result[res_pos] = sum;
}

/* TOP ROWS: rows wrap, the centre column band does not. */
static int wrap_reduce_top(const wrap_reduce_state *s, int y_start, int y_ctr_start, int *res_pos)
{
    int x_pos, y_pos;

    for (y_pos = y_start; y_pos < y_ctr_start; y_pos += s->y_step) {
        for (x_pos = s->x_start; x_pos < s->x_ctr_start; x_pos += s->x_step, (*res_pos)++)
            wrap_inprod(s, y_pos + s->y_dim, 1, x_pos + s->x_dim, 1, *res_pos);

        for (; x_pos < s->x_ctr_stop; x_pos += s->x_step, (*res_pos)++)
            wrap_inprod(s, y_pos + s->y_dim, 1, x_pos, 0, *res_pos);

        for (; x_pos < s->x_stop; x_pos += s->x_step, (*res_pos)++)
            wrap_inprod(s, y_pos + s->y_dim, 1, x_pos, 1, *res_pos);
    }
    return y_pos;
}

/* MID ROWS: rows do not wrap; only the left and right column bands do. */
static int wrap_reduce_mid(const wrap_reduce_state *s, int y_pos, int y_ctr_stop, int *res_pos)
{
    int x_pos;

    for (; y_pos < y_ctr_stop; y_pos += s->y_step) {
        for (x_pos = s->x_start; x_pos < s->x_ctr_start; x_pos += s->x_step, (*res_pos)++)
            wrap_inprod(s, y_pos, 0, x_pos + s->x_dim, 1, *res_pos);

        for (; /* CENTER SECTION */
             x_pos < s->x_ctr_stop; x_pos += s->x_step, (*res_pos)++)
            wrap_inprod(s, y_pos, 0, x_pos, 0, *res_pos);

        for (; x_pos < s->x_stop; x_pos += s->x_step, (*res_pos)++)
            wrap_inprod(s, y_pos, 0, x_pos, 1, *res_pos);
    }
    return y_pos;
}

/* BOTTOM ROWS: rows wrap again, the centre column band does not. */
static void wrap_reduce_bottom(const wrap_reduce_state *s, int y_pos, int y_stop, int *res_pos)
{
    int x_pos;

    for (; y_pos < y_stop; y_pos += s->y_step) {
        for (x_pos = s->x_start; x_pos < s->x_ctr_start; x_pos += s->x_step, (*res_pos)++)
            wrap_inprod(s, y_pos, 1, x_pos + s->x_dim, 1, *res_pos);

        for (; x_pos < s->x_ctr_stop; x_pos += s->x_step, (*res_pos)++)
            wrap_inprod(s, y_pos, 1, x_pos, 0, *res_pos);

        for (; x_pos < s->x_stop; x_pos += s->x_step, (*res_pos)++)
            wrap_inprod(s, y_pos, 1, x_pos, 1, *res_pos);
    }
}

int internal_wrap_reduce(image, x_dim, y_dim, filt, x_fdim, y_fdim, x_start, x_step, x_stop,
                         y_start, y_step, y_stop, result)
register image_type *filt, *result;
register int x_dim, y_dim, x_fdim, y_fdim;
image_type *image;
int x_start, x_step, x_stop, y_start, y_step, y_stop;
{
    image_type **imval;
    int y_pos, y_im, res_pos = 0;
    int x_ctr_stop = x_dim - x_fdim + 1;
    int y_ctr_stop = y_dim - y_fdim + 1;
    int y_ctr_start = 0;
    int x_fmid = x_fdim / 2;
    int y_fmid = y_fdim / 2;
    wrap_reduce_state s;

    /* shift start/stop coords to filter upper left hand corner */
    x_start -= x_fmid;
    y_start -= y_fmid;
    x_stop -= x_fmid;
    y_stop -= y_fmid;

    if (x_stop < x_ctr_stop)
        x_ctr_stop = x_stop;
    if (y_stop < y_ctr_stop)
        y_ctr_stop = y_stop;

    /* Set up pointer array for rows */
    imval = (image_type **)malloc(y_dim * sizeof(image_type *));
    if (imval IS NULL) {
        printf("INTERNAL_WRAP: Failed to allocate temp array!");
        return (-1);
    }
    for (y_pos = y_im = 0; y_pos < y_dim; y_pos++, y_im += x_dim)
        imval[y_pos] = (image + y_im);

    s.imval = imval;
    s.filt = filt;
    s.result = result;
    s.x_dim = x_dim;
    s.y_dim = y_dim;
    s.x_fdim = x_fdim;
    s.filt_size = x_fdim * y_fdim;
    s.x_step = x_step;
    s.y_step = y_step;
    s.x_start = x_start;
    s.x_stop = x_stop;
    s.x_ctr_start = 0;
    s.x_ctr_stop = x_ctr_stop;

    y_pos = wrap_reduce_top(&s, y_start, y_ctr_start, &res_pos);
    y_pos = wrap_reduce_mid(&s, y_pos, y_ctr_stop, &res_pos);
    wrap_reduce_bottom(&s, y_pos, y_stop, &res_pos);

    free((image_type **)imval);

    return (0);
} /* end of internal_wrap_reduce */

/*
 --------------------------------------------------------------------
 Performs upsampling (padding with zeros) followed by convolution of
 FILT with IMAGE (a.k.a. EXPAND in Burt&Adelson81).  The operations
 are combined to avoid unnecessary multiplication of filter samples
 with zeros in the upsampled image.  The convolution is done in 9
 sections so that mod operation is not performed unnecessarily.
 Arguments are described in the comment above internal_wrap_reduce.

 WARNING: this subroutine destructively modifes the RESULT image, so
 the user must zero the result before invocation!
 -------------------------------------------------------------------- */

/* Shared state of one internal_wrap_expand() call; the analogue of
   wrap_reduce_state, writing into imval instead of reading from it. */
typedef struct {
    image_type **imval;
    image_type *image;
    image_type *filt;
    int x_dim;
    int y_dim;
    int x_fdim;
    int filt_size;
    int x_step;
    int y_step;
    int x_start;
    int x_stop;
    int x_ctr_start;
    int x_ctr_stop;
} wrap_expand_state;

/* abstract out the inner product computation (was the INPROD2 macro) */
static void wrap_inprod2(const wrap_expand_state *s, int y_startv, int y_wrap, int x_startv,
                         int x_wrap, int im_pos)
{
    double val = s->image[im_pos];
    int filt_pos, x_res, y_res, x_filt_stop;
    int row, col;

    for (y_res = y_startv, filt_pos = 0, x_filt_stop = s->x_fdim; x_filt_stop <= s->filt_size;
         y_res++, x_filt_stop += s->x_fdim)
        for (x_res = x_startv; filt_pos < x_filt_stop; filt_pos++, x_res++) {
            row = y_wrap ? (y_res % s->y_dim) : y_res;
            col = x_wrap ? (x_res % s->x_dim) : x_res;
            s->imval[row][col] += val * s->filt[filt_pos];
        }
}

/* TOP ROWS: rows wrap, the centre column band does not. */
static int wrap_expand_top(const wrap_expand_state *s, int y_start, int y_ctr_start, int *im_pos)
{
    int x_pos, y_pos;

    for (y_pos = y_start; y_pos < y_ctr_start; y_pos += s->y_step) {
        for (x_pos = s->x_start; x_pos < s->x_ctr_start; x_pos += s->x_step, (*im_pos)++)
            wrap_inprod2(s, y_pos + s->y_dim, 1, x_pos + s->x_dim, 1, *im_pos);

        for (; x_pos < s->x_ctr_stop; x_pos += s->x_step, (*im_pos)++)
            wrap_inprod2(s, y_pos + s->y_dim, 1, x_pos, 0, *im_pos);

        for (; x_pos < s->x_stop; x_pos += s->x_step, (*im_pos)++)
            wrap_inprod2(s, y_pos + s->y_dim, 1, x_pos, 1, *im_pos);
    }
    return y_pos;
}

/* MID ROWS: rows do not wrap; only the left and right column bands do. */
static int wrap_expand_mid(const wrap_expand_state *s, int y_pos, int y_ctr_stop, int *im_pos)
{
    int x_pos;

    for (; y_pos < y_ctr_stop; y_pos += s->y_step) {
        for (x_pos = s->x_start; x_pos < s->x_ctr_start; x_pos += s->x_step, (*im_pos)++)
            wrap_inprod2(s, y_pos, 0, x_pos + s->x_dim, 1, *im_pos);

        for (; /* CENTER SECTION */
             x_pos < s->x_ctr_stop; x_pos += s->x_step, (*im_pos)++)
            wrap_inprod2(s, y_pos, 0, x_pos, 0, *im_pos);

        for (; x_pos < s->x_stop; x_pos += s->x_step, (*im_pos)++)
            wrap_inprod2(s, y_pos, 0, x_pos, 1, *im_pos);
    }
    return y_pos;
}

/* BOTTOM ROWS: rows wrap again, the centre column band does not. */
static void wrap_expand_bottom(const wrap_expand_state *s, int y_pos, int y_stop, int *im_pos)
{
    int x_pos;

    for (; y_pos < y_stop; y_pos += s->y_step) {
        for (x_pos = s->x_start; x_pos < s->x_ctr_start; x_pos += s->x_step, (*im_pos)++)
            wrap_inprod2(s, y_pos, 1, x_pos + s->x_dim, 1, *im_pos);

        for (; x_pos < s->x_ctr_stop; x_pos += s->x_step, (*im_pos)++)
            wrap_inprod2(s, y_pos, 1, x_pos, 0, *im_pos);

        for (; x_pos < s->x_stop; x_pos += s->x_step, (*im_pos)++)
            wrap_inprod2(s, y_pos, 1, x_pos, 1, *im_pos);
    }
}

int internal_wrap_expand(image, filt, x_fdim, y_fdim, x_start, x_step, x_stop, y_start, y_step,
                         y_stop, result, x_dim, y_dim)
register image_type *filt, *result;
register int x_fdim, y_fdim, x_dim, y_dim;
image_type *image;
int x_start, x_step, x_stop, y_start, y_step, y_stop;
{
    image_type **imval;
    int y_pos, y_res, im_pos = 0;
    int x_ctr_stop = x_dim - x_fdim + 1;
    int y_ctr_stop = y_dim - y_fdim + 1;
    int y_ctr_start = 0;
    int x_fmid = x_fdim / 2;
    int y_fmid = y_fdim / 2;
    wrap_expand_state s;

    /* shift start/stop coords to filter upper left hand corner */
    x_start -= x_fmid;
    y_start -= y_fmid;
    x_stop -= x_fmid;
    y_stop -= y_fmid;

    if (x_stop < x_ctr_stop)
        x_ctr_stop = x_stop;
    if (y_stop < y_ctr_stop)
        y_ctr_stop = y_stop;

    /* Set up pointer array for rows */
    imval = (image_type **)malloc(y_dim * sizeof(image_type *));
    if (imval IS NULL) {
        printf("INTERNAL_WRAP: Failed to allocate temp array!");
        return (-1);
    }
    for (y_pos = y_res = 0; y_pos < y_dim; y_pos++, y_res += x_dim)
        imval[y_pos] = (result + y_res);

    s.imval = imval;
    s.image = image;
    s.filt = filt;
    s.x_dim = x_dim;
    s.y_dim = y_dim;
    s.x_fdim = x_fdim;
    s.filt_size = x_fdim * y_fdim;
    s.x_step = x_step;
    s.y_step = y_step;
    s.x_start = x_start;
    s.x_stop = x_stop;
    s.x_ctr_start = 0;
    s.x_ctr_stop = x_ctr_stop;

    y_pos = wrap_expand_top(&s, y_start, y_ctr_start, &im_pos);
    y_pos = wrap_expand_mid(&s, y_pos, y_ctr_stop, &im_pos);
    wrap_expand_bottom(&s, y_pos, y_stop, &im_pos);

    free((image_type **)imval);
    return (0);
} /* end of internal_wrap_expand */

/* Local Variables: */
/* buffer-read-only: t */
/* End: */
