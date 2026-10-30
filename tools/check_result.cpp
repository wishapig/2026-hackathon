#include "dag/resources.hpp"
#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
void exact(std::ifstream &f, void *data, std::size_t bytes) {
    if (bytes > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()))
        throw std::runtime_error("payload too large");
    f.read(static_cast<char *>(data), static_cast<std::streamsize>(bytes));
    if (!f)
        throw std::runtime_error("truncated matrix file");
}
std::size_t positive_env(const char *name, std::size_t fallback, std::size_t maximum) {
    const char *text = std::getenv(name);
    if (!text)
        return fallback;
    std::string s(text);
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error(std::string("invalid ") + name);
    std::size_t value = 0;
    for (char c : s) {
        auto d = static_cast<unsigned>(c - '0');
        if (d > maximum || value > (maximum - d) / 10)
            throw std::runtime_error(std::string(name) + " out of range");
        value = value * 10 + d;
    }
    if (!value)
        throw std::runtime_error(std::string(name) + " must be positive");
    return value;
}
void finite(double x, const char *stage) {
    if (!std::isfinite(x))
        throw std::runtime_error(std::string("non-finite ") + stage);
}
double residual(std::uint32_t n, const std::vector<double> &a, const std::vector<double> &l,
                unsigned threads) {
    for (double x : a)
        finite(x, "input element");
    for (double x : l)
        finite(x, "output element");
    for (std::size_t i = 0; i < n; ++i)
        if (!(l[i * n + i] > 0.0))
            throw std::runtime_error("Cholesky diagonal must be strictly positive");
    double max_diff = 0.0, max_a = 0.0;
    std::atomic<std::uint32_t> next{0};
    std::atomic<bool> failed{false};
    std::exception_ptr error;
    std::mutex mutex;
    const auto worker = [&] {
        try {
            double diff_max = 0.0, a_max = 0.0;
            while (!failed.load(std::memory_order_relaxed)) {
                const auto i = next.fetch_add(1, std::memory_order_relaxed);
                if (i >= n)
                    break;
                double diff = 0.0, norm = 0.0;
                for (std::uint32_t j = 0; j < n; ++j) {
                    double value = 0.0;
                    for (std::uint32_t k = 0; k <= std::min(i, j); ++k) {
                        double product = l[static_cast<std::size_t>(i) * n + k] *
                                         l[static_cast<std::size_t>(j) * n + k];
                        finite(product, "LLT product");
                        value += product;
                        finite(value, "LLT sum");
                    }
                    const double difference = a[static_cast<std::size_t>(i) * n + j] - value;
                    finite(difference, "residual difference");
                    diff += std::abs(difference);
                    norm += std::abs(a[static_cast<std::size_t>(i) * n + j]);
                    finite(diff, "difference row norm");
                    finite(norm, "input row norm");
                }
                diff_max = std::max(diff_max, diff);
                a_max = std::max(a_max, norm);
            }
            std::lock_guard<std::mutex> lock(mutex);
            max_diff = std::max(max_diff, diff_max);
            max_a = std::max(max_a, a_max);
        } catch (...) {
            failed.store(true, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lock(mutex);
            if (!error)
                error = std::current_exception();
        }
    };
    std::vector<std::thread> pool;
    try {
        for (unsigned t = 1; t < threads; ++t)
            pool.emplace_back(worker);
        worker();
    } catch (...) {
        failed.store(true);
        for (auto &t : pool)
            t.join();
        throw;
    }
    for (auto &t : pool)
        t.join();
    if (error)
        std::rethrow_exception(error);
    if (!(max_a > 0.0))
        throw std::runtime_error("input norm must be positive");
    const double scaled = max_a * static_cast<double>(n);
    finite(scaled, "scaled norm");
    const double denominator = scaled * DBL_EPSILON;
    finite(denominator, "residual denominator");
    if (!(denominator > 0.0))
        throw std::runtime_error("residual denominator underflow");
    const double result = max_diff / denominator;
    finite(result, "scaled residual");
    return result;
}
void eof(std::ifstream &f) {
    char c;
    if (f.get(c))
        throw std::runtime_error("trailing matrix data");
    if (!f.eof())
        throw std::runtime_error("matrix read failure");
}
} // namespace
int main(int argc, char **argv) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " input.bin output.bin\n";
        return 1;
    }
    try {
        static_assert(sizeof(double) == 8 && sizeof(std::uint32_t) == 4,
                      "unsupported binary format");
        const auto requested = positive_env("DAG_CHECK_MEMORY_MB", 0,
                                            std::numeric_limits<std::size_t>::max() / (1024 * 1024)) *
                               1024 * 1024;
        const auto budget = dag::available_memory_budget(requested);
        const auto threads = positive_env("DAG_CHECK_THREADS", 1, 1024);
        const char *format_env = std::getenv("DAG_IO_FORMAT");
        const std::string format = format_env ? format_env : "public";
        if (format != "public" && format != "original")
            throw std::runtime_error("DAG_IO_FORMAT must be public or original");
        const bool has_block_size = format == "public";
        const std::uintmax_t header_bytes = has_block_size ? 8 : 4;
        std::ifstream input(argv[1], std::ios::binary), output(argv[2], std::ios::binary);
        if (!input || !output)
            throw std::runtime_error("cannot open matrix files");
        std::uint32_t count = 0, results = 0;
        exact(input, &count, 4);
        exact(output, &results, 4);
        if (count != results)
            throw std::runtime_error("case counts do not match");
        const auto in_size = std::filesystem::file_size(argv[1]),
                   out_size = std::filesystem::file_size(argv[2]);
        if (in_size < 4 || out_size < 4 ||
            count > (in_size - 4) / (header_bytes + sizeof(double)) ||
            count > (out_size - 4) / (header_bytes + sizeof(double)))
            throw std::runtime_error("impossible case count");
        bool all_passed = true;
        std::uintmax_t consumed = 4;
        for (std::uint32_t index = 0; index < count; ++index) {
            std::uint32_t n = 0, b = 0, rn = 0, rb = 0;
            exact(input, &n, 4);
            exact(output, &rn, 4);
            if (has_block_size) {
                exact(input, &b, 4);
                exact(output, &rb, 4);
            }
            if (n != rn || b != rb)
                throw std::runtime_error("result n/b does not match input");
            if (!n || (has_block_size && (!b || b > n || n % b)))
                throw std::runtime_error("invalid case dimensions");
            const auto elements = static_cast<std::uint64_t>(n) * n;
            consumed += header_bytes;
            if (consumed > in_size || consumed > out_size ||
                elements > (in_size - consumed) / sizeof(double) ||
                elements > (out_size - consumed) / sizeof(double))
                throw std::runtime_error("truncated matrix payload");
            consumed += elements * sizeof(double);
            if (elements > budget / sizeof(double) / 2)
                throw std::runtime_error("matrix pair exceeds checker memory budget");
            std::vector<double> a(static_cast<std::size_t>(elements)),
                l(static_cast<std::size_t>(elements));
            exact(input, a.data(), a.size() * sizeof(double));
            exact(output, l.data(), l.size() * sizeof(double));
            const double r =
                residual(n, a, l, static_cast<unsigned>(std::min<std::size_t>(threads, n)));
            const bool passed = r < 100.0;
            all_passed = all_passed && passed;
            std::cout << "case=" << index << " n=" << n;
            if (has_block_size)
                std::cout << " b=" << b;
            std::cout << " scaled_residual=" << std::setprecision(17) << r
                      << " status=" << (passed ? "PASS" : "FAIL") << '\n';
        }
        eof(input);
        eof(output);
        return all_passed ? 0 : 1;
    } catch (const std::exception &error) {
        std::cerr << "dag_check failed: " << error.what() << '\n';
        return 1;
    }
}
