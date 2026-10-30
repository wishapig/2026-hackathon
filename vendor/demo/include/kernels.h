#ifndef COMPILER2026_KERNELS_H
#define COMPILER2026_KERNELS_H

#ifdef __cplusplus
extern "C" {
#endif

void cholesky(double *A, double *L, int b, int lda);
void trsm(double *A, double *L, double *X, int b, int lda);
void madd(double *A, double *B, double *C, int b, int lda);

#ifdef __cplusplus
}
#endif

#endif
