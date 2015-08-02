#include <intpart.h>
#include <stdlib.h>
#include <stdio.h>
#include <float.h>
#include <assert.h>

/*
 * Convenience function: non-chunked version of intpart_from_floatpart_chunked.
 */
int intpart_from_floatpart(int n, int *intpart, float *floatpart, int l) {
    return intpart_from_floatpart_chunked(n, intpart, floatpart, 1, l);
}

/*
 * Convenience function: non-chunked version of intpart_equipartition_chunked.
 */
int intpart_equipartition(int n, int *intpart, int l) {
    return intpart_equipartition_chunked(n, intpart, 1, l);
}

/*
 * Generate a partitioning which attemps to make partitions as equal as
 * possible.
 */
int intpart_equipartition_chunked(int n, int *intpart, int chunksize, int l) {
    int i, ret;
    float *floatpart = malloc(sizeof(float) * l);
    float each = 1.0 / ((float)l);
    for(i = 0; i < l; i++)
        floatpart[i] = each;
    ret = intpart_from_floatpart_chunked(n, intpart, floatpart, chunksize, l);
    free(floatpart);
    return ret;
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
int intpart_from_floatpart_chunked(int n, int *intpart, float *floatpart,
                                   int chunksize, int l) {
    int i, in, remaining_chunks;
    float *diffs;

    // Do nothing and exit early if some argument doesn't make sense.
    if(n < 1 || intpart == NULL || floatpart == NULL || chunksize < 1 || l < 1)
        return -1;

    // Intermediate "n". This is just the number of chunks given the chunksize.
    remaining_chunks = in = n / chunksize;

    // Do nothing if there is no way to create a partitioning.
    if(remaining_chunks < l)
        return -1;

    // Keep track of the offset due to integerness. (perfect - integer.)
    diffs = malloc(sizeof(float) * l);

    // Do an initial round of assignment, rounding down.
    for(i = 0; i < l; i++) {
        intpart[i] = (floatpart[i] * in);
        remaining_chunks -= intpart[i];
        diffs[i] = (floatpart[i] * in) - (float)(intpart[i]);
    }

    // Distribute remaining chunks to the partitions with the greatest
    // difference from their perfect portion.
    while(remaining_chunks > 0) {
        int maxi = -1;
        float maxdiff = -FLT_MAX;
        // Find the partition with the greatest difference. Our hope is that
        // there are not too many chunks leftover, so this won't have to be done
        // too many times! If we find a partition that is empty, give it
        // something first.
        for(i = 0; i < l; i++) {
            if(diffs[i] > maxdiff) {
                maxi = i;
                maxdiff = diffs[i];
            }
            if(intpart[i] == 0) {
                maxi = i;
                maxdiff = diffs[i];
                break;
            }
        }

        // Distribute a remaining chunk here.
        remaining_chunks--;
        intpart[maxi] = intpart[maxi] + 1;
        diffs[maxi] = (floatpart[maxi] * in) - (float)(intpart[maxi]);
    }

    // It's still possible that there are empty partitions. If so, find them and
    // steal chunks for them from the partitions with the greatest diffs.
    int empty = -1;
    for(i = 0; i < l; i++) {
        if(intpart[i] == 0) {
            empty = i;
            break;
        }
    }
    while(empty >= 0) {
        int maxi = -1;
        float maxdiff = -FLT_MAX;
        // Find the partition with the greatest difference and multiple chunks.
        // There *has* to be one with multiple chunks if we have empty
        // partitions.
        for(i = 0; i < l; i++) {
            if(intpart[i] < 2)
                continue;
            if(diffs[i] > maxdiff) {
                maxi = i;
                maxdiff = diffs[i];
            }
        }
        assert(maxi >= 0);

        // Got one to steal from.
        intpart[maxi]--;
        intpart[empty]++;
        // Update diffs.
        diffs[maxi] = (floatpart[maxi] * in) - (float)(intpart[maxi]);
        diffs[empty] = (floatpart[empty] * in) - (float)(intpart[empty]);

        // Check if there is still some empty partition.
        empty = -1;
        for(i = 0; i < l; i++) {
            if(intpart[i] == 0) {
                empty = i;
                break;
            }
        }
    }

    // Expand the partition back up by the chunking factor.
    for(i = 0; i < l; i++) {
        intpart[i] *= chunksize;
    }

    // Sum intpart to ensure that it is not greater than n.
    int sum = 0;
    for(i = 0; i < l; i++)
        sum += intpart[i];
    assert(sum <= n);

    // Find the min; ensure it is not zero.
    int minp = n;
    for(i = 0; i < l; i++)
        minp = (intpart[i] < minp ? intpart[i] : minp);
    assert(minp > 0);

    // Ensure that we got as close as possible.
    assert((n - sum) <= (n % chunksize));

finish:
    free(diffs);

    return 0;
}

/*
 * Calls intpart_from_floatpart_chunked with floatpart "normalized" using the
 * normalization weight normweight. The floatpart passed to
 * intpart_from_floatpart_chunked will be normalized to sum to one after
 * normweight is added to each floatpart item. For example, Passing
 * normweight=0 has no effect. Passing normweight as a value very large
 * relative to the items in normweight will result in a partitioning
 * approaching equipartitioning.
 */
int intpart_from_floatpart_chunked_normalized(int n, int *intpart,
                                              float *floatpart, int chunksize,
                                              float normweight, int l) {
    float *normfloatpart = malloc(sizeof(float) * l);
    int i;
    float sum = 0;
    for(i = 0; i < l; i++) {
        normfloatpart[i] = floatpart[i] + normweight;
        sum += normfloatpart[i];
    }
    for(i = 0; i < l; i++) {
        normfloatpart[i] /= sum;
    }

    int ret;
    ret =
        intpart_from_floatpart_chunked(n, intpart, normfloatpart, chunksize, l);

finish:
    free(normfloatpart);

    return ret;
}
