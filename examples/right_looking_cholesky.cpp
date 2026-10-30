// Supplemental scalar algorithm, not a replacement for official blocked kernels.
#include <algorithm>
#include <cmath>
int right_looking_cholesky(const double *A, double *L, int n) {
    std::copy(A, A + n * n, L);
    for (int k = 0; k < n; ++k) {
        L[k * n + k] = std::sqrt(L[k * n + k]);
        for (int i = k + 1; i < n; ++i) {
            L[i * n + k] /= L[k * n + k];
        }
        for (int i = k + 1; i < n; ++i) {
            for (int j = k + 1; j <= i; ++j) {
                L[i * n + j] -= L[i * n + k] * L[j * n + k];
            }
        }
    }
    for (int row = 0; row < n; ++row) {
        for (int col = row + 1; col < n; ++col) {
            L[row * n + col] = 0.0;
        }
    }
    return 0;
}
