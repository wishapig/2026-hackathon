#include "kernels_internal.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace {

inline double &elem(double *base, int lda, int row, int col) {
    return base[row * lda + col];
}

inline const double &elem(const double *base, int lda, int row, int col) {
    return base[row * lda + col];
}

void zero_upper_triangle(double *L, int b, int lda) {
    for (int row = 0; row < b; ++row) {
        for (int col = row + 1; col < b; ++col) {
            elem(L, lda, row, col) = 0.0;
        }
    }
}

}  // namespace

extern "C" void __official_cholesky_impl(double *A, double *L, int b, int lda) {
    for (int j = 0; j < b; ++j) {
        double diag_sum = 0.0;
        for (int k = 0; k < j; ++k) {
            const double value = elem(L, lda, j, k);
            diag_sum += value * value;
        }

        const double pivot = elem(A, lda, j, j) - diag_sum;
        if (pivot <= 0.0) {
            throw std::runtime_error("Input block is not SPD");
        }
        elem(L, lda, j, j) = std::sqrt(pivot);

        for (int i = j + 1; i < b; ++i) {
            double sum = 0.0;
            for (int k = 0; k < j; ++k) {
                sum += elem(L, lda, i, k) * elem(L, lda, j, k);
            }
            elem(L, lda, i, j) = (elem(A, lda, i, j) - sum) / elem(L, lda, j, j);
        }
    }
    zero_upper_triangle(L, b, lda);
}

extern "C" void __official_trsm_impl(double *A, double *L, double *X, int b, int lda) {
    std::vector<double> scratch(static_cast<std::size_t>(b) * b, 0.0);

    for (int row = 0; row < b; ++row) {
        for (int col = 0; col < b; ++col) {
            scratch[static_cast<std::size_t>(row) * b + col] = elem(A, lda, row, col);
        }
    }

    for (int row = 0; row < b; ++row) {
        for (int col = 0; col < b; ++col) {
            double value = scratch[static_cast<std::size_t>(row) * b + col];
            for (int k = 0; k < col; ++k) {
                value -= elem(X, lda, row, k) * elem(L, lda, col, k);
            }
            elem(X, lda, row, col) = value / elem(L, lda, col, col);
        }
    }
}

extern "C" void __official_madd_impl(double *A, double *B, double *C, int b, int lda) {
    for (int row = 0; row < b; ++row) {
        for (int col = 0; col < b; ++col) {
            double delta = 0.0;
            for (int k = 0; k < b; ++k) {
                delta += elem(A, lda, row, k) * elem(B, lda, col, k);
            }
            elem(C, lda, row, col) -= delta;
        }
    }
}
