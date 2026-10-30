#ifndef COMPILER2026_KERNELS_INTERNAL_H
#define COMPILER2026_KERNELS_INTERNAL_H

extern "C" {
void __official_cholesky_impl(double *A, double *L, int b, int lda);
void __official_trsm_impl(double *A, double *L, double *X, int b, int lda);
void __official_madd_impl(double *A, double *B, double *C, int b, int lda);
}

#endif
