#ifndef COMPILER2026_MATRIX_CASE_IO_HPP
#define COMPILER2026_MATRIX_CASE_IO_HPP

#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace contest {

struct MatrixCase {
    std::uint32_t n = 0;
    std::uint32_t b = 0;
    std::vector<double> a;
};

struct ResultCase {
    std::uint32_t n = 0;
    std::uint32_t b = 0;
    std::vector<double> l;
};

template <typename T>
inline void read_exact(std::ifstream &stream, T *dst, std::size_t bytes, const std::string &what) {
    stream.read(reinterpret_cast<char *>(dst), static_cast<std::streamsize>(bytes));
    if (!stream) {
        throw std::runtime_error("Failed to read " + what);
    }
}

template <typename T>
inline void write_exact(std::ofstream &stream, const T *src, std::size_t bytes, const std::string &what) {
    stream.write(reinterpret_cast<const char *>(src), static_cast<std::streamsize>(bytes));
    if (!stream) {
        throw std::runtime_error("Failed to write " + what);
    }
}

inline std::vector<MatrixCase> read_input_cases(const std::string &path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Failed to open input file: " + path);
    }

    std::uint32_t count = 0;
    read_exact(stream, &count, sizeof(count), "case count");

    std::vector<MatrixCase> cases;
    cases.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        MatrixCase item;
        read_exact(stream, &item.n, sizeof(item.n), "matrix dimension");
        read_exact(stream, &item.b, sizeof(item.b), "block size");
        item.a.resize(static_cast<std::size_t>(item.n) * item.n);
        read_exact(stream, item.a.data(), item.a.size() * sizeof(double), "matrix payload");
        cases.push_back(std::move(item));
    }
    return cases;
}

inline std::vector<ResultCase> read_output_cases(const std::string &path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Failed to open output file: " + path);
    }

    std::uint32_t count = 0;
    read_exact(stream, &count, sizeof(count), "result count");

    std::vector<ResultCase> cases;
    cases.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        ResultCase item;
        read_exact(stream, &item.n, sizeof(item.n), "result matrix dimension");
        read_exact(stream, &item.b, sizeof(item.b), "result block size");
        item.l.resize(static_cast<std::size_t>(item.n) * item.n);
        read_exact(stream, item.l.data(), item.l.size() * sizeof(double), "result payload");
        cases.push_back(std::move(item));
    }
    return cases;
}

inline void write_output_cases(const std::string &path, const std::vector<ResultCase> &cases) {
    std::ofstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Failed to open output file for writing: " + path);
    }

    const auto count = static_cast<std::uint32_t>(cases.size());
    write_exact(stream, &count, sizeof(count), "result count");
    for (const auto &item : cases) {
        write_exact(stream, &item.n, sizeof(item.n), "result matrix dimension");
        write_exact(stream, &item.b, sizeof(item.b), "result block size");
        write_exact(stream, item.l.data(), item.l.size() * sizeof(double), "result payload");
    }
}

inline void write_input_cases(const std::string &path, const std::vector<MatrixCase> &cases) {
    std::ofstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Failed to open input file for writing: " + path);
    }

    const auto count = static_cast<std::uint32_t>(cases.size());
    write_exact(stream, &count, sizeof(count), "case count");
    for (const auto &item : cases) {
        write_exact(stream, &item.n, sizeof(item.n), "matrix dimension");
        write_exact(stream, &item.b, sizeof(item.b), "block size");
        write_exact(stream, item.a.data(), item.a.size() * sizeof(double), "matrix payload");
    }
}

}  // namespace contest

#endif
