//
//  Demo scafd client
//
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <zmq.h>
#ifndef __clang__
#include <omp.h>
#endif
#include "scaf.h"

#define N 4096*1024
#define REQUESTS 1024

int main (void)
{
   double *data = malloc(sizeof(double)*N);
   int request_nbr;
   for (request_nbr = 0; request_nbr != REQUESTS; request_nbr++) {
      int j;

      int threads = scaf_section_start();
      printf("Running on %d threads.\n", threads);

      // BEGIN PARALLEL LOOP //
#ifndef __clang__
      omp_set_num_threads(threads);
#endif
#pragma omp parallel for
      for(j=0; j<N; j++){
         data[j] = sin(2*3.1459*j) * cos(j/3);
      }
      scaf_section_end();
      // END PARALLEL LOOP //

   }

   scaf_retire();
   return 0;
}

