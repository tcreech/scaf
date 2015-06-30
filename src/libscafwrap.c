#include <dlfcn.h>
#include "scaf.h"

/* Used to generate a string to pass to dlsym */
#define __STRINGIFY(X) #X
#define STRINGIFY(X) __STRINGIFY(X)

/* Describe the functions we are wrapping */
#define __scaf_SECTION_START GOMP_parallel_start
#define __scaf_SECTION_START_RET void
#define __scaf_SECTION_START_FORMAL_ARGS void (*fn) (void *), void *data, unsigned num_threads
#define __scaf_SECTION_START_ACTUAL_ARGS fn, data, num_threads
#define __scaf_SECTION_START_ACTUAL_ARGS_1_2 fn, data
#define __scaf_SECTION_START_ACTUAL_ARGS_1 fn
#define __scaf_SECTION_START_ACTUAL_ARGS_2 data
#define __scaf_SECTION_END GOMP_parallel_end
#define __scaf_SECTION_END_RET void
#define __scaf_SECTION_END_FORMAL_ARGS void
#define __scaf_SECTION_END_ACTUAL_ARGS void

/* Local pointers to the actual functions which we still want to eventually
 * call */
static __scaf_SECTION_START_RET (*__real_start)(__scaf_SECTION_START_FORMAL_ARGS) = NULL;
static __scaf_SECTION_END_RET (*__real_end)(__scaf_SECTION_END_FORMAL_ARGS) = NULL;

/* Wrapper around the beginning of a parallel section */
__scaf_SECTION_START_RET
__scaf_SECTION_START( __scaf_SECTION_START_FORMAL_ARGS )
{
   int scaf_num_threads;

   /* Find the real function if not already saved */
   if(!__real_start)
      __real_start = dlsym(RTLD_NEXT, STRINGIFY(__scaf_SECTION_START));

   /* Query SCAF for the current number of threads to use */
   scaf_num_threads = scaf_section_start(__scaf_SECTION_START_ACTUAL_ARGS_1);
   /* Ask SCAF to start an experiment if necessary */
   scaf_gomp_experiment_create(__scaf_SECTION_START_ACTUAL_ARGS_1_2);
   /* Finally, call the real function with the altered number of requested
    * threads */
   return __real_start(__scaf_SECTION_START_ACTUAL_ARGS_1_2, scaf_num_threads);
}

/* Wrapper around the end of a parallel section */
__scaf_SECTION_END_RET
__scaf_SECTION_END( __scaf_SECTION_END_FORMAL_ARGS )
{
   /* Find the real function if not already saved */
   if(!__real_end)
      __real_end = dlsym(RTLD_NEXT, STRINGIFY(__scaf_SECTION_END));

   /* Tell SCAF that the section is over */
   scaf_section_end();
   /* Call the real GOMP function to terminate the section */
   __real_end();
   /* Terminate any experiment */
   scaf_gomp_training_destroy();
}

