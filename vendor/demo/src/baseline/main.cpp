#include "matrix_case_io.hpp"

#include <cstdlib>
#include <cstdio>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <time.h>
#include <vector>

namespace contest {
int block_cholesky(const double *A, double *L, int n, int b);
}

namespace {

double monotonic_seconds() {
    timespec ts{};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        throw std::runtime_error("clock_gettime(CLOCK_MONOTONIC) failed");
    }
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1.0e-9;
}

void maybe_write_timing_file(double elapsed_seconds) {
    const char *path = std::getenv("COMPILER2026_TIMING_FILE");
    if (path == nullptr || path[0] == '\0') {
        return;
    }

    FILE *stream = std::fopen(path, "w");
    if (stream == nullptr) {
        throw std::runtime_error(
            "Failed to open COMPILER2026_TIMING_FILE for writing: " + std::string(path));
    }

    if (std::fprintf(stream, "%.17g\n", elapsed_seconds) < 0) {
        std::fclose(stream);
        throw std::runtime_error(
            "Failed to write COMPILER2026_TIMING_FILE: " + std::string(path));
    }

    if (std::fclose(stream) != 0) {
        throw std::runtime_error(
            "Failed to close COMPILER2026_TIMING_FILE: " + std::string(path));
    }
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " input.bin output.bin\n";
        return 1;
    }

    try {
        const auto inputs = contest::read_input_cases(argv[1]);
        std::vector<contest::ResultCase> outputs;
        outputs.reserve(inputs.size());
        double total_compute_seconds = 0.0;

        for (const auto &item : inputs) {
            contest::ResultCase result;
            result.n = item.n;
            result.b = item.b;
            result.l.assign(static_cast<std::size_t>(item.n) * item.n, 0.0);
            const double start = monotonic_seconds();
            contest::block_cholesky(item.a.data(), result.l.data(), static_cast<int>(item.n),
                                    static_cast<int>(item.b));
            const double end = monotonic_seconds();
            total_compute_seconds += (end - start);
            outputs.push_back(std::move(result));
        }

        contest::write_output_cases(argv[2], outputs);
        maybe_write_timing_file(total_compute_seconds);
        return 0;
    } catch (const std::exception &ex) {
        std::cerr << "baseline_serial failed: " << ex.what() << "\n";
        return 1;
    }
}
