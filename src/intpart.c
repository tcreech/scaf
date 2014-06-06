#include <intpart.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

/*
 * Convenience function: non-chunked version of intpart_from_floatpart_chunked.
 */
int intpart_from_floatpart(int n, int* intpart, float* floatpart, int l){
   return intpart_from_floatpart_chunked(n, intpart, floatpart, 1, l);
}

/*
 * Convenience function: non-chunked version of intpart_equipartition_chunked.
 */
int intpart_equipartition(int n, int* intpart, int l){
   return intpart_equipartition_chunked(n, intpart, 1, l);
}

/*
 * Generate a partitioning which attemps to make partitions as equal as
 * possible.
 */
int intpart_equipartition_chunked(int n, int* intpart, int chunksize, int l){
   int i;
   float *floatpart = malloc(sizeof(float) * l);
   float each = 1.0 / ((float)l);
   for(i=0; i<l; i++)
      floatpart[i] = each;
   return intpart_from_floatpart_chunked(n, intpart, floatpart, chunksize, l);
}

/*
 * Given integer n, find integer partitions which closely resemble the float
 * partitioning floatpart, which is assumed to sum to 1. The number of
 * partitions is specified as l. The resulting partitions will be written to
 * intpart, which is assumed to be memory pointing to l ints' worth of space.
 * The partitions written to intpart will be multiples of chunksize.
 *
 * Generally, the goals for the partition are:
 * 1) If no true partition is possible due to chunking, prefer summing to less
 *    than n.
 * 2) Avoid empty partitions.
 *
 * NOTE: If floatpart sums to greater than 1, then intpart will sum to
 * greater than n. For now, I want to ensure that this never happens, so this
 * code will hit an assert if intpart ever sums to greater than 1.
 *
 * NOTE: intpart will NOT sum to n if chunksize does not divide n. (It
 * will specifically sum to /less/ than n.)
 */
int intpart_from_floatpart_chunked(int n, int *intpart, float* floatpart, int chunksize, int l){
   int i, in, remaining_chunks;
   float *diffs;

   // Do nothing and exit early if some argument doesn't make sense.
   if(n < 1 || intpart == NULL || floatpart == NULL || chunksize < 1 || l < 1)
      return -1;

   // Intermediate "n". This is just the number of chunks given the chunksize.
   remaining_chunks = in = n / chunksize;

   // Keep track of the offset due to integerness. (perfect - integer.)
   diffs = malloc(sizeof(float) * l);

   // Do an initial round of assignment, rounding down.
   for(i=0; i<l; i++){
      intpart[i] = (floatpart[i] * in);
      remaining_chunks -= intpart[i];
      diffs[i] = (floatpart[i]*in) - (float)(intpart[i]);
   }

   //printf("%d/%d chunks remain.\n", remaining_chunks, in);
   // Distribute remaining chunks to the partitions with the greatest
   // difference from their perfect portion.
   while(remaining_chunks > 0){
      int maxi = 0;
      float maxdiff = 0;
      // Find the partition with the greatest difference. Our hope is that
      // there are not too many chunks leftover, so this won't have to be done
      // too many times! If we find a partition that is empty, give it
      // something first.
      for(i=0; i<l; i++){
         //printf("diffs[%d] = %f (%d from %f)\n", i, diffs[i], intpart[i], (floatpart[i]*in));
         if(diffs[i] > maxdiff){
            maxi = i;
            maxdiff = diffs[i];
         }
         if(intpart[i]==0){
            maxi = i;
            maxdiff = diffs[i];
            break;
         }
      }
      //printf("partition with greatest difference is at %d (%f).\n\n", maxi, maxdiff);

      // Distribute a remaining chunk here.
      remaining_chunks--;
      intpart[maxi] = intpart[maxi] + 1;
      diffs[maxi] = (floatpart[maxi]*in) - (float)(intpart[maxi]);

      //printf("%d/%d chunks remain.\n", remaining_chunks, in);
      assert(remaining_chunks >= 0);
   }

   //TODO: remove me. For debug only.
   for(i=0; i<l; i++){
      //printf("diffs[%d] = %f (%d from %f)\n", i, diffs[i], intpart[i], (floatpart[i]*in));
   }

   // Expand the partition back up by the chunking factor.
   for(i=0; i<l; i++){
      intpart[i] *= chunksize;
   }

   // Sum intpart to ensure that it is not greater than n.
   int sum = 0;
   for(i=0; i<l; i++)
      sum += intpart[i];
   assert(sum <= n);


   // Find the min; ensure it is not zero.
   int minp = n;
   for(i=0; i<l; i++)
      minp = (intpart[i] < minp ? intpart[i] : minp);
   assert(minp > 0);

   // Ensure that we got as close as possible.
   assert((n - sum) <= (n % chunksize));

finish:
   free(diffs);

   return 0;
}

