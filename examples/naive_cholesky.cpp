// Scalar left-looking Cholesky expressed directly in ordinary C++ arithmetic.
// This extension does not replace the compulsory official blocked operators.
#include <algorithm>
#include <cmath>
int naive_cholesky(const double *A, double *L, int n) {
    std::copy(A, A + n * n, L);
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < i; ++j) {
            for (int k = 0; k < j; ++k) {
                L[i * n + j] -= L[i * n + k] * L[j * n + k];
            }
            L[i * n + j] /= L[j * n + j];
        }
        for (int k = 0; k < i; ++k) {
            L[i * n + i] -= L[i * n + k] * L[i * n + k];
        }
        L[i * n + i] = std::sqrt(L[i * n + i]);
    }
    for (int row = 0; row < n; ++row) {
        for (int col = row + 1; col < n; ++col) {
            L[row * n + col] = 0.0;
        }
    }
    return 0;
}
