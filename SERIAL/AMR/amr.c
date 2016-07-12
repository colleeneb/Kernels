/*
Copyright (c) 2013, Intel Corporation

Redistribution and use in source and binary forms, with or without 
modification, are permitted provided that the following conditions 
are met:

* Redistributions of source code must retain the above copyright 
      notice, this list of conditions and the following disclaimer.
* Redistributions in binary form must reproduce the above 
      copyright notice, this list of conditions and the following 
      disclaimer in the documentation and/or other materials provided 
      with the distribution.
* Neither the name of Intel Corporation nor the names of its 
      contributors may be used to endorse or promote products 
      derived from this software without specific prior written 
      permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS 
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT 
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS 
FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE 
COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, 
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, 
BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; 
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER 
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT 
LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN 
ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE 
POSSIBILITY OF SUCH DAMAGE.
*/

/*******************************************************************

NAME:    Stencil

PURPOSE: This program tests the efficiency with which a space-invariant,
         linear, symmetric filter (stencil) can be applied to a square
         grid or image, with periodic introduction and removal of
         subgrids.
  
USAGE:   The program takes as input the linear
         dimension of the grid, and the number of iterations on the grid

               <progname> <iterations> <background grid size> <refinement level>
                          <refinement size> <refinement period> 
                          <refinement duration> <refinement sub-iterations>
  
         The output consists of diagnostics to make sure the 
         algorithm worked, and of timing statistics.

FUNCTIONS CALLED:

         Other than standard C functions, the following functions are used in 
         this program:
         wtime()

HISTORY: - Written by Rob Van der Wijngaart, February 2009.
         - RvdW: Removed unrolling pragmas for clarity;
           added constant to array "in" at end of each iteration to force 
           refreshing of neighbor data in parallel versions; August 2013
  
**********************************************************************************/

#include <par-res-kern_general.h>

#if DOUBLE
  #define DTYPE   double
  #define EPSILON 1.e-8
  #define COEFX   1.0
  #define COEFY   1.0
  #define FSTR    "%lf"
#else
  #define DTYPE   float
  #define EPSILON 0.001f
  #define COEFX   1.0f
  #define COEFY   1.0f
  #define FSTR    "%f"
#endif

/* define shorthand for indexing multi-dimensional arrays                        */
#define IN(i,j)        in[i+(j)*(n)]
#define OUT(i,j)       out[i+(j)*(n)]
#define INR(g,i,j)     inr[g][i+(j)*(nr_true)]
#define INRG(i,j)      inrg[i+(j)*(nr_true)]
#define OUTR(g,i,j)    outr[g][i+(j)*(nr_true)]
#define WEIGHT(ii,jj)  weight[ii+RADIUS][jj+RADIUS]
#define WEIGHTR(ii,jj) weight_r[ii+RADIUS][jj+RADIUS]

/* using bi-linear interpolation                                                 */
void interpolate(DTYPE *inrg, DTYPE *in, long n, long nr_true, 
                 long rstarti, long rstartj, long expand,
                 DTYPE hr) {
  long ir, jr, ib, jrb, jrb1, jb, rendi, rendj;
  DTYPE xr, xb, yr, yb;

  if (hr==(DTYPE)1.0) {
    for (jr=0; jr<nr_true; jr++) for (ir=0; ir<nr_true; ir++)
      INRG(ir,jr) = IN(ir+rstarti,jr+rstartj);
  } 
  else {
    rendi = rstarti+(nr_true-1)/expand;
    rendj = rstartj+(nr_true-1)/expand;
    /* First, interpolate in x-direction                                        */
    for (jr=0,jb=rstartj; jr<nr_true; jr+=expand,jb++) {
      for (ir=0; ir<nr_true-1; ir++) {
        xr = rstarti+hr*(DTYPE)ir;
        ib = (long)xr;
        xb = (DTYPE)ib;
        INRG(ir,jr) = IN(ib+1,jb)*(xr-xb) + IN(ib,jb)*(xb+(DTYPE)1.0-xr);
      }
      INRG(nr_true-1,jr) = IN(rendi,jb);
    }

    /* Next, interpolate in y-direction                                         */
    for (jr=0; jr<nr_true-1; jr++) {
      yr = hr*(DTYPE)jr;
      jb = (long)yr;
      jrb = jb*expand;
      jrb1 = (jb+1)*expand;
      yb = floor(yr);
      for (ir=0; ir<nr_true; ir++) {
        INRG(ir,jr) = INRG(ir,jrb1)*(yr-yb) + INRG(ir,jrb)*(yb+(DTYPE)1.0-yr);
      }
    }
  }
}

int main(int argc, char ** argv) {

  long   n;                 /* linear grid dimension                             */
  int    r_level;           /* refinement level                                  */
  long   rstarti[4];        /* left boundary of refinements                      */
  long   rstartj[4];        /* bottom boundary of refinements                    */
  int    g;                 /* refinement grid index                             */
  long   nr;                /* linear refinement size in bg grid units           */
  long   nr_true;           /* linear refinement size                            */
  long   expand;            /* number of refinement cells per background cell    */
  int    period;            /* refinement period                                 */
  int    duration;          /* lifetime of a refinement                          */
  int    sub_iterations;    /* number of sub-iterations on refinement            */
  int    tile_size;         /* loop nest block factor                            */
  int    tiling=0;          /* boolean indication loop nest blocking             */
  long   i, j, ii, jj, it, jt, iter, l, sub_iter;  /* dummies                    */
  DTYPE  norm,              /* L1 norm of solution on background grid            */
         reference_norm;
  DTYPE  norm_r[4],         /* L1 norm of solution on refinements                */
         reference_norm_r[4];
  DTYPE  hr;                /* mesh spacing of refinement                        */
  DTYPE  f_active_points;   /* interior of grid with respect to stencil          */
  DTYPE  f_active_points_r; /* interior of refinement with respect to stencil    */
  DTYPE  flops;             /* floating point ops per iteration                  */
  int    iterations;        /* number of times to run the algorithm              */
  int    iterations_r[4];   /* number of iterations on each refinement           */
  int    full_cycles;       /* number of full cycles all refinement grids appear */
  int    leftover_iterations;/* number of iterations in last partial AMR cycle   */
  int    num_interpolations;/* total number of timed interpolations              */
  double stencil_time,      /* timing parameters                                 */
         avgtime;
  int    stencil_size;      /* number of points in stencil                       */
  DTYPE  * RESTRICT in;     /* background grid input values                      */
  DTYPE  * RESTRICT out;    /* background grid output values                     */
  DTYPE  * RESTRICT inr[4]; /* refinement grid input values                      */
  DTYPE  * RESTRICT outr[4];/* refinement grid output values                     */
  long   total_length;      /* total required length to store bg grid values     */
  long   total_lengthr;     /* total required length to store refinement values  */
  DTYPE  weight[2*RADIUS+1][2*RADIUS+1]; /* weights of points in the stencil     */
  DTYPE  weight_r[2*RADIUS+1][2*RADIUS+1]; /* weights of points in the stencil   */
  int    validate=1;        /* tracks correct solution on all grids              */

  printf("Parallel Research Kernels Version %s\n", PRKVERSION);
  printf("Serial AMR stencil execution on 2D grid\n");

  /*******************************************************************************
  ** process and test input parameters    
  ********************************************************************************/

  if (argc != 8 && argc !=9){
    printf("Usage: %s <# iterations> <background grid size> <refinement size>\n",
           *argv);
    printf("       <refinement level> <refinement period>  <refinement duration>\n");
    printf("       <refinement sub-iterations> [tile_size]\n");
    return(EXIT_FAILURE);
  }

  iterations  = atoi(*++argv); 
  if (iterations < 1){
    printf("ERROR: iterations must be >= 1 : %d \n",iterations);
    exit(EXIT_FAILURE);
  }

  n  = atol(*++argv);

  if (n < 2){
    printf("ERROR: grid must have at least one cell: %ld\n", n);
    exit(EXIT_FAILURE);
  }

  nr = atol(*++argv);
  if (nr < 1) {
    printf("ERROR: refinements must have at least one cell: %ld\n", n);
    exit(EXIT_FAILURE);
  }
  if (nr>=n) {
    printf("ERROR: refinements must be contained in background grid: %ld\n", nr);
    exit(EXIT_FAILURE);
  }

  r_level = atoi(*++argv);
  if (r_level < 0) {
    printf("ERROR: refinement levels must be >= 0 : %d\n", r_level);
    exit(EXIT_FAILURE);
  }

  /* calculate refinement mesh spacing plus ratio of mesh spacings */
  hr = (DTYPE)1.0; expand = 1;
  for (l=0; l<r_level; l++) {
    hr /= (DTYPE)2.0;
    expand *= 2;
  }

  period = atoi(*++argv);
  if (period < 1) {
    printf("ERROR: refinement period must be at least one: %d\n", period);
    exit(EXIT_FAILURE);
  }
  
  duration = atoi(*++argv);
  if (duration < 1 || duration > period) {
    printf("ERROR: refinement duration must be positive, no greater than period: %d\n",
           duration);
    exit(EXIT_FAILURE);
  }

  sub_iterations = atoi(*++argv);
  if (sub_iterations < 1) {
    printf("ERROR: refinement sub-iterations must be positive: %d\n", sub_iterations);
    exit(EXIT_FAILURE);
  }

  if (argc == 9) {
    tile_size = atoi(*++argv);
    if (tile_size<=0 || tile_size>n) tile_size=n;
    else tiling=1; 
  }

  if (RADIUS < 1) {
    printf("ERROR: Stencil radius %d should be positive\n", RADIUS);
    exit(EXIT_FAILURE);
  }

  if (2*RADIUS+1 > n) {
    printf("ERROR: Stencil radius %d exceeds grid size %ld\n", RADIUS, n);
    exit(EXIT_FAILURE);
  }

  nr_true = nr*expand+1;
  if (2*RADIUS+1 > nr_true) {
    printf("ERROR: Stencil radius %d exceeds refinement size %ld\n", RADIUS, nr_true);
    exit(EXIT_FAILURE);
  }

  /* reserve space for background input/output fields                       */
  total_length = n*n*sizeof(DTYPE);
  in  = (DTYPE *) prk_malloc(total_length);
  out = (DTYPE *) prk_malloc(total_length);
  if (!in || !out) {
    printf("ERROR: could not allocate space for input or output array\n");
    exit(EXIT_FAILURE);
  }

  /* reserve space for refinement input/output fields                       */
  total_lengthr = 4*nr_true*nr_true*sizeof(DTYPE);
  inr[0]  = (DTYPE *) prk_malloc(total_lengthr);
  outr[0] = (DTYPE *) prk_malloc(total_lengthr);
  if (!inr[0] || !outr[0]) {
    printf("ERROR: could not allocate space for refinement input or output arrays\n");
    exit(EXIT_FAILURE);
  }
  for (g=1; g<4; g++) {
    inr[g]  = inr[g-1]  +  nr_true*nr_true;
    outr[g] = outr[g-1] +  nr_true*nr_true;
  }

  /* fill the stencil weights to reflect a discrete divergence operator     */
  for (jj=-RADIUS; jj<=RADIUS; jj++) {
      for (ii=-RADIUS; ii<=RADIUS; ii++) {
          WEIGHT(ii,jj) = (DTYPE) 0.0;
      }
  }
#if STAR
  stencil_size = 4*RADIUS+1;
  for (ii=1; ii<=RADIUS; ii++) {
    WEIGHT(0, ii) = WEIGHT( ii,0) =  (DTYPE) (1.0/(2.0*ii*RADIUS));
    WEIGHT(0,-ii) = WEIGHT(-ii,0) = -(DTYPE) (1.0/(2.0*ii*RADIUS));
  }
#else
  stencil_size = (2*RADIUS+1)*(2*RADIUS+1);
  for (jj=1; jj<=RADIUS; jj++) {
    for (ii=-jj+1; ii<jj; ii++) {
      WEIGHT(ii,jj)  =  (DTYPE) (1.0/(4.0*jj*(2.0*jj-1)*RADIUS));
      WEIGHT(ii,-jj) = -(DTYPE) (1.0/(4.0*jj*(2.0*jj-1)*RADIUS));
      WEIGHT(jj,ii)  =  (DTYPE) (1.0/(4.0*jj*(2.0*jj-1)*RADIUS));
      WEIGHT(-jj,ii) = -(DTYPE) (1.0/(4.0*jj*(2.0*jj-1)*RADIUS));
    }
    WEIGHT(jj,jj)    =  (DTYPE) (1.0/(4.0*jj*RADIUS));
    WEIGHT(-jj,-jj)  = -(DTYPE) (1.0/(4.0*jj*RADIUS));
  }
#endif

  /* weights for the refinement have to be scaled with the mesh spacing   */
  for (jj=-RADIUS; jj<=RADIUS; jj++) {
    for (ii=-RADIUS; ii<=RADIUS; ii++) {
      WEIGHTR(ii,jj) = WEIGHT(ii,jj)*(DTYPE)expand;
    }
  }
  
  f_active_points   = (DTYPE) (n-2*RADIUS)*(DTYPE) (n-2*RADIUS);
  f_active_points_r = (DTYPE) (nr_true-2*RADIUS)*(DTYPE) (nr_true-2*RADIUS);

  printf("Background grid size = %ld\n", n);
  printf("Radius of stencil    = %d\n", RADIUS);
#if STAR
  printf("Type of stencil      = star\n");
#else
  printf("Type of stencil      = compact\n");
#endif
#if DOUBLE
  printf("Data type            = double precision\n");
#else
  printf("Data type            = single precision\n");
#endif
#if LOOPGEN
  printf("Script used to expand stencil loop body\n");
#else
  printf("Compact representation of stencil loop body\n");
#endif
  if (tiling) printf("Tile size            = %d\n", tile_size);
  else        printf("Untiled\n");
  printf("Number of iterations = %d\n", iterations);
  printf("Refinements:\n");
  printf("   Coarse grid cells = %ld\n", nr);
  printf("   Grid size         = %ld\n", nr_true);
  printf("   Period            = %d\n", period);
  printf("   Duration          = %d\n", duration);
  printf("   Level             = %d\n", r_level);
  printf("   Sub-iterations    = %d\n", sub_iterations);

  /* intialize the input and output arrays                                     */
  for (j=0; j<n; j++) for (i=0; i<n; i++) 
    IN(i,j)  = COEFX*i+COEFY*j;
  for (j=RADIUS; j<n-RADIUS; j++) for (i=RADIUS; i<n-RADIUS; i++) 
    OUT(i,j) = (DTYPE)0.0;

  /* compute layout of refinements (bottom left bg grid coordinate             */
  rstarti[0] = rstarti[2] = 0;
  rstarti[1] = rstarti[3] = n-nr-1;
  rstartj[0] = rstartj[3] = 0;
  rstartj[1] = rstartj[2] = n-nr-1;

  /* intialize the refinement arrays                                           */
  for (g=0; g<4; g++) {
    for (j=0; j<nr_true; j++) for (i=0; i<nr_true; i++) 
      INR(g,i,j)  = (DTYPE)0.0;
    for (j=RADIUS; j<nr_true-RADIUS; j++) for (i=RADIUS; i<nr_true-RADIUS; i++) 
      OUTR(g,i,j) = (DTYPE)0.0;
  }
  stencil_time = 0.0; /* silence compiler warning */

  num_interpolations = 0;

  for (iter = 0; iter<=iterations; iter++){

    /* start timer after a warmup iteration */
    if (iter == 1)  stencil_time = wtime();

    if (!(iter%period)) {
      /* a specific refinement has come to life                                */
      g=(iter/period)%4;
      num_interpolations++;
      interpolate(inr[g], in, n, nr_true, rstarti[g], rstartj[g], expand, hr);
    }

    if ((iter%period) < duration) {
      for (sub_iter=0; sub_iter<sub_iterations; sub_iter++) {
        if (!tiling) {
          for (j=RADIUS; j<nr_true-RADIUS; j++) {
            for (i=RADIUS; i<nr_true-RADIUS; i++) {
              #if STAR
                #if LOOPGEN
                  #include "loop_body_star_amr.incl"
                #else
                  for (jj=-RADIUS; jj<=RADIUS; jj++)  OUTR(g,i,j) += WEIGHTR(0,jj)*INR(g,i,j+jj);
                  for (ii=-RADIUS; ii<0; ii++)        OUTR(g,i,j) += WEIGHTR(ii,0)*INR(g,i+ii,j);
                  for (ii=1; ii<=RADIUS; ii++)        OUTR(g,i,j) += WEIGHTR(ii,0)*INR(g,i+ii,j);
                #endif
              #else 
                #if LOOPGEN
                  #include "loop_body_compact_amr.incl"
                #else
                  /* would like to be able to unroll this loop, but compiler will ignore  */
                  for (jj=-RADIUS; jj<=RADIUS; jj++) 
		    for (ii=-RADIUS; ii<=RADIUS; ii++)  OUTR(g,i,j) += WEIGHTR(ii,jj)*INR(g,i+ii,j+jj);
                #endif
              #endif
            }
          }
        }
        else {
          for (jt=RADIUS; jt<nr_true-RADIUS; jt+=tile_size) {
            for (it=RADIUS; it<nr_true-RADIUS; it+=tile_size) {
              for (j=jt; j<MIN(nr_true-RADIUS,jt+tile_size); j++) {
                for (i=it; i<MIN(nr_true-RADIUS,it+tile_size); i++) {
                  #if STAR
                    #if LOOPGEN
                      #include "loop_body_star_amr.incl"
                    #else
                      for (jj=-RADIUS; jj<=RADIUS; jj++)  OUTR(g,i,j) += WEIGHTR(0,jj)*INR(g,i,j+jj);
                      for (ii=-RADIUS; ii<0; ii++)        OUTR(g,i,j) += WEIGHTR(ii,0)*INR(g,i+ii,j);
                      for (ii=1; ii<=RADIUS; ii++)        OUTR(g,i,j) += WEIGHTR(ii,0)*INR(g,i+ii,j);
                    #endif
                  #else 
                    #if LOOPGEN
                      #include "loop_body_compact_amr.incl"
                    #else
                      /* would like to be able to unroll this loop, but compiler will ignore  */
                      for (jj=-RADIUS; jj<=RADIUS; jj++) 
			for (ii=-RADIUS; ii<=RADIUS; ii++)  OUTR(g,i,j) += WEIGHTR(ii,jj)*INR(g,i+ii,j+jj);
                    #endif
                  #endif
                }
              }
            }
          }
        }
      }
      /* add constant to solution to force refresh of neighbor data, if any        */
      for (j=0; j<nr_true; j++) for (i=0; i<nr_true; i++) INR(g,i,j)+= (DTYPE)1.0;
    }

    /* Apply the stencil operator to background grid                           */

    if (!tiling) {
      for (j=RADIUS; j<n-RADIUS; j++) {
        for (i=RADIUS; i<n-RADIUS; i++) {
          #if STAR
            #if LOOPGEN
              #include "loop_body_star.incl"
            #else
              for (jj=-RADIUS; jj<=RADIUS; jj++)  OUT(i,j) += WEIGHT(0,jj)*IN(i,j+jj);
              for (ii=-RADIUS; ii<0; ii++)        OUT(i,j) += WEIGHT(ii,0)*IN(i+ii,j);
              for (ii=1; ii<=RADIUS; ii++)        OUT(i,j) += WEIGHT(ii,0)*IN(i+ii,j);
            #endif
          #else 
            #if LOOPGEN
              #include "loop_body_compact.incl"
            #else
              /* would like to be able to unroll this loop, but compiler will ignore  */
              for (jj=-RADIUS; jj<=RADIUS; jj++) 
              for (ii=-RADIUS; ii<=RADIUS; ii++)  OUT(i,j) += WEIGHT(ii,jj)*IN(i+ii,j+jj);
            #endif
          #endif
        }
      }
    }
    else {
      for (jt=RADIUS; jt<n-RADIUS; jt+=tile_size) {
        for (it=RADIUS; it<n-RADIUS; it+=tile_size) {
          for (j=jt; j<MIN(n-RADIUS,jt+tile_size); j++) {
            for (i=it; i<MIN(n-RADIUS,it+tile_size); i++) {
              #if STAR
                #if LOOPGEN
                  #include "loop_body_star.incl"
                #else
                  for (jj=-RADIUS; jj<=RADIUS; jj++)  OUT(i,j) += WEIGHT(0,jj)*IN(i,j+jj);
                  for (ii=-RADIUS; ii<0; ii++)        OUT(i,j) += WEIGHT(ii,0)*IN(i+ii,j);
                  for (ii=1; ii<=RADIUS; ii++)        OUT(i,j) += WEIGHT(ii,0)*IN(i+ii,j);
                #endif
              #else 
                #if LOOPGEN
                  #include "loop_body_compact.incl"
                #else
                  /* would like to be able to unroll this loop, but compiler will ignore  */
                  for (jj=-RADIUS; jj<=RADIUS; jj++) 
                  for (ii=-RADIUS; ii<=RADIUS; ii++)  OUT(i,j) += WEIGHT(ii,jj)*IN(i+ii,j+jj);
                #endif
              #endif
            }
          }
        }
      }
    }

    /* add constant to solution to force refresh of neighbor data, if any        */
    for (j=0; j<n; j++) for (i=0; i<n; i++) IN(i,j)+= (DTYPE)1.0;

  } /* end of iterations                                                         */

  stencil_time = wtime() - stencil_time;

  /* compute L1 norm on background grid                                          */
  norm = (DTYPE) 0.0;
  for (j=RADIUS; j<n-RADIUS; j++) for (i=RADIUS; i<n-RADIUS; i++) {
    norm += (DTYPE)ABS(OUT(i,j));
  }
  norm /= f_active_points;
  
  /* compute L1 norm on refinements                                              */
  for (g=0; g<4; g++) {
    norm_r[g] = (DTYPE) 0.0;
    for (j=RADIUS; j<nr_true-RADIUS; j++) for (i=RADIUS; i<nr_true-RADIUS; i++) {
      norm_r[g] += (DTYPE)ABS(OUTR(g,i,j));
      //      printf("g=%d, OUTR(%d,%d)=%lf\n", g, i, j, OUTR(g,i,j));
    }
    norm_r[g] /= f_active_points_r;
  }

  /*******************************************************************************
  ** Analyze and output results.
  ********************************************************************************/

/* verify correctness of background grid solution                                */
  reference_norm = (DTYPE) (iterations+1) * (COEFX + COEFY);
  if (ABS(norm-reference_norm) > EPSILON) {
    printf("ERROR: L1 norm = "FSTR", Reference L1 norm = "FSTR"\n",
           norm, reference_norm);
    validate = 0;
  }
  else {
#if VERBOSE
    printf("Reference L1 norm = "FSTR", L1 norm = "FSTR"\n", 
           reference_norm, norm);
#endif
  }

/* verify correctness of refinement grid solutions                               */
  for (g=0; g<4; g++) {
    full_cycles = ((iterations+1)/(period*4));
    leftover_iterations = (iterations+1)%(period*4);
    iterations_r[g] = sub_iterations*(full_cycles*duration+
                      MIN(MAX(0,leftover_iterations-g*period),duration));
    reference_norm_r[g] = (DTYPE) (iterations_r[g]) * (COEFX + COEFY);
    if (ABS(norm_r[g]-reference_norm_r[g]) > EPSILON) {
      printf("ERROR: L1 norm %d = "FSTR", Reference L1 norm = "FSTR"\n",
             g, norm_r[g], reference_norm_r[g]);
      validate = 0;
    }
    else {
#if VERBOSE
      printf("Reference L1 norm %d = "FSTR", L1 norm = "FSTR"\n", 
             reference_norm_r[g], norm_r[g]);
#endif
    }
  }

  if (!validate) {
    printf("Solution does not validate\n");
    exit(EXIT_FAILURE);
  }

  printf("Solution validates\n");

  flops = f_active_points * iterations;
  /* subtract one untimed iteration from refinement 0                            */
  iterations_r[0]--;
  for (g=0; g<4; g++) flops += f_active_points_r * iterations_r[g];
  flops *= (DTYPE) (2*stencil_size+1);
  /* add interpolation flops, if applicable                                      */
  if (r_level>0) {
    /* subtract one interpolation (not timed)                                      */
    num_interpolations--;
    flops += nr_true*(num_interpolations)*3*(nr_true+nr);
  }
  avgtime = stencil_time/iterations;
  printf("Rate (MFlops/s): "FSTR"  Avg time (s): %lf\n",
         1.0E-06 * flops/stencil_time, avgtime);

  exit(EXIT_SUCCESS);
}
