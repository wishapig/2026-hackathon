#include "kernels.h"
#include "kernels_internal.h"

extern "C" void cholesky(double *A, double *L, int b, int lda) {
    __official_cholesky_impl(A, L, b, lda);
}

extern "C" void trsm(double *A, double *L, double *X, int b, int lda) {
    __official_trsm_impl(A, L, X, b, lda);
}

extern "C" void madd(double *A, double *B, double *C, int b, int lda) {
    __official_madd_impl(A, B, C, b, lda);
}
