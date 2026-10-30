// A different dependency structure: each interior cell uses north and west.
// combine's numeric implementation is an extension, not an official judge kernel.
#include <algorithm>
int wavefront(const double *A, double *L, int n, int b) {
    std::copy(A, A + n * n, L);
    for (int row = 1; row < n; ++row) {
        for (int col = 1; col < n; ++col) {
            combine(&L[(row - 1) * n + col], &L[row * n + col - 1],
                    &L[row * n + col], n);
        }
    }
    return 0;
}
