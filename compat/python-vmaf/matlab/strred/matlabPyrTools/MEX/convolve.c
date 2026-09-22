/* 
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;  File: convolve.c
;;;  Author: Eero Simoncelli
;;;  Description: General convolution code for 2D images
;;;  Creation Date: Spring, 1987.
;;;  MODIFICATIONS:
;;;     10/89: approximately optimized the choice of register vars on SPARCS.
;;;      6/96: Switched array types to double float.
;;;      2/97: made more robust and readable.  Added STOP arguments.
;;;      8/97: Bug: when calling internal_reduce with edges in {reflect1,repeat,
;;;            extend} and an even filter dimension.  Solution: embed the filter
;;;            in the upper-left corner of a filter with odd Y and X dimensions.
;;;  ----------------------------------------------------------------
;;;    Object-Based Vision and Image Understanding System (OBVIUS),
;;;      Copyright 1988, Vision Science Group,  Media Laboratory,  
;;;              Massachusetts Institute of Technology.
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
*/

#include <stdio.h>
#include <math.h>
#include "convolve.h"

/*
  --------------------------------------------------------------------
  Correlate FILT with IMAGE, subsampling according to START, STEP, and
  STOP parameters, with values placed into RESULT array.  RESULT
  dimensions should be ceil((stop-start)/step).  TEMP should be a
  pointer to a temporary double array the size of the filter.
  EDGES is a string specifying how to handle boundaries -- see edges.c.
  The convolution is done in 9 sections, where the border sections use
  specially computed edge-handling filters (see edges.c). The origin 
  of the filter is assumed to be (floor(x_fdim/2), floor(y_fdim/2)).
------------------------------------------------------------------------ */

/* Shared state of one internal_reduce() call.  The row bands below used to be
   inline sections of that function reading a dozen `register` locals; passing
   them by struct keeps every band inside the 60-line complexity bound without
   changing a single index computation. */
typedef struct {
    image_type *image;
    image_type *temp;
    image_type *filt;
    image_type *result;
    int x_dim;
    int x_fdim;
    int y_fdim;
    int filt_size;
    int x_step;
    int y_step;
    int x_start;
    int x_stop;
    int x_ctr_start;
    int x_ctr_stop;
    int x_res_dim;
    fptr reflect;
} reduce_state;

/* abstract out the inner product computation (was the INPROD macro) */
static void reduce_inprod(const reduce_state *s, int xcnr, int ycnr, int res_pos)
{
    double sum = 0.0;
    int im_pos, filt_pos, x_filt_stop;

    for (im_pos = ycnr * s->x_dim + xcnr, filt_pos = 0, x_filt_stop = s->x_fdim;
         x_filt_stop <= s->filt_size; im_pos += (s->x_dim - s->x_fdim), x_filt_stop += s->x_fdim)
        for (; filt_pos < x_filt_stop; filt_pos++, im_pos++)
            sum += s->image[im_pos] * s->temp[filt_pos];
    s->result[res_pos] = sum;
}

/* TOP ROWS.  Advances *res_pos and returns the y position the middle band
   starts at (the old `y_ctr_start = y_pos` hand-off). */
static int reduce_top_rows(const reduce_state *s, int y_start, int y_ctr_start, int *res_pos)
{
    int x_pos, y_pos;

    for (y_pos = y_start; y_pos < y_ctr_start; y_pos += s->y_step) {
        for (x_pos = s->x_start; /* TOP-LEFT CORNER */
             x_pos < s->x_ctr_start; x_pos += s->x_step, (*res_pos)++) {
            (*s->reflect)(s->filt, s->x_fdim, s->y_fdim, x_pos - 1, y_pos - 1, s->temp, REDUCE);
            reduce_inprod(s, 0, 0, *res_pos);
        }

        (*s->reflect)(s->filt, s->x_fdim, s->y_fdim, 0, y_pos - 1, s->temp, REDUCE);
        for (; /* TOP EDGE */
             x_pos < s->x_ctr_stop; x_pos += s->x_step, (*res_pos)++)
            reduce_inprod(s, x_pos, 0, *res_pos);

        for (; /* TOP-RIGHT CORNER */
             x_pos < s->x_stop; x_pos += s->x_step, (*res_pos)++) {
            (*s->reflect)(s->filt, s->x_fdim, s->y_fdim, x_pos - s->x_ctr_stop + 1, y_pos - 1,
                          s->temp, REDUCE);
            reduce_inprod(s, s->x_ctr_stop, 0, *res_pos);
        }
    }
    return y_pos;
}

/* LEFT EDGE, CENTER and RIGHT EDGE.  Hands back the result index and the y
   position the bottom band continues from. */
static void reduce_middle_rows(const reduce_state *s, int y_ctr_start, int y_ctr_stop, int *res_pos,
                               int *y_pos_out)
{
    int x_pos, base_res_pos;
    int y_pos = y_ctr_start;
    int res = *res_pos;

    for (base_res_pos = res, x_pos = s->x_start; /* LEFT EDGE */
         x_pos < s->x_ctr_start; x_pos += s->x_step, base_res_pos++) {
        (*s->reflect)(s->filt, s->x_fdim, s->y_fdim, x_pos - 1, 0, s->temp, REDUCE);
        for (y_pos = y_ctr_start, res = base_res_pos; y_pos < y_ctr_stop;
             y_pos += s->y_step, res += s->x_res_dim)
            reduce_inprod(s, 0, y_pos, res);
    }

    (*s->reflect)(s->filt, s->x_fdim, s->y_fdim, 0, 0, s->temp, REDUCE);
    for (; /* CENTER */
         x_pos < s->x_ctr_stop; x_pos += s->x_step, base_res_pos++)
        for (y_pos = y_ctr_start, res = base_res_pos; y_pos < y_ctr_stop;
             y_pos += s->y_step, res += s->x_res_dim)
            reduce_inprod(s, x_pos, y_pos, res);

    for (; /* RIGHT EDGE */
         x_pos < s->x_stop; x_pos += s->x_step, base_res_pos++) {
        (*s->reflect)(s->filt, s->x_fdim, s->y_fdim, x_pos - s->x_ctr_stop + 1, 0, s->temp, REDUCE);
        for (y_pos = y_ctr_start, res = base_res_pos; y_pos < y_ctr_stop;
             y_pos += s->y_step, res += s->x_res_dim)
            reduce_inprod(s, s->x_ctr_stop, y_pos, res);
    }

    *res_pos = res;
    *y_pos_out = y_pos;
}

/* BOTTOM ROWS. */
static void reduce_bottom_rows(const reduce_state *s, int y_pos, int y_stop, int y_ctr_stop,
                               int res_pos)
{
    int x_pos;

    for (res_pos -= (s->x_res_dim - 1); y_pos < y_stop; y_pos += s->y_step) {
        for (x_pos = s->x_start; /* BOTTOM-LEFT CORNER */
             x_pos < s->x_ctr_start; x_pos += s->x_step, res_pos++) {
            (*s->reflect)(s->filt, s->x_fdim, s->y_fdim, x_pos - 1, y_pos - y_ctr_stop + 1, s->temp,
                          REDUCE);
            reduce_inprod(s, 0, y_ctr_stop, res_pos);
        }

        (*s->reflect)(s->filt, s->x_fdim, s->y_fdim, 0, y_pos - y_ctr_stop + 1, s->temp, REDUCE);
        for (; /* BOTTOM EDGE */
             x_pos < s->x_ctr_stop; x_pos += s->x_step, res_pos++)
            reduce_inprod(s, x_pos, y_ctr_stop, res_pos);

        for (; /* BOTTOM-RIGHT CORNER */
             x_pos < s->x_stop; x_pos += s->x_step, res_pos++) {
            (*s->reflect)(s->filt, s->x_fdim, s->y_fdim, x_pos - s->x_ctr_stop + 1,
                          y_pos - y_ctr_stop + 1, s->temp, REDUCE);
            reduce_inprod(s, s->x_ctr_stop, y_ctr_stop, res_pos);
        }
    }
}

int internal_reduce(image, x_dim, y_dim, filt, temp, x_fdim, y_fdim, x_start, x_step, x_stop,
                    y_start, y_step, y_stop, result, edges)
register image_type *image, *temp;
register int x_fdim, x_dim;
register image_type *result;
register int x_step, y_step;
int x_start, y_start;
int x_stop, y_stop;
image_type *filt;
int y_dim, y_fdim;
char *edges;
{
    int res_pos = 0;
    int y_pos;
    int y_ctr_stop = y_dim - ((y_fdim == 1) ? 0 : y_fdim);
    int x_ctr_stop = x_dim - ((x_fdim == 1) ? 0 : x_fdim);
    int y_ctr_start = ((y_fdim == 1) ? 0 : 1);
    int x_fmid = x_fdim / 2;
    int y_fmid = y_fdim / 2;
    reduce_state s;

    s.reflect = edge_function(edges); /* look up edge-handling function */
    if (!s.reflect)
        return (-1);

    /* shift start/stop coords to filter upper left hand corner */
    x_start -= x_fmid;
    y_start -= y_fmid;
    x_stop -= x_fmid;
    y_stop -= y_fmid;

    if (x_stop < x_ctr_stop)
        x_ctr_stop = x_stop;
    if (y_stop < y_ctr_stop)
        y_ctr_stop = y_stop;

    s.image = image;
    s.temp = temp;
    s.filt = filt;
    s.result = result;
    s.x_dim = x_dim;
    s.x_fdim = x_fdim;
    s.y_fdim = y_fdim;
    s.filt_size = x_fdim * y_fdim;
    s.x_step = x_step;
    s.y_step = y_step;
    s.x_start = x_start;
    s.x_stop = x_stop;
    s.x_ctr_start = ((x_fdim == 1) ? 0 : 1);
    s.x_ctr_stop = x_ctr_stop;
    s.x_res_dim = (x_stop - x_start + x_step - 1) / x_step;

    y_ctr_start = reduce_top_rows(&s, y_start, y_ctr_start, &res_pos);
    reduce_middle_rows(&s, y_ctr_start, y_ctr_stop, &res_pos, &y_pos);
    reduce_bottom_rows(&s, y_pos, y_stop, y_ctr_stop, res_pos);

    return (0);
} /* end of internal_reduce */

/*
  --------------------------------------------------------------------
  Upsample IMAGE according to START,STEP, and STOP parameters and then
  convolve with FILT, adding values into RESULT array.  IMAGE
  dimensions should be ceil((stop-start)/step).  See
  description of internal_reduce (above).

  WARNING: this subroutine destructively modifies the RESULT array!
 ------------------------------------------------------------------------ */

/* Shared state of one internal_expand() call; the analogue of reduce_state. */
typedef struct {
    image_type *image;
    image_type *temp;
    image_type *filt;
    image_type *result;
    int x_dim;
    int x_fdim;
    int y_fdim;
    int filt_size;
    int x_step;
    int y_step;
    int x_start;
    int x_stop;
    int x_ctr_start;
    int x_ctr_stop;
    int x_im_dim;
    fptr reflect;
} expand_state;

/* abstract out the inner product computation (was the INPROD2 macro) */
static void expand_inprod(const expand_state *s, int xcnr, int ycnr, int im_pos)
{
    double val = s->image[im_pos];
    int res_pos, filt_pos, x_filt_stop;

    for (res_pos = ycnr * s->x_dim + xcnr, filt_pos = 0, x_filt_stop = s->x_fdim;
         x_filt_stop <= s->filt_size; res_pos += (s->x_dim - s->x_fdim), x_filt_stop += s->x_fdim)
        for (; filt_pos < x_filt_stop; filt_pos++, res_pos++)
            s->result[res_pos] += val * s->temp[filt_pos];
}

/* TOP ROWS.  Advances *im_pos and returns the y position the middle band
   starts at (the old `y_ctr_start = y_pos` hand-off). */
static int expand_top_rows(const expand_state *s, int y_start, int y_ctr_start, int *im_pos)
{
    int x_pos, y_pos;

    for (y_pos = y_start; y_pos < y_ctr_start; y_pos += s->y_step) {
        for (x_pos = s->x_start; /* TOP-LEFT CORNER */
             x_pos < s->x_ctr_start; x_pos += s->x_step, (*im_pos)++) {
            (*s->reflect)(s->filt, s->x_fdim, s->y_fdim, x_pos - 1, y_pos - 1, s->temp, EXPAND);
            expand_inprod(s, 0, 0, *im_pos);
        }

        (*s->reflect)(s->filt, s->x_fdim, s->y_fdim, 0, y_pos - 1, s->temp, EXPAND);
        for (; /* TOP EDGE */
             x_pos < s->x_ctr_stop; x_pos += s->x_step, (*im_pos)++)
            expand_inprod(s, x_pos, 0, *im_pos);

        for (; /* TOP-RIGHT CORNER */
             x_pos < s->x_stop; x_pos += s->x_step, (*im_pos)++) {
            (*s->reflect)(s->filt, s->x_fdim, s->y_fdim, x_pos - s->x_ctr_stop + 1, y_pos - 1,
                          s->temp, EXPAND);
            expand_inprod(s, s->x_ctr_stop, 0, *im_pos);
        }
    }
    return y_pos;
}

/* LEFT EDGE, CENTER and RIGHT EDGE. */
static void expand_middle_rows(const expand_state *s, int y_ctr_start, int y_ctr_stop, int *im_pos,
                               int *y_pos_out)
{
    int x_pos, base_im_pos;
    int y_pos = y_ctr_start;
    int im = *im_pos;

    for (base_im_pos = im, x_pos = s->x_start; /* LEFT EDGE */
         x_pos < s->x_ctr_start; x_pos += s->x_step, base_im_pos++) {
        (*s->reflect)(s->filt, s->x_fdim, s->y_fdim, x_pos - 1, 0, s->temp, EXPAND);
        for (y_pos = y_ctr_start, im = base_im_pos; y_pos < y_ctr_stop;
             y_pos += s->y_step, im += s->x_im_dim)
            expand_inprod(s, 0, y_pos, im);
    }

    (*s->reflect)(s->filt, s->x_fdim, s->y_fdim, 0, 0, s->temp, EXPAND);
    for (; /* CENTER */
         x_pos < s->x_ctr_stop; x_pos += s->x_step, base_im_pos++)
        for (y_pos = y_ctr_start, im = base_im_pos; y_pos < y_ctr_stop;
             y_pos += s->y_step, im += s->x_im_dim)
            expand_inprod(s, x_pos, y_pos, im);

    for (; /* RIGHT EDGE */
         x_pos < s->x_stop; x_pos += s->x_step, base_im_pos++) {
        (*s->reflect)(s->filt, s->x_fdim, s->y_fdim, x_pos - s->x_ctr_stop + 1, 0, s->temp, EXPAND);
        for (y_pos = y_ctr_start, im = base_im_pos; y_pos < y_ctr_stop;
             y_pos += s->y_step, im += s->x_im_dim)
            expand_inprod(s, s->x_ctr_stop, y_pos, im);
    }

    *im_pos = im;
    *y_pos_out = y_pos;
}

/* BOTTOM ROWS. */
static void expand_bottom_rows(const expand_state *s, int y_pos, int y_stop, int y_ctr_stop,
                               int im_pos)
{
    int x_pos;

    for (im_pos -= (s->x_im_dim - 1); y_pos < y_stop; y_pos += s->y_step) {
        for (x_pos = s->x_start; /* BOTTOM-LEFT CORNER */
             x_pos < s->x_ctr_start; x_pos += s->x_step, im_pos++) {
            (*s->reflect)(s->filt, s->x_fdim, s->y_fdim, x_pos - 1, y_pos - y_ctr_stop + 1, s->temp,
                          EXPAND);
            expand_inprod(s, 0, y_ctr_stop, im_pos);
        }

        (*s->reflect)(s->filt, s->x_fdim, s->y_fdim, 0, y_pos - y_ctr_stop + 1, s->temp, EXPAND);
        for (; /* BOTTOM EDGE */
             x_pos < s->x_ctr_stop; x_pos += s->x_step, im_pos++)
            expand_inprod(s, x_pos, y_ctr_stop, im_pos);

        for (; /* BOTTOM-RIGHT CORNER */
             x_pos < s->x_stop; x_pos += s->x_step, im_pos++) {
            (*s->reflect)(s->filt, s->x_fdim, s->y_fdim, x_pos - s->x_ctr_stop + 1,
                          y_pos - y_ctr_stop + 1, s->temp, EXPAND);
            expand_inprod(s, s->x_ctr_stop, y_ctr_stop, im_pos);
        }
    }
}

int internal_expand(image, filt, temp, x_fdim, y_fdim, x_start, x_step, x_stop, y_start, y_step,
                    y_stop, result, x_dim, y_dim, edges)
register image_type *result, *temp;
register int x_fdim, x_dim;
register int x_step, y_step;
register image_type *image;
int x_start, y_start;
image_type *filt;
int y_fdim, y_dim;
char *edges;
{
    int im_pos = 0;
    int y_pos;
    int x_ctr_stop = x_dim - ((x_fdim == 1) ? 0 : x_fdim);
    int y_ctr_stop = (y_dim - ((y_fdim == 1) ? 0 : y_fdim));
    int y_ctr_start = ((y_fdim == 1) ? 0 : 1);
    int x_fmid = x_fdim / 2;
    int y_fmid = y_fdim / 2;
    expand_state s;

    s.reflect = edge_function(edges); /* look up edge-handling function */
    if (!s.reflect)
        return (-1);

    /* shift start/stop coords to filter upper left hand corner */
    x_start -= x_fmid;
    y_start -= y_fmid;
    x_stop -= x_fmid;
    y_stop -= y_fmid;

    if (x_stop < x_ctr_stop)
        x_ctr_stop = x_stop;
    if (y_stop < y_ctr_stop)
        y_ctr_stop = y_stop;

    s.image = image;
    s.temp = temp;
    s.filt = filt;
    s.result = result;
    s.x_dim = x_dim;
    s.x_fdim = x_fdim;
    s.y_fdim = y_fdim;
    s.filt_size = x_fdim * y_fdim;
    s.x_step = x_step;
    s.y_step = y_step;
    s.x_start = x_start;
    s.x_stop = x_stop;
    s.x_ctr_start = ((x_fdim == 1) ? 0 : 1);
    s.x_ctr_stop = x_ctr_stop;
    s.x_im_dim = (x_stop - x_start + x_step - 1) / x_step;

    y_ctr_start = expand_top_rows(&s, y_start, y_ctr_start, &im_pos);
    expand_middle_rows(&s, y_ctr_start, y_ctr_stop, &im_pos, &y_pos);
    expand_bottom_rows(&s, y_pos, y_stop, y_ctr_stop, im_pos);

    return (0);
} /* end of internal_expand */

/* Local Variables: */
/* buffer-read-only: t */
/* End: */
