#include "matrix_case_io.hpp"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

std::size_t resolve_thread_count(std::uint32_t n) {
    std::size_t threads = std::thread::hardware_concurrency();
    if (threads == 0) {
        threads = 1;
    }

    if (const char *env = std::getenv("VERIFIER_THREADS")) {
        char *end = nullptr;
        const unsigned long configured = std::strtoul(env, &end, 10);
        if (end != env && *end == '\0' && configured > 0) {
            threads = static_cast<std::size_t>(configured);
        }
    }

    // On the current 128-core Kunpeng-920 host, measurements show that
    // 96 threads is slightly more efficient than saturating all 128 cores
    // for both generation and verification workloads.
    threads = std::min<std::size_t>(threads, 96);
    return std::max<std::size_t>(1, std::min<std::size_t>(threads, n));
}

double calculate_scaled_residual(std::uint32_t n, const std::vector<double> &A,
                                 const std::vector<double> &L) {
    double max_diff = 0.0;
    double max_A = 0.0;
    const std::size_t matrix_width = static_cast<std::size_t>(n);
    const std::size_t thread_count = resolve_thread_count(n);
    std::atomic<std::uint32_t> next_row{0};
    std::exception_ptr worker_error;
    std::mutex error_mutex;

    auto worker = [&]() {
        try {
            double local_max_diff = 0.0;
            double local_max_A = 0.0;

            while (true) {
                const std::uint32_t i = next_row.fetch_add(1, std::memory_order_relaxed);
                if (i >= n) {
                    break;
                }

                double row_sum_diff = 0.0;
                double row_sum_A = 0.0;
                const std::size_t row_i = static_cast<std::size_t>(i) * matrix_width;
                for (std::uint32_t j = 0; j < n; ++j) {
                    double sum_LLT = 0.0;
                    const auto limit = std::min(i, j);
                    const std::size_t row_j = static_cast<std::size_t>(j) * matrix_width;
                    for (std::uint32_t k = 0; k <= limit; ++k) {
                        sum_LLT += L[row_i + k] * L[row_j + k];
                    }
                    row_sum_diff += std::abs(A[row_i + j] - sum_LLT);
                    row_sum_A += std::abs(A[row_i + j]);
                }
                local_max_diff = std::max(local_max_diff, row_sum_diff);
                local_max_A = std::max(local_max_A, row_sum_A);
            }

            std::lock_guard<std::mutex> lock(error_mutex);
            max_diff = std::max(max_diff, local_max_diff);
            max_A = std::max(max_A, local_max_A);
        } catch (...) {
            std::lock_guard<std::mutex> lock(error_mutex);
            if (!worker_error) {
                worker_error = std::current_exception();
            }
            next_row.store(n, std::memory_order_relaxed);
        }
    };

    if (thread_count == 1) {
        worker();
    } else {
        std::vector<std::thread> workers;
        workers.reserve(thread_count);
        for (std::size_t idx = 0; idx < thread_count; ++idx) {
            workers.emplace_back(worker);
        }
        for (auto &thread : workers) {
            thread.join();
        }
    }

    if (worker_error) {
        std::rethrow_exception(worker_error);
    }

    if (max_A == 0.0) {
        return (max_diff == 0.0) ? 0.0 : DBL_MAX;
    }

    return max_diff / (max_A * static_cast<double>(n) * DBL_EPSILON);
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " input.bin output.bin\n";
        return 1;
    }

    try {
        const auto inputs = contest::read_input_cases(argv[1]);
        const auto outputs = contest::read_output_cases(argv[2]);

        if (inputs.size() != outputs.size()) {
            throw std::runtime_error("Input and output case counts do not match");
        }

        bool all_passed = true;
        for (std::size_t index = 0; index < inputs.size(); ++index) {
            const auto &input = inputs[index];
            const auto &output = outputs[index];

            if (input.n != output.n || input.b != output.b) {
                throw std::runtime_error("Result metadata does not match the input case");
            }

            const double residual =
                calculate_scaled_residual(input.n, input.a, output.l);
            const bool passed = residual < 100.0;
            all_passed &= passed;

            std::cout << "case=" << index << " n=" << input.n << " b=" << input.b
                      << " scaled_residual=" << residual
                      << " status=" << (passed ? "PASS" : "FAIL") << "\n";
        }

        return all_passed ? 0 : 1;
    } catch (const std::exception &ex) {
        std::cerr << "Verifier failed: " << ex.what() << "\n";
        return 1;
    }
}
