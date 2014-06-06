#ifndef INTPART_H
#define INTPART_H 1

int intpart_from_floatpart(int n, int* intpart, float* floatpart, int l);
int intpart_from_floatpart_chunked(int n, int *intpart, float* floatpart, int chunksize, int l);

int intpart_equipartition(int n, int* intpart, int l);
int intpart_equipartition_chunked(int n, int* intpart, int chunksize, int l);

#endif

