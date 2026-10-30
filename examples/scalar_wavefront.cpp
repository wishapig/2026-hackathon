// Supplemental north/west dependency example using generic scalar expressions.
#include <algorithm>
int scalar_wavefront(const double *A, double *L, int n) {
    std::copy(A, A + n * n, L);
    for (int row = 1; row < n; ++row) {
        for (int col = 1; col < n; ++col) {
            L[row * n + col] = A[row * n + col] +
                0.25 * L[(row - 1) * n + col] + 0.5 * L[row * n + col - 1];
        }
    }
    return 0;
}
