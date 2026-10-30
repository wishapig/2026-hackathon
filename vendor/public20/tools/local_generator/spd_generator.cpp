#include "matrix_case_io.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::size_t resolve_thread_count(std::uint32_t n) {
    std::size_t threads = std::thread::hardware_concurrency();
    if (threads == 0) {
        threads = 1;
    }

    if (const char *env = std::getenv("SPD_GENERATOR_THREADS")) {
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

void fill_spd_matrix(const std::vector<double> &m, std::vector<double> &a, std::uint32_t n) {
    const std::size_t matrix_width = static_cast<std::size_t>(n);
    const std::size_t thread_count = resolve_thread_count(n);
    std::atomic<std::uint32_t> next_row{0};
    std::exception_ptr worker_error;
    std::mutex error_mutex;

    auto worker = [&]() {
        try {
            while (true) {
                const std::uint32_t i = next_row.fetch_add(1, std::memory_order_relaxed);
                if (i >= n) {
                    break;
                }

                const std::size_t row_i = static_cast<std::size_t>(i) * matrix_width;
                for (std::uint32_t j = i; j < n; ++j) {
                    const std::size_t row_j = static_cast<std::size_t>(j) * matrix_width;
                    double sum = 0.0;
                    for (std::uint32_t k = 0; k < n; ++k) {
                        sum += m[row_i + k] * m[row_j + k];
                    }
                    if (i == j) {
                        sum += static_cast<double>(n);
                    }
                    a[row_i + j] = sum;
                    if (i != j) {
                        a[row_j + i] = sum;
                    }
                }
            }
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
}

contest::MatrixCase generate_case(std::uint32_t n, std::uint32_t b, std::uint32_t seed) {
    if (n == 0 || b == 0) {
        throw std::runtime_error("n and b must be positive");
    }
    if (b > n) {
        throw std::runtime_error("b must not exceed n");
    }
    if (n % b != 0) {
        throw std::runtime_error(
            "Current local scaffold requires test cases to satisfy n % b == 0");
    }

    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> dist(-1.0, 1.0);

    std::vector<double> m(static_cast<std::size_t>(n) * n);
    std::vector<double> a(static_cast<std::size_t>(n) * n, 0.0);

    for (double &value : m) {
        value = dist(rng);
    }

    fill_spd_matrix(m, a, n);

    contest::MatrixCase item;
    item.n = n;
    item.b = b;
    item.a = std::move(a);
    return item;
}

std::string trim(std::string value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

std::vector<contest::MatrixCase> load_spec_file(const std::string &path) {
    std::ifstream stream(path);
    if (!stream) {
        throw std::runtime_error("Failed to open spec file: " + path);
    }

    std::vector<contest::MatrixCase> cases;
    std::string line;
    std::uint32_t line_no = 0;
    while (std::getline(stream, line)) {
        ++line_no;
        line = trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }

        for (char &ch : line) {
            if (ch == ':' || ch == ',') {
                ch = ' ';
            }
        }

        std::istringstream parser(line);
        std::uint32_t n = 0;
        std::uint32_t b = 0;
        std::uint32_t seed = 1;
        if (!(parser >> n >> b)) {
            throw std::runtime_error("Invalid case spec at line " + std::to_string(line_no));
        }
        if (!(parser >> seed)) {
            seed = line_no;
        }
        cases.push_back(generate_case(n, b, seed));
    }

    if (cases.empty()) {
        throw std::runtime_error("Spec file does not contain any cases");
    }
    return cases;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 5 && argc != 4) {
        std::cerr << "Usage: " << argv[0] << " output.bin n b seed\n";
        std::cerr << "   or: " << argv[0] << " output.bin --spec spec.txt\n";
        return 1;
    }

    try {
        const std::string output_path = argv[1];
        std::vector<contest::MatrixCase> cases;

        if (argc == 4) {
            if (std::string(argv[2]) != "--spec") {
                throw std::runtime_error("Expected --spec when using 3 arguments after output path");
            }
            cases = load_spec_file(argv[3]);
        } else {
            const std::uint32_t n = static_cast<std::uint32_t>(std::stoul(argv[2]));
            const std::uint32_t b = static_cast<std::uint32_t>(std::stoul(argv[3]));
            const std::uint32_t seed = static_cast<std::uint32_t>(std::stoul(argv[4]));
            cases.push_back(generate_case(n, b, seed));
        }

        contest::write_input_cases(output_path, cases);
        return 0;
    } catch (const std::exception &ex) {
        std::cerr << "spd_generator failed: " << ex.what() << "\n";
        return 1;
    }
}
